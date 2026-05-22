# app_vlp

Android AR app module with optional `mapmind_vlp_api` native integration and a built-in gRPC stream server.

https://github.com/user-attachments/assets/0202e980-5f74-47c4-9e53-88ddcdbd4d77


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

## mapmind_vlp_api library

`mapmind_vlp_api` is enabled by default and linked by `hello_ar_native`.

```bash
./gradlew :app:assembleDebug
```
