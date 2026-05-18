# app_vlp

Android AR app module with optional `mobili::vlp` integration and a built-in WebSocket stream server.

## WebSocket stream

- Server endpoint: `ws://<phone-ip>:8765`
- Message type: JSON (`type = "frame"`)
- Payload fields:
  - `timestamp_ns`
  - `width`, `height`
  - `intrinsics`: `fx`, `fy`, `cx`, `cy`
  - `pose`: `qx`, `qy`, `qz`, `qw`, `tx`, `ty`, `tz`
  - `gray_b64`: base64-encoded gray image bytes (Y channel)

## Python test client

Script:

- `scripts/ws_stream_client.py`

Install dependency:

```bash
pip install websocket-client
```

Run via ADB port forward:

```bash
adb forward tcp:8765 tcp:8765
python3 python/ws_stream_client.py --host 127.0.0.1 --port 8765 --max-frames 20
```

The client saves received frames as `.pgm` files under `stream_frames/` by default.

## Optional mobili::vlp library

`mobili::vlp` is disabled by default.

- Enable it with Gradle property:

```bash
./gradlew :app:assembleDebug -PenableMobiliVlp=true
```

- Default (disabled):

```bash
./gradlew :app:assembleDebug
```

When disabled, the app runs without linking `libmobili_vlp_api.so`.
