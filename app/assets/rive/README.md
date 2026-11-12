# Rive 资源

把 `.riv` 放这里（模块 2）。约定：

| 文件 | 内容 | 状态机 |
|---|---|---|
| `eq_curve.riv` | 31 段 EQ 曲线 | `EQ`：输入 `eq0..eq30`（Number） |
| `play_button.riv` | 播放键呼吸灯 | `Play`：输入 `breath`（Number 0~1）、`pressed`（Bool） |
| `vinyl.riv` | 黑胶转盘 | `Spin`：输入 `rpm`（Number） |

> 骨架期还没美术资源，`rive_stage.dart` 会在缺文件时降级为纯 Flutter 绘制（不崩）。
