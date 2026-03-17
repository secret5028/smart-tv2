#!/usr/bin/env python3
"""TCP MJPEG/PCM stream server for ESP32 TV player."""

import argparse
import logging
import os
import select
import socket
import struct
import subprocess
import threading
import time
from collections import deque
from typing import Optional


CHANNELS = {
    "ytn": "https://www.youtube.com/channel/UCk0QSdHCADlHAm5SHSZ_4eA/live",
    "mbc": "https://www.youtube.com/channel/UCF2GJqHCMuFmHsBgGDVbOEQ/live",
    "kbs": "https://www.youtube.com/channel/UCcQTRi69dsVYHN3exePtZ1A/live",
}

HOST = "0.0.0.0"
WIDTH = 280
HEIGHT = 240
FPS = 15
FRAME_INTERVAL = 1.0 / FPS
AUDIO_RATE = 16000
AUDIO_CHUNK = round(AUDIO_RATE / FPS)
JPEG_QUALITY = 12
READ_CHUNK = 4096
PACKET_MAGIC = b"\xAA\xBB"
BUFFER_SECONDS = 10.0
MAX_BUFFER_SECONDS = 14.0

LOG = logging.getLogger("tv_server")


def resolve_stream_url(source: str) -> str:
    if "youtube.com" not in source and "youtu.be" not in source:
        return source

    cmd = [
        "yt-dlp",
        "--no-warnings",
        "--no-playlist",
        "-g",
        source,
    ]
    LOG.info("resolving stream URL with yt-dlp: %s", source)
    result = subprocess.run(
        cmd,
        check=True,
        capture_output=True,
        text=True,
    )
    for line in result.stdout.splitlines():
        line = line.strip()
        if line:
            LOG.info("resolved direct stream URL")
            return line
    raise RuntimeError("yt-dlp did not return a stream URL")


class ClientManager:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._clients = []

    def add(self, conn: socket.socket) -> None:
        with self._lock:
            self._clients.append(conn)
            total = len(self._clients)
        try:
            peer = conn.getpeername()
        except OSError:
            peer = "<unknown>"
        LOG.info("client connected: %s (total=%d)", peer, total)

    def broadcast(self, data: bytes) -> None:
        dead = []
        with self._lock:
            clients = list(self._clients)

        for conn in clients:
            try:
                conn.sendall(data)
            except OSError:
                dead.append(conn)

        if not dead:
            return

        with self._lock:
            for conn in dead:
                try:
                    conn.close()
                except OSError:
                    pass
                if conn in self._clients:
                    self._clients.remove(conn)
            total = len(self._clients)
        LOG.info("removed %d dead client(s) (total=%d)", len(dead), total)

    def count(self) -> int:
        with self._lock:
            return len(self._clients)


class MjpegReader:
    def __init__(self, fd: int) -> None:
        self._fd = fd
        self._buf = bytearray()

    def read_frame(self) -> Optional[bytes]:
        while True:
            soi = self._buf.find(b"\xff\xd8")
            if soi < 0:
                chunk = os.read(self._fd, READ_CHUNK)
                if not chunk:
                    return None
                self._buf.extend(chunk)
                continue

            if soi > 0:
                del self._buf[:soi]

            eoi = self._buf.find(b"\xff\xd9", 2)
            while eoi < 0:
                chunk = os.read(self._fd, READ_CHUNK)
                if not chunk:
                    return None
                self._buf.extend(chunk)
                eoi = self._buf.find(b"\xff\xd9", 2)

            frame_end = eoi + 2
            frame = bytes(self._buf[:frame_end])
            del self._buf[:frame_end]
            return frame


