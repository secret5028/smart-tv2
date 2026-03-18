#!/usr/bin/env python3
"""TCP MJPEG/PCM stream server for ESP32 TV player."""

from __future__ import annotations

import argparse
import json
import logging
import os
import queue
import select
import socket
import struct
import subprocess
import threading
import time
from io import BytesIO
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional

try:
    from PIL import Image
except ImportError:
    Image = None


CHANNELS: dict[str, dict[str, str]] = {
    "kbs": {"name": "KBS News24", "type": "direct", "url": "https://news24.gscdn.kbs.co.kr/news24-02/news24-02_hd.m3u8?Policy=eyJTdGF0ZW1lbnQiOlt7IlJlc291cmNlIjoiaHR0cHM6Ly9uZXdzMjQuZ3NjZG4ua2JzLmNvLmtyL25ld3MyNC0wMi8qIiwiQ29uZGl0aW9uIjp7IkRhdGVMZXNzVGhhbiI6eyJBV1M6RXBvY2hUaW1lIjoxNzczOTk5MzYwfX19XX0_&Key-Pair-Id=APKAICDSGT3Y7IXGJ3TA&Signature=BdRcWnCw-Kqs6aby5ETrskltakLtSXlMN~3I6ryM32VKbXdfnzptoOFlNA7cKRWrLE4Z-49BxyL6hnWJDCSlHlEqLk2bNx0UnOpS3OWw~T6dy9lRC58xGVv~H7QxJtRtLy0xfgHt5iNKCFLWfVzhV7xPqFcbdDX67HRi8kRhEPDApQSqs1KSVemBTA2LcJZWch2aFvPpK0qfhLO2JjxEewgpXF3E7a8FwHp49bgBqWfl3LFZeUGN4c3JkedN0Rcjy~bWgvG~FtmCmX543CZSC-1FuSDhiArP8bjFM-Js-2cOODManD0dYzMY1TxB~EZNPm9ucU9qiy7h-NGRXV-0pw__"},
    "ytn": {"name": "YTN", "type": "youtube", "url": "https://www.youtube.com/@ytnnews24/live"},
    "sbsnews": {"name": "SBS News", "type": "youtube", "url": "https://www.youtube.com/@SBSnews8/live"},
    "kbsworld": {"name": "KBS World", "type": "youtube", "url": "https://www.youtube.com/@KBSWorldTV/live"},
    "jtbc": {"name": "JTBC News", "type": "youtube", "url": "https://www.youtube.com/@jtbc_news/live"},
    "mbn": {"name": "MBN", "type": "youtube", "url": "https://www.youtube.com/@MBN/live"},
    "goodtv": {"name": "GoodTV", "type": "direct", "url": "http://mobliestream.c3tv.com:1935/live/goodtv.sdp/playlist.m3u8"},
    "ebs1": {"name": "EBS1", "type": "direct", "url": "https://ebsonair.ebs.co.kr/groundwavefamilypc/familypc1m/playlist.m3u8"},
    "ebs2": {"name": "EBS2", "type": "direct", "url": "https://ebsonair.ebs.co.kr/ebs2familypc/familypc1m/playlist.m3u8"},
    "ebskids": {"name": "EBS Kids", "type": "direct", "url": "https://ebsonair.ebs.co.kr/ebsufamilypc/familypc1m/playlist.m3u8"},
    "fgtv": {"name": "FGTV", "type": "direct", "url": "https://fgtvlive.fgtv.com/smil:fgtv.smil/playlist.m3u8"},
    "lotte": {"name": "Lotte iMall", "type": "direct", "url": "https://pchlslivesw.lotteimall.com/live/livestream/lotteimalllive_mp4.m3u8"},
    "mbc_chuncheon": {"name": "MBC Chuncheon", "type": "direct", "url": "https://stream.chmbc.co.kr/TV/myStream/playlist.m3u8"},
    "mbc_yeosu": {"name": "MBC Yeosu", "type": "direct", "url": "https://5c3639aa99149.streamlock.net/live_TV/tv/playlist.m3u8"},
    "jibs": {"name": "JIBS", "type": "direct", "url": "http://123.140.197.22/stream/1/play.m3u8"},
    "jtv": {"name": "JTV", "type": "direct", "url": "http://61.85.197.53:1935/jtv_live/myStream/playlist.m3u8"},
    "oun": {"name": "OUN", "type": "direct", "url": "https://live.knou.ac.kr/knou1/live1/playlist.m3u8"},
    "wshopping": {"name": "W Shopping", "type": "direct", "url": "https://liveout.catenoid.net/live-05-wshopping/wshopping_1500k/playlist.m3u8"},
    "shinsegae": {"name": "Shinsegae TV", "type": "direct", "url": "https://liveout.catenoid.net/live-02-shinsegaetvshopping/shinsegaetvshopping_720p/playlist.m3u8"},
    "nhtv": {"name": "NH TV", "type": "direct", "url": "http://nonghyup.flive.skcdn.com/nonghyup/_definst_/nhlive/playlist.m3u8"},
}

