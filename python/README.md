
## Live gRPC stream client

Install dependencies:

```bash
pip install grpcio opencv-python numpy flask
```

Forward phone gRPC port:

```bash
adb forward tcp:50051 tcp:50051
```

Run client:

```bash
python3 python/ws_stream_client.py --host 127.0.0.1 --port 50051 --show-depth
```

https://github.com/user-attachments/assets/4f305b9f-1916-4a6f-b0e7-45c40ce3d9f1


## Live gRPC web client (no WebRTC)

This starts a local web server that subscribes to `/vlp.FrameStreamService/StreamFrames`
and serves an MJPEG page. Robot control buttons use gRPC control RPC
`/vlp.ControlService/SendControl` (no MQTT).

Forward phone gRPC ports:

```bash
adb forward tcp:50051 tcp:50051
```

Run web client:

```bash
python3 python/grpc_web_client.py \
  --grpc-host 192.168.19.171 \
  --grpc-port 50051 \
  --control-grpc-port 50051 \
  --web-port 8088 \
  --show-depth
```

Open in browser:

```text
http://127.0.0.1:8088/
```


## Read a recorded VLPREC1 dataset

Print frame metadata:

```bash
python3 python/vlprec_reader.py --input data/extreme_movement/2026-05-29_11-19-06/vlp_stream.rec  --display
```

Export frame bodies and metadata CSV:

```bash
python3 python/vlprec_reader.py \
  --input data/vlp_stream.rec \
  --export-dir out/frames \
  --csv out/frames/metadata.csv
```

Useful options:

- `--speed 2.0` for 2x playback
- `--max-frames 300` to stop early
