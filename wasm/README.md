# [8] WebAssembly · WasmEdge 热更新插件

下发的音效包 = 一个 `.wasm`。改音效不用发版，但插件的能力被压到只剩"算"。

## 沙箱限制（宿主 `core/src/wasm/host_wasmedge.cpp` 强制）

| 项 | 限制 | 原因 |
|---|---|---|
| 文件 / 网络 | **无 WASI**，宿主不注册任何 IO 能力 | 插件不许碰用户数据 |
| 内存 | 32 MB | 防一个坏插件吃光整机 |
| 栈 | 1 MB | 同上 |
| JIT | **禁**（Android W^X + 私有数据执行） | 只能走 interpreter 或预编译 AOT |
| 每块样本 | 32 ~ 64 | 保证 4ms 预算内能跑完 |

## 插件 ABI（固定 3 个导出）

```wat
(export "yieyu_abi")      ;; () -> i32  必须返回 1
(export "yieyu_init")     ;; (sr: i32, params: *f32, n: i32) -> i32
(export "yieyu_process")  ;; (io: *f32, frames: i32) -> i32
```

`yieyu_process` 拿到的是**交错立体声 float 指针**，可以就地改写。返回值 <0 表示本块跳过（宿主当作 bypass）。

## 为什么这里不用 JNI/动态库

`soldier.so` 会被 `dlopen` 进主进程：一个 bad 指针就能崩整机，还能读我们所有内存。
wasm 是**验证过的沙箱**，逃逸需要宿主 bug —— 这是我们敢热更新的唯一前提。
