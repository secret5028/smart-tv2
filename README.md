# ESP32 TV Streaming Project

Oracle Cloud VM에서 라이브 스트림을 FFmpeg로 MJPEG/PCM으로 변환해 ESP32-S3 LCD 보드로 TCP 송출하는 프로젝트다.

## Current Stable Streaming Baseline

- Video: `280x240`
- Frame rate: `15fps`
- JPEG quality: `q=12`
- Audio: `16kHz`, mono, `pcm_u8`
- Server buffer: `10s`
- Max buffer: `14s`
- Active source: KBS signed `m3u8`

Rollback baseline before touch UI work:

- Tag: `rollback-pre-swipe-ui`
- Commit: `232497b`
- Notes: [docs/ROLLBACK_BASELINES.md](/c:/Users/AAA/Desktop/esptv2/docs/ROLLBACK_BASELINES.md)

## Server

`tv_server.py` runs on Linux and pushes packets in this format:

```text
[0xAA][0xBB][video size LE][JPEG][audio size LE][PCM u8]
```

Run:

```bash
python3 tv_server.py --url "SIGNED_M3U8_URL" --port 9000 --verbose
```

## Firmware

`tv_player/` contains the PlatformIO firmware for Waveshare ESP32-S3-Touch-LCD-1.69 V2.

Current firmware behavior:

- Connects to Wi-Fi and TCP stream server
- Renders JPEG frames and plays PCM audio
- Calls channel API on `SERVER_HOST:9001`
- Uses touch swipe for channel switching
- Shows channel name in green at top-left for 3 seconds
- Shows monochrome analog-style noise while connecting or buffering

## Build

```bash
cd tv_player
pio run
pio run -t upload
```

## Notes

- `tv_server.py` uses `pass_fds`, so it must run on Linux, not Windows.
- Signed `m3u8` URLs expire and must be refreshed.
- Touch controller auto-detection in the firmware currently probes common I2C pin pairs at runtime.