HOST = "0.0.0.0"
WIDTH = 280
HEIGHT = 157
FPS = 15
FRAME_INTERVAL = 1.0 / FPS
AUDIO_RATE = 16000
AUDIO_CHUNK = round(AUDIO_RATE / FPS)
JPEG_QUALITY = 12
MAX_JPEG_BYTES = 3891
PIL_MAX_QUALITY = 75
PIL_MIN_QUALITY = 8
READ_CHUNK = 4096
PACKET_MAGIC = b"\xAA\xBB"
BUFFER_SECONDS = 10.0
MAX_BUFFER_SECONDS = 14.0

LOG = logging.getLogger("tv_server")


class ServerState:
    def __init__(self, initial_channel: str) -> None:
        self._lock = threading.Lock()
        self._current_channel = initial_channel
        self._change_serial = 0

    def get(self) -> tuple[str, int]:
        with self._lock:
            return self._current_channel, self._change_serial

    def set_channel(self, channel_key: str) -> None:
        with self._lock:
            if self._current_channel == channel_key:
                return
            self._current_channel = channel_key
            self._change_serial += 1
        LOG.info("channel changed to %s (%s)", channel_key, CHANNELS[channel_key]["name"])


def resolve_stream_url(channel_key: str) -> str:
    channel = CHANNELS[channel_key]
    if channel["type"] == "direct":
        return channel["url"]

    cmd = [
        "yt-dlp",
        "--no-warnings",
        "--no-playlist",
        "-g",
        channel["url"],
    ]
    LOG.info("resolving stream URL with yt-dlp: %s", channel["url"])
    result = subprocess.run(
        cmd,
        check=True,
        capture_output=True,
        text=True,
    )
    for line in result.stdout.splitlines():
        line = line.strip()
        if line:
            LOG.info("resolved direct stream URL for %s", channel_key)
            return line
    raise RuntimeError("yt-dlp did not return a stream URL")


class ClientManager:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._clients: dict[socket.socket, queue.Queue[Optional[bytes]]] = {}

    def add(self, conn: socket.socket) -> None:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn.settimeout(0.5)
        send_queue: queue.Queue[Optional[bytes]] = queue.Queue(maxsize=3)
        with self._lock:
            self._clients[conn] = send_queue
            total = len(self._clients)
        thread = threading.Thread(target=self._sender_loop, args=(conn, send_queue), daemon=True)
        thread.start()
        try:
            peer = conn.getpeername()
        except OSError:
            peer = "<unknown>"
        LOG.info("client connected: %s (total=%d)", peer, total)

    def _sender_loop(self, conn: socket.socket, send_queue: queue.Queue[Optional[bytes]]) -> None:
        try:
            while True:
                data = send_queue.get()
                if data is None:
                    return
                conn.sendall(data)
        except OSError:
            pass
        finally:
            self._remove_client(conn)

    def _remove_client(self, conn: socket.socket) -> None:
        with self._lock:
            existed = conn in self._clients
            if existed:
                self._clients.pop(conn, None)
            total = len(self._clients)
        try:
            conn.close()
        except OSError:
            pass
        if existed:
            LOG.info("removed 1 dead client(s) (total=%d)", total)

    def broadcast(self, data: bytes) -> None:
        with self._lock:
            clients = list(self._clients.items())

        for conn, send_queue in clients:
            try:
                send_queue.put_nowait(data)
            except queue.Full:
                try:
                    send_queue.get_nowait()
                except queue.Empty:
                    pass
                try:
                    send_queue.put_nowait(data)
                except queue.Full:
                    LOG.debug("dropping frame for slow client")

    def count(self) -> int:
        with self._lock:
            return len(self._clients)


