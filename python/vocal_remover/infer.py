"""【模块 18】本地 AI 人声消除 —— Python 只负责"训练 / 导出 / 离线跑批"。

运行时方是谁：`core/src/ai/onnx_vocals.cpp`（C++ ONNX Runtime）。
手机上不装 CPython（+120MB、GIL、还有解释器攻击面）。

用法：
    python infer.py --export  ../out/models/vocals.onnx   # 导出 ONNX
    python infer.py --check   ../out/models/vocals.onnx   # 校验算子集/输入输出
    python infer.py --run in.flac out.wav                 # 桌面整曲跑一遍（人耳验听）
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def export(out_path: Path) -> None:
    """导出静态 shape 的 ONNX（Android 侧一定要静态，否则 ORT 会退化到慢路径）。"""
    import torch
    from .model import VocalSeparator  # 延迟 import，--check 时不需要 torch

    net = VocalSeparator().eval()
    dummy = torch.zeros(1, 2, 44100 * 8)  # 8s 立体声，48kHz 用 48000*8
    out_path.parent.mkdir(parents=True, exist_ok=True)
    torch.onnx.export(
        net, dummy, str(out_path),
        input_names=["pcm"], output_names=["accomp", "vocal"],
        opset_version=17, do_constant_folding=True, dynamic_axes=None,
    )
    size = out_path.stat().st_size / 1024 / 1024
    print(f"→ {out_path} ({size:.1f} MB)")


def check(model_path: Path) -> int:
    import onnxruntime as ort

    so = ort.SessionOptions()
    so.intra_op_num_threads = 2
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    sess = ort.InferenceSession(str(model_path), so, providers=["CPUExecutionProvider"])
    inputs = [(i.name, i.shape, i.type) for i in sess.get_inputs()]
    outputs = [(o.name, o.shape, o.type) for o in sess.get_outputs()]
    print("输入:", inputs)
    print("输出:", outputs)
    # 手机端只接受 f32 单输入双输出、且第 3 维静态
    ok = (
        len(inputs) == 1 and inputs[0][1][-1] > 0 and str(inputs[0][2]) == "tensor(float)"
        and len(outputs) == 2
    )
    print("可用于 Android:", "是" if ok else "否（要静态 shape 的 f32）")
    return 0 if ok else 1


def run(model_path: Path, src: Path, dst: Path) -> int:
    import numpy as np
    import onnxruntime as ort
    import soundfile as sf

    audio, sr = sf.read(str(src), dtype="float32", always_2d=True)
    if sr != 48000:
        print(f"警告：采样率 {sr}，建议先重采样到 48000（给模块 14 做也可以）", file=sys.stderr)
    n = 48000 * 8
    pad = (-len(audio)) % n
    audio = np.pad(audio, ((0, pad), (0, 0)))
    sess = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    accomp = np.concatenate([
        sess.run(None, {"pcm": audio[i:i + n].T[None, ...]})[0][0].T
        for i in range(0, len(audio), n)
    ])
    sf.write(str(dst), accomp[: len(accomp) - pad], sr)
    print(f"→ {dst}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="模块18：人声消除模型的导出/校验/离线跑批")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--export", type=Path, help="导出 ONNX 到指定路径")
    g.add_argument("--check", type=Path, help="检查 ONNX 是否满足 Android 静态约束")
    g.add_argument("--run", nargs=2, metavar=("IN", "OUT"), help="桌面整曲跑一遍")
    ap.add_argument("--model", type=Path, default=Path("../out/models/vocals.onnx"))
    a = ap.parse_args()

    if a.export:
        export(a.export); return 0
    if a.check:
        return check(a.check)
    if a.run:
        return run(a.model, Path(a.run[0]), Path(a.run[1]))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
