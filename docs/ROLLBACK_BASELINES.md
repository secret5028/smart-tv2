# Rollback Baselines

## 2026-03-18 Pre-Swipe UI

- Baseline tag: `rollback-pre-swipe-ui`
- Baseline commit: `232497b`
- Purpose: last stable state before touch swipe channel UI, buffering overlay, and channel API client logic

### Known behavior at this baseline

- Video stream: `280x240`
- Frame rate: `15fps`
- JPEG quality: `q=12`
- Server buffer: `10s`
- Max buffer: `14s`
- Source in active use: KBS signed `m3u8`
- ESP32 firmware renders MJPEG + PCM stream, but has no touch swipe UI yet

### Roll back to this baseline

```bash
git checkout rollback-pre-swipe-ui
```

Or, if staying on the current branch:

```bash
git reset --hard 232497b
```
