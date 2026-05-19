# app_vlp

Android AR app module with optional `mobili::vlp` integration and a built-in gRPC stream server.

## gRPC stream

- Server endpoint: `<phone-ip>:50051` (plaintext gRPC)
- RPC: `vlp.FrameStreamService/StreamFrames` (server streaming)
- Payload format: binary frame
  - Header (little-endian, 64 bytes): magic, timestamp, width, height, intrinsics, pose
  - Body: JPEG bytes for full RGB image
- JPEG conversion is done only in the gRPC send path (when a subscriber is ready), not in every camera-frame update.

## Python test client

Script:

- `python/ws_stream_client.py`

Install dependency:

```bash
pip install grpcio opencv-python numpy
```

Run via ADB port forward:

```bash
adb forward tcp:50051 tcp:50051
python3 python/ws_stream_client.py --host 127.0.0.1 --port 50051
```

The client displays the stream live in OpenCV with overlaid intrinsics and pose.

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
