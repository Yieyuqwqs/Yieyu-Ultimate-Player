# [18] Python · ONNX Runtime（训练/导出侧）

**Python 不进手机。** 上线形态是：Python 训练 → 导出静态 shape `.onnx` → 手机里 `onnxruntime` C++ 跑。

| 角色 | 谁 |
|---|---|
| 训练模型 | Python（本目录） |
| 导出 ONNX（opset 17，静态 shape） | `infer.py --export` |
| 手机推理 | `core/src/ai/onnx_vocals.cpp`（ORT C++，CPU/NNAPI EP） |

## 三条硬约束

1. **静态 shape**：最后一维必须是常量，否则 ORT 在 Android 上退化到极慢路径。
2. **f32 进 f32 出**，不算重采样（采样率统一由模块 14 负责）。
3. **只跑离线通道**：整曲预处理，进度回调给 UI。**绝不进 AAudio 回调。**

## 为什么不用 Chaquopy 在手机上跑 Python

体积 +120MB、GIL 抢线程、多一个解释器攻击面。而 ORT 的 C++ API 本来就能干完所有事。
