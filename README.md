# VisualLocalizationAndroidClient

Make an Android phone act as an RGBD + pose sensor for local mapping and visualization.

This repository contains:
- Android capture/export code (`app_vlp/`)
- Native mapping pipeline with Voxblox (`mapping/`)
- Python tools for gRPC streaming and dataset inspection (`python/`)

## What it does

Current C++ mapping pipeline (`//mapping:mono_vio_ws_stream_client`) replays recorded `VLPREC1` sessions and:
- decodes JPEG + pose per frame
- reads recorder depth attached in payload
- integrates depth + pose into Voxblox TSDF/ESDF
- publishes trajectory + ESDF 3D points + ESDF 2D plane slice to a built-in web viewer

https://github.com/user-attachments/assets/77e57cfb-af64-442e-8c0e-f65b7eb5f4ae

## Repository layout

- `app_vlp/`: Android app and native bridge to publish frame/depth/pose and optionally record datasets.
- `mapping/`: C++ mapping runtime and web viewer.
  - `mapping/mono_vio_ws_stream_client.cc`: main replay + mapping executable.
  - `mapping/common/data_session.*`: VLPREC1 reader.
  - `mapping/voxblox/voxblox_processor.*`: TSDF/ESDF integration and visualization extraction.
  - `mapping/backend/web_client.html`: web frontend.
- `python/`: tooling clients and `vlprec_reader.py` dataset inspector.
- `data/`: local datasets/screenshots (not guaranteed to exist on a fresh clone).

## Build

Prerequisites (host):
- Bazel (repo is Bazel-based)
- C++17 toolchain
- OpenCV dev libs available to Bazel toolchain

Build mapping viewer:

```bash
bazel build //mapping:mono_vio_ws_stream_client
```

## Run mapping replay + web viewer

Run:

```bash
./bazel-bin/mapping/mono_vio_ws_stream_client \
  --data_session=data/files/<session_dir>/vlp_stream.rec \
  --logtostderr=1 \
  --enable_websocket=true \
  --websocket_port=9002 \
  --enable_voxblox=true
```

Open:

```text
http://127.0.0.1:9002/index.html
```

## Important runtime flags

Replay:
- `--data_session=<path>`: input `.rec` file (required)
- `--loop_session=true|false`: restart at EOF
- `--replay_realtime=true|false`: honor recorded timestamps
- `--replay_speed=<float>`: playback speed multiplier

Voxblox/ESDF:
- `--enable_voxblox=true|false`
- `--voxblox_voxel_size_m=<meters>`
- `--esdf_update_every_n=<int>`: update/publish ESDF every N successful integrations
- `--esdf2d_height_m=<meters>`: world-space Y plane for 2D ESDF slice
- `--esdf2d_max_cells=<int>`: safety cap for 2D slice grid size

Web:
- `--enable_websocket=true|false`
- `--websocket_port=<port>`
- `--web_send_hz=<float>`
- `--web_max_traj_points=<int>`

## Dataset format

Recorded sessions use `VLPREC1` container with per-frame payloads containing:
- VLP2 frame header (timestamp, intrinsics, pose)
- JPEG image body
- optional depth chunk(s) tagged `DPT1`

Depth is decoded as metric `CV_32F` and integrated without resizing; intrinsics are scaled to depth resolution before integration.

## Python utilities

Inspect and play recorded datasets:

```bash
python3 python/vlprec_reader.py --input data/files/<session_dir>/vlp_stream.rec --display
```

Other Python gRPC/web clients are in `python/README.md`.

## Notes

- `mapping/README.md` has mapping-specific usage details.
- This repo may include local datasets and generated outputs that are machine-specific.