class FFmpegPipeline:
    def __init__(self, source: str, verbose: bool = False) -> None:
        self._source = source
        self._verbose = verbose
        self._proc = None
        self.video_fd = None
        self.audio_fd = None

    def start(self) -> None:
        stream_url = resolve_stream_url(self._source)
        video_r, video_w = os.pipe()
        audio_r, audio_w = os.pipe()

        vf = (
            "scale={w}:{h}:force_original_aspect_ratio=decrease,"
            "pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:black,"
            "fps={fps}"
        ).format(w=WIDTH, h=HEIGHT, fps=FPS)

        cmd = [
            "ffmpeg",
            "-loglevel",
            "info" if self._verbose else "warning",
            "-reconnect",
            "1",
            "-reconnect_streamed",
            "1",
            "-reconnect_delay_max",
            "5",
            "-i",
            stream_url,
            "-map",
            "0:v:0",
            "-an",
            "-vf",
            vf,
            "-f",
            "image2pipe",
            "-vcodec",
            "mjpeg",
            "-q:v",
            str(JPEG_QUALITY),
            "-pix_fmt",
            "yuvj420p",
            "pipe:%d" % video_w,
            "-map",
            "0:a:0?",
            "-vn",
            "-ac",
            "1",
            "-ar",
            str(AUDIO_RATE),
            "-f",
            "u8",
            "-acodec",
            "pcm_u8",
            "pipe:%d" % audio_w,
        ]

        try:
            self._proc = subprocess.Popen(
                cmd,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=None if self._verbose else subprocess.DEVNULL,
                pass_fds=(video_w, audio_w),
                bufsize=0,
            )
        except Exception:
            os.close(video_r)
            os.close(video_w)
            os.close(audio_r)
            os.close(audio_w)
            raise

        os.close(video_w)
        os.close(audio_w)
        self.video_fd = video_r
        self.audio_fd = audio_r
        LOG.info("ffmpeg started (pid=%d)", self._proc.pid)

    def is_alive(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def stop(self) -> None:
        for fd in (self.video_fd, self.audio_fd):
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
        self.video_fd = None
        self.audio_fd = None

        if self._proc is not None and self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait(timeout=3)

        if self._proc is not None:
            LOG.info("ffmpeg stopped")
        self._proc = None


def build_packet(jpeg_bytes: bytes, audio_bytes: bytes) -> bytes:
    return b"".join(
        (
            PACKET_MAGIC,
            struct.pack("<I", len(jpeg_bytes)),
            jpeg_bytes,
            struct.pack("<I", len(audio_bytes)),
            audio_bytes,
        )
    )


def fill_audio_buffer(audio_fd: int, audio_buffer: bytearray) -> None:
    while True:
        readable, _, _ = select.select([audio_fd], [], [], 0)
        if not readable:
            return
        chunk = os.read(audio_fd, READ_CHUNK)
        if not chunk:
            return
        audio_buffer.extend(chunk)


def take_audio_chunk(audio_buffer: bytearray) -> bytes:
    if len(audio_buffer) >= AUDIO_CHUNK:
        chunk = bytes(audio_buffer[:AUDIO_CHUNK])
        del audio_buffer[:AUDIO_CHUNK]
        return chunk

    chunk = bytes(audio_buffer)
    audio_buffer.clear()
    return chunk + (b"\x80" * (AUDIO_CHUNK - len(chunk)))


def accept_loop(server: socket.socket, clients: ClientManager) -> None:
    while True:
        try:
            conn, _ = server.accept()
        except OSError:
            return
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        clients.add(conn)


def stream_loop(stream_url: str, clients: ClientManager, verbose: bool) -> None:
    while True:
        pipeline = FFmpegPipeline(stream_url, verbose=verbose)
        try:
            pipeline.start()
            assert pipeline.video_fd is not None
            assert pipeline.audio_fd is not None

            video_reader = MjpegReader(pipeline.video_fd)
            audio_buffer = bytearray()
            packet_buffer = deque()
            source_time = 0.0
            next_frame_at = time.monotonic()
            buffering = True

            while pipeline.is_alive():
                fill_audio_buffer(pipeline.audio_fd, audio_buffer)
                jpeg_bytes = video_reader.read_frame()
                if jpeg_bytes is None:
                    LOG.warning("video pipe ended")
                    break

                fill_audio_buffer(pipeline.audio_fd, audio_buffer)
                audio_bytes = take_audio_chunk(audio_buffer)
                packet_buffer.append((source_time, build_packet(jpeg_bytes, audio_bytes)))
                source_time += FRAME_INTERVAL

                if buffering and packet_buffer:
                    buffered_seconds = source_time - packet_buffer[0][0]
                    if buffered_seconds >= BUFFER_SECONDS:
                        buffering = False
                        next_frame_at = time.monotonic()
                        LOG.info("buffer primed: %.2fs", buffered_seconds)

                while packet_buffer:
                    buffered_seconds = source_time - packet_buffer[0][0]
                    if buffered_seconds <= MAX_BUFFER_SECONDS:
                        break
                    packet_buffer.popleft()
                    buffering = False
                    LOG.warning(
                        "dropping buffered frame to cap drift (buffer=%.2fs queued=%d)",
                        buffered_seconds,
                        len(packet_buffer),
                    )

                if buffering:
                    continue

                now = time.monotonic()
                delay = next_frame_at - now
                if delay > 0:
                    time.sleep(delay)

                if clients.count() > 0 and packet_buffer:
                    _, packet = packet_buffer.popleft()
                    clients.broadcast(packet)
                    LOG.debug(
                        "broadcast buffered frame clients=%d queued=%d",
                        clients.count(),
                        len(packet_buffer),
                    )
                elif packet_buffer:
                    packet_buffer.popleft()

                next_frame_at += FRAME_INTERVAL
                now = time.monotonic()
                if next_frame_at < now - FRAME_INTERVAL:
                    next_frame_at = now + FRAME_INTERVAL

        except KeyboardInterrupt:
            raise
        except Exception as exc:
            LOG.exception("stream loop error: %s", exc)
        finally:
            pipeline.stop()

        LOG.warning("restarting ffmpeg in 3 seconds")
        time.sleep(3)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Pull a live stream with FFmpeg and push MJPEG+PCM packets to ESP32 clients."
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--url", help="direct stream URL")
    group.add_argument("--channel", choices=sorted(CHANNELS), help="built-in channel name")
    parser.add_argument("--host", default=HOST, help="listen host (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=9000, help="listen port (default: 9000)")
    parser.add_argument("--verbose", action="store_true", help="enable verbose logging")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    stream_url = args.url or CHANNELS[args.channel]
    LOG.info("stream source: %s", stream_url)
    LOG.info(
        "video=%dx%d@%dfps audio=%dHz pcm_u8 chunk=%d",
        WIDTH,
        HEIGHT,
        FPS,
        AUDIO_RATE,
        AUDIO_CHUNK,
    )

    clients = ClientManager()
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.port))
    server.listen()
    LOG.info("listening on %s:%d", args.host, args.port)

    thread = threading.Thread(target=accept_loop, args=(server, clients), daemon=True)
    thread.start()

    try:
        stream_loop(stream_url, clients, args.verbose)
    except KeyboardInterrupt:
        LOG.info("shutting down")
    finally:
        try:
            server.close()
        except OSError:
            pass


if __name__ == "__main__":
    main()
