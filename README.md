# ESP32 TV Streaming Project

Oracle Cloud VM에서 라이브 스트림을 FFmpeg로 MJPEG/PCM으로 변환해 ESP32-S3 LCD 보드로 TCP 송출하는 프로젝트다.

## 구성

- `tv_server.py`
  - VM에서 실행
  - 입력 스트림을 MJPEG 비디오 + PCM u8 오디오로 변환
  - ESP32 클라이언트에 TCP 브로드캐스트
- `tv_player/`
  - Waveshare ESP32-S3-Touch-LCD-1.69 (V2)용 PlatformIO 펌웨어
  - TCP 패킷을 받아 JPEG 디코드 후 LCD 출력
  - PCM u8 오디오를 I2S 스피커로 재생

## 현재 안정 설정

현재 확인된 실사용 설정은 아래와 같다.

- 비디오 출력: `280x240`
- 프레임레이트: `15fps`
- JPEG 품질: `q=12`
- 오디오: `16kHz`, mono, `pcm_u8`
- 서버 버퍼: `10초`
- 최대 버퍼: `14초`
- 실사용 소스: KBS signed `m3u8`

## 서버 실행

리눅스 VM에서 실행한다.

```bash
python3 tv_server.py --url "SIGNED_M3U8_URL" --port 9000 --verbose
```

KBS 같은 signed `m3u8`는 만료되므로, 만료 시 새 URL로 다시 실행해야 한다.

## 펌웨어 빌드

```bash
cd tv_player
pio run
```

업로드:

```bash
pio run -t upload
```

## 참고

- 현재 서버 구현은 `pass_fds`를 사용하므로 Windows가 아니라 Linux VM에서 실행해야 한다.
- ESP32는 Wi-Fi 연결 후 `SERVER_HOST:SERVER_PORT`로 plain TCP 접속한다.
- 빌드 산출물과 로그는 Git에 포함하지 않는다.
