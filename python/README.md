
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
python3 python/grpc_stream_client.py --host 127.0.0.1 --port 50051
```

https://github.com/user-attachments/assets/4f305b9f-1916-4a6f-b0e7-45c40ce3d9f1

## Read a recorded VLPREC dataset

Print frame metadata:

```bash
python3 python/vlprec_reader.py --input data/${SESSION}/vlp_stream.rec  --display
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
- `--save-video out/vlprec_triptych.mp4` save triptych video instead of live display
- `--video-fps 30` set output video FPS when saving

Save video example:

```bash
SESSION=extreme_movement
python3 python/vlprec_reader.py \
  --input data/${SESSION}/vlp_stream.rec \
  --save-video data/${SESSION}/vlprec_triptych.mp4 \
  --video-fps 30
```
