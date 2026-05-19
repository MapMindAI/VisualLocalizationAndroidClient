import time
from typing import Any, Dict, Optional

import cv2
import numpy as np

try:
    import onnxruntime as ort
except ImportError:
    ort = None


class DA3PairProcessor:
    def __init__(self, model_path: str, input_width: int, input_height: int):
        if ort is None:
            raise RuntimeError("onnxruntime is not installed.")
        self.model_path = model_path
        self.input_width = input_width
        self.input_height = input_height
        self.session = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
        self.input_names = [x.name for x in self.session.get_inputs()]
        self.input_shapes = [x.shape for x in self.session.get_inputs()]
        self.output_names = [x.name for x in self.session.get_outputs()]
        print(
            f"[DA3] loaded model: {model_path}, inputs={list(zip(self.input_names, self.input_shapes))}, "
            f"outputs={self.output_names}"
        )

    @staticmethod
    def is_available() -> bool:
        return ort is not None

    def _preprocess(self, image_bgr: np.ndarray) -> np.ndarray:
        rgb = cv2.cvtColor(image_bgr, cv2.COLOR_BGR2RGB)
        resized = cv2.resize(
            rgb,
            (self.input_width, self.input_height),
            interpolation=cv2.INTER_LINEAR,
        )
        tensor = resized.astype(np.float32) / 255.0
        return np.transpose(tensor, (2, 0, 1))[None, ...]  # NCHW

    def _build_one_input_tensor(self, shape: Any, a_nchw: np.ndarray, b_nchw: np.ndarray) -> np.ndarray:
        if len(shape) == 4:
            if shape[1] == 6 or shape[1] == "6" or shape[1] is None or shape[1] == -1:
                return np.concatenate([a_nchw, b_nchw], axis=1)
            if shape[3] == 6 or shape[3] == "6":
                a_nhwc = np.transpose(a_nchw, (0, 2, 3, 1))
                b_nhwc = np.transpose(b_nchw, (0, 2, 3, 1))
                return np.concatenate([a_nhwc, b_nhwc], axis=3)
            return a_nchw
        if len(shape) == 5:
            return np.stack([a_nchw, b_nchw], axis=1)
        return a_nchw

    def infer_pair(
        self,
        image_a_bgr: np.ndarray,
        image_b_bgr: np.ndarray,
        pair_label: str = "",
    ) -> Optional[Dict[str, np.ndarray]]:
        a_nchw = self._preprocess(image_a_bgr)
        b_nchw = self._preprocess(image_b_bgr)

        if len(self.input_names) == 1:
            inp = self._build_one_input_tensor(self.input_shapes[0], a_nchw, b_nchw)
            feeds = {self.input_names[0]: inp}
        else:
            feeds = {}
            feeds[self.input_names[0]] = a_nchw
            feeds[self.input_names[1]] = b_nchw
            for name in self.input_names[2:]:
                feeds[name] = a_nchw

        t0 = time.time()
        output_values = self.session.run(None, feeds)
        infer_ms = (time.time() - t0) * 1000.0
        if not output_values:
            print("[DA3] no outputs returned")
            return None
        outputs = {name: value for name, value in zip(self.output_names, output_values)}
        label = f" {pair_label}" if pair_label else ""
        shape_summary = {k: tuple(v.shape) for k, v in outputs.items()}
        print(f"[DA3]{label} infer={infer_ms:.1f}ms outputs={shape_summary}")
        return outputs

    @staticmethod
    def output_to_depth_map(output: np.ndarray) -> Optional[np.ndarray]:
        arr = output
        if arr.ndim == 4:
            arr = arr[0, 0]
        elif arr.ndim == 3:
            arr = arr[0]
        elif arr.ndim != 2:
            return None
        arr = arr.astype(np.float32)
        if not np.all(np.isfinite(arr)):
            return None
        return arr

    @staticmethod
    def output_to_vis(output: np.ndarray) -> Optional[np.ndarray]:
        arr = DA3PairProcessor.output_to_depth_map(output)
        if arr is None:
            return None
        mn = float(np.min(arr))
        mx = float(np.max(arr))
        if not np.isfinite(mn) or not np.isfinite(mx) or mx <= mn:
            return None
        norm = (arr - mn) / (mx - mn)
        depth_u8 = (norm * 255.0).astype(np.uint8)
        return cv2.applyColorMap(depth_u8, cv2.COLORMAP_INFERNO)
