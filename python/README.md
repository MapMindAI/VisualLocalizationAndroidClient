
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
