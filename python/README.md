
## Live gRPC stream client

Install dependencies:

```bash
pip install grpcio opencv-python numpy
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


## Read a recorded VLPREC1 dataset

Print frame metadata:

```bash
python3 python/vlprec_reader.py --input data/files/2026-05-22_15-42-37/vlp_stream.rec  --display
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