class ControlHandler(BaseHTTPRequestHandler):
    state: ServerState

    def do_GET(self) -> None:
        if self.path == "/channels":
            body = json.dumps(
                {
                    key: {"name": value["name"], "type": value["type"]}
                    for key, value in CHANNELS.items()
                },
                ensure_ascii=False,
            ).encode("utf-8")
            self._send(200, "application/json; charset=utf-8", body)
            return

        if self.path == "/status":
            channel_key, serial = self.state.get()
            channel = CHANNELS[channel_key]
            body = json.dumps(
                {
                    "channel": channel_key,
                    "name": channel["name"],
                    "type": channel["type"],
                    "change_serial": serial,
                },
                ensure_ascii=False,
            ).encode("utf-8")
            self._send(200, "application/json; charset=utf-8", body)
            return

        self._send(404, "text/plain", b"not found")

    def do_POST(self) -> None:
        if not self.path.startswith("/channel/"):
            self._send(404, "text/plain", b"not found")
            return

        channel_key = self.path[len("/channel/") :]
        if channel_key not in CHANNELS:
            self._send(404, "text/plain", f"unknown channel: {channel_key}".encode("utf-8"))
            return

        self.state.set_channel(channel_key)
        body = json.dumps(
            {"ok": True, "channel": channel_key, "name": CHANNELS[channel_key]["name"]},
            ensure_ascii=False,
        ).encode("utf-8")
        self._send(200, "application/json; charset=utf-8", body)

    def _send(self, code: int, ctype: str, body: bytes) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args) -> None:
        LOG.debug("api %s - " + fmt, self.address_string(), *args)


