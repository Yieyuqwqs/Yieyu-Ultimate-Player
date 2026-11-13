#!/usr/bin/env python3
"""【模块18】人声消除的核系数生成器（纯标准库，构建期跑，产物是 C++ 头文件）。

为什么是"Python 生成 C 常量"而不是"手机上跑 Python"：
  手机侧要的是每帧 2048 点 STFT 的内核；把它算在运行期是浪费，也不可控。
  Python 在构建期把分析窗、中置权重曲线算好，编成常量进 .so；
  这样模块 18 的 Python 部分**每次构建都真的被执行**，不是摆设。

产物：core/src/ai/vocal_kernel.h（CI 里 tools/check-ai-kernel.mjs 会重新生成并逐值比对）
"""
import math
import os
import sys

NFFT = 2048
TAIL = 32


def hann(n, total):
    # 周期 Hann（STFT 用，hop=N/4 时满足 COLA）
    return 0.5 * (1.0 - math.cos(2.0 * math.pi * n / total))


def center_weight(freq):
    """中置抑制的频率权重：人声主区 200~5000Hz 全权重，两端余弦过渡。"""
    if freq < 80.0 or freq > 8000.0:
        return 0.0
    if freq < 200.0:
        return 0.5 * (1.0 - math.cos(math.pi * (freq - 80.0) / 120.0))
    if freq > 5000.0:
        return 0.5 * (1.0 + math.cos(math.pi * (freq - 5000.0) / 3000.0))
    return 1.0


def fmt(v):
    return "%.9gf" % v


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    out = os.path.join(root, "core", "src", "ai", "vocal_kernel.h")
    half = NFFT // 2 + 1

    win = [hann(i, NFFT) for i in range(NFFT)]
    # 中置权重按"归一化频率"给出：索引 i → i/ NFFT 的归一化频率，乘出来由调用方决定采样率
    w = [center_weight(1000.0 * i / float(half)) for i in range(half)]  # 以 1kHz 为尺度，运行期按 bin 频率重算

    lines = []
    lines.append("// 由 python/vocal_remover/design.py 生成，**不要手改**。")
    lines.append("// CI 里 tools/check-ai-kernel.mjs 会重新跑一次生成器并逐值比对（差 1e-6 就红）。")
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace yieyu {")
    lines.append("namespace vocal {")
    lines.append("constexpr int   kNfft = %d;" % NFFT)
    lines.append("constexpr int   kHop = %d;" % (NFFT // 4))
    lines.append("constexpr int   kBins = %d;" % half)
    lines.append("constexpr float kMaskPower = 2.0f;      // |mid|^p / (|mid|^p + |side|^p)")
    lines.append("constexpr float kCenterAtten = 0.95f;   // 中置最多压掉 95%（留一点，避免"挖空")")
    lines.append("constexpr float kEps = 1e-9f;")
    lines.append("")
    lines.append("// 周期 Hann 分析/合成窗（平方后满足 COLA，hop = N/4）")
    lines.append("static const float kWindow[kNfft] = {")
    for i in range(0, NFFT, 8):
        lines.append("    " + ", ".join(fmt(v) for v in win[i:i + 8]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("// 中置权重曲线（按 1kHz 尺度索引，运行期按实际 bin 频率线性取样）")
    lines.append("static const float kWeightShape[kBins] = {")
    for i in range(0, half, 8):
        lines.append("    " + ", ".join(fmt(v) for v in w[i:i + 8]) + ",")
    lines.append("};")
    lines.append("}  // namespace vocal")
    lines.append("}  // namespace yieyu")
    lines.append("")

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("  → %s (%d + %d 个系数)" % (out, NFFT, half))


if __name__ == "__main__":
    sys.exit(main())
