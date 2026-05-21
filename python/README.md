# Python Client

This folder contains the desktop gRPC client for `app_vlp` streaming.

## Files

- `ws_stream_client.py`: gRPC stream receiver + OpenCV viewer + keyframe/DA3 processor.
- `grpc_recorder.py`: records raw gRPC stream payloads with receive-time intervals.
- `grpc_player.py`: replays recorded payloads at original timing.
- `grpc_player_da2.py`: replays recorded payloads, runs DA2 ONNX depth, and shows RGB+depth side by side.
- `grpc_stream_da2_depth.py`: replays a recorded `VLPREC1` file, runs DA2 on RGB, and renders RGB/depth, trajectory, and point cloud in HTML.
- `da3_pair_processor.py`: extracted DA3 ONNX pair-inference module.
- `models/da3_small_2_392x224_sim.onnx`: Depth Anything 3 ONNX model (pair inference, 392x224).

## Install

```bash
pip install grpcio opencv-python numpy onnxruntime
```

## Record stream data

Record sequential payloads from gRPC stream:

```bash
python3 python/grpc_recorder.py \
  --host 192.168.19.153 \
  --port 50051 \
  --output data/vlp_stream.rec
```

Press `Ctrl+C` to stop.

## Replay recorded data

Replay using original receive-time intervals:

```bash
python3 python/grpc_player.py --input data/vlp_stream.rec
```

Useful options:

- `--speed 2.0` for 2x faster replay
- `--loop` to replay continuously
- `--no-display` to replay timing only without OpenCV window

## Replay as gRPC server

Run player as a gRPC server with the same endpoint:

```bash
python3 python/grpc_player.py \
  --input data/vlp_stream.rec \
  --serve-port 50051 \
  --loop
```

It serves:

- `/vlp.FrameStreamService/StreamFrames` (unary-stream, raw bytes)

Then run your existing client against this host/port.

## Replay + DA2 monocular depth

Run playback with Depth Anything V2 ONNX and visualize RGB + depth panel:

```bash
python3 python/grpc_player_da2.py \
  --input data/vlp_stream.rec \
  --model python/models/depth_anything_v2_metric_vits.onnx \
  --providers CPUExecutionProvider
```

Useful options:

- `--speed 2.0`
- `--loop`
- `--no-display`

Render recent depth-with-pose fused point cloud in HTML:

```bash
python3 python/grpc_player_da2.py \
  --input data/vlp_stream.rec \
  --model python/models/depth_anything_v2_vits.onnx \
  --websocket-port 9002 \
  --pc-recent-n 8 \
  --pc-pixel-step 8 \
  --pc-max-points 30000 \
  --depth-scale 1.0 \
  --depth-max 8.0
```

Then open:

```text
http://127.0.0.1:9002/index.html
```

## Replay recorded stream with DA2 depth

Replay a recorded `vlp_stream_vlp3.rec`; depth/PCL are computed from DA2 on RGB (recorded depth is ignored):

```bash
python3 python/grpc_stream_da2_depth.py \
  --input data/vlp_stream_vlp3.rec --no-display \
  --model python/models/depth_anything_v2_metric_vits.onnx \
  --providers CPUExecutionProvider \
  --websocket-port 9002 \
  --pc-recent-n 8 \
  --pc-pixel-step 8 \
  --pc-max-points 30000 \
  --depth-max 8.0
```

Useful options:

- `--speed 2.0`
- `--loop`
- `--da2-depth-scale 1.0`
- `--output-mp4 out/replay.mp4 --mp4-fps 20`

Open:

```text
http://127.0.0.1:9002/index.html
```

## Run

Forward port from phone:

```bash
adb forward tcp:50051 tcp:50051
```

Start client:

```bash
python3 python/ws_stream_client.py --host 127.0.0.1 --port 50051
```

Press `q` in the OpenCV window to quit.

## Keyframe logic

Frames are marked as keyframes when pose change vs last keyframe exceeds either threshold:

- Rotation threshold (`--keyframe-rot-deg`, default `12.0`)
- Translation threshold (`--keyframe-trans-m`, default `0.20`)

Example:

```bash
python3 python/ws_stream_client.py \
  --keyframe-rot-deg 5.0 \
  --keyframe-trans-m 0.10
```

## DA3 pair processing

On each new keyframe, DA3 runs on sequential pairs:

- `(kf1, kf2)`
- `(kf2, kf3)`
- `(kf3, kf4)`

DA3 result is shown in a second window: `DA3 Depth (Keyframe Pair)`.

## Depth scale alignment with real pose

For each keyframe pair, the client:

1. Reads DA3 pair `extrinsics` output and computes DA3 translation delta:
   `||t_da3(kf_{i+1}) - t_da3(kf_i)||`.
2. Computes real translation delta from gRPC poses:
   `||t_real(kf_{i+1}) - t_real(kf_i)||`.
3. Computes depth scale:
   `scale = real_delta / da3_delta`.
4. Applies this scale to DA3 relative depth for visualization.

The side-by-side depth panel overlays:

- `scale=<value>`
- `scale: failed (translation)` when translation-based scale is unavailable

## Model options

Default model path and size:

- `--da3-model python/models/da3_small_2_392x224_sim.onnx`
- `--da3-width 392`
- `--da3-height 224`

Example:

```bash
python3 python/ws_stream_client.py \
  --da3-model python/models/da3_small_2_392x224_sim.onnx \
  --da3-width 392 \
  --da3-height 224
```