def start_control_server(state: ServerState, host: str, port: int) -> ThreadingHTTPServer:
    class Handler(ControlHandler):
        pass

    Handler.state = state
    httpd = ThreadingHTTPServer((host, port), Handler)
    LOG.info("control api listening on %s:%d", host, port)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    return httpd


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
    def __init__(self, channel_key: str, verbose: bool = False) -> None:
        self._channel_key = channel_key
        self._verbose = verbose
        self._proc: Optional[subprocess.Popen] = None
        self.video_fd: Optional[int] = None
        self.audio_fd: Optional[int] = None

    def start(self) -> None:
        stream_url = resolve_stream_url(self._channel_key)
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
            f"pipe:{video_w}",
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
            f"pipe:{audio_w}",
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
        LOG.info("ffmpeg started for %s (pid=%d)", self._channel_key, self._proc.pid)

    def is_alive(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def returncode(self) -> Optional[int]:
        if self._proc is None:
            return None
        return self._proc.poll()

    def stop(self) -> None:
        returncode_before_stop = self.returncode()
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
            LOG.info(
                "ffmpeg stopped (returncode_before_stop=%s final_returncode=%s)",
                returncode_before_stop,
                self._proc.returncode,
            )
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


def fit_jpeg_size(jpeg_bytes: bytes) -> bytes:
    if len(jpeg_bytes) <= MAX_JPEG_BYTES or Image is None:
        return jpeg_bytes

    try:
        with Image.open(BytesIO(jpeg_bytes)) as image:
            image = image.convert("RGB")
            best = jpeg_bytes
            low = PIL_MIN_QUALITY
            high = PIL_MAX_QUALITY

            while low <= high:
                quality = (low + high) // 2
                out = BytesIO()
                image.save(out, format="JPEG", quality=quality, optimize=True, progressive=False)
                encoded = out.getvalue()

                if len(encoded) <= MAX_JPEG_BYTES:
                    best = encoded
                    low = quality + 1
                else:
                    high = quality - 1

            if len(best) > MAX_JPEG_BYTES:
                for quality in (6, 4, 2, 1):
                    out = BytesIO()
                    image.save(
                        out,
                        format="JPEG",
                        quality=quality,
                        optimize=True,
                        progressive=False,
                    )
                    encoded = out.getvalue()
                    best = encoded
                    if len(encoded) <= MAX_JPEG_BYTES:
                        break

            return best
    except Exception:
        LOG.exception("adaptive jpeg recompress failed")
        return jpeg_bytes


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
        clients.add(conn)


def stream_loop(state: ServerState, clients: ClientManager, verbose: bool) -> None:
    while True:
        channel_key, change_serial = state.get()
        packet_buffer: deque[tuple[float, bytes]] = deque()
        pipeline = FFmpegPipeline(channel_key, verbose=verbose)

        try:
            pipeline.start()
            assert pipeline.video_fd is not None
            assert pipeline.audio_fd is not None

            video_reader = MjpegReader(pipeline.video_fd)
            audio_buffer = bytearray()
            source_time = 0.0
            next_frame_at = time.monotonic()
            buffering = True

            while pipeline.is_alive():
                current_channel, current_serial = state.get()
                if current_serial != change_serial or current_channel != channel_key:
                    LOG.info("channel switch requested, restarting ffmpeg")
                    break

                fill_audio_buffer(pipeline.audio_fd, audio_buffer)
                jpeg_bytes = video_reader.read_frame()
                if jpeg_bytes is None:
                    LOG.warning(
                        "video pipe ended (channel=%s ffmpeg_returncode=%s buffered_audio=%d queued=%d)",
                        channel_key,
                        pipeline.returncode(),
                        len(audio_buffer),
                        len(packet_buffer),
                    )
                    break

                fill_audio_buffer(pipeline.audio_fd, audio_buffer)
                audio_bytes = take_audio_chunk(audio_buffer)
                jpeg_bytes = fit_jpeg_size(jpeg_bytes)
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

                delay = next_frame_at - time.monotonic()
                if delay > 0:
                    time.sleep(delay)

                if clients.count() > 0 and packet_buffer:
                    _, packet = packet_buffer.popleft()
                    clients.broadcast(packet)
                    LOG.debug(
                        "broadcast buffered frame channel=%s clients=%d queued=%d",
                        channel_key,
                        clients.count(),
                        len(packet_buffer),
                    )
                elif packet_buffer:
                    packet_buffer.popleft()

                next_frame_at += FRAME_INTERVAL
                now = time.monotonic()
                if next_frame_at < now - FRAME_INTERVAL:
                    next_frame_at = now + FRAME_INTERVAL

            LOG.warning(
                "stream iteration ended (channel=%s ffmpeg_alive=%s ffmpeg_returncode=%s queued=%d clients=%d)",
                channel_key,
                pipeline.is_alive(),
                pipeline.returncode(),
                len(packet_buffer),
                clients.count(),
            )

        except KeyboardInterrupt:
            raise
        except Exception as exc:
            LOG.exception("stream loop error: %s", exc)
        finally:
            pipeline.stop()

        current_channel, current_serial = state.get()
        if current_serial == change_serial and current_channel == channel_key:
            LOG.warning("restarting ffmpeg in 3 seconds for channel=%s", channel_key)
            time.sleep(3)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Pull a live stream with FFmpeg and push MJPEG+PCM packets to ESP32 clients."
    )
    parser.add_argument("--channel", choices=sorted(CHANNELS), default="kbs", help="built-in channel name")
    parser.add_argument("--host", default=HOST, help="listen host (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=9000, help="stream port (default: 9000)")
    parser.add_argument("--control-port", type=int, default=9001, help="control API port (default: 9001)")
    parser.add_argument("--verbose", action="store_true", help="enable verbose logging")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    LOG.info(
        "video=%dx%d@%dfps audio=%dHz pcm_u8 chunk=%d q=%d max_jpeg=%d",
        WIDTH,
        HEIGHT,
        FPS,
        AUDIO_RATE,
        AUDIO_CHUNK,
        JPEG_QUALITY,
        MAX_JPEG_BYTES,
    )

    state = ServerState(args.channel)
    clients = ClientManager()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.port))
    server.listen()
    LOG.info("stream server listening on %s:%d", args.host, args.port)

    accept_thread = threading.Thread(target=accept_loop, args=(server, clients), daemon=True)
    accept_thread.start()

    control_server = start_control_server(state, args.host, args.control_port)

    try:
        stream_loop(state, clients, args.verbose)
    except KeyboardInterrupt:
        LOG.info("shutting down")
    finally:
        control_server.shutdown()
        server.close()


if __name__ == "__main__":
    main()
