// [18] ONNX Runtime：本地 AI 人声消除（KTV 伴奏）。
// 铁律：只在"离线/非实时"通道跑（整曲预处理），绝不进 AAudio 回调。
// 说明：Python 负责训练与导出 .onnx，运行时是 C++ ORT —— 手机上不装 CPython。
#include "../common/log.h"

#ifdef YIEYU_ENABLE_AI
#include <onnxruntime_cxx_api.h>
#endif

namespace yieyu {
namespace {
bool g_ready = false;
}
int onnx_vocals_init(const char* modelPath) {
#ifdef YIEYU_ENABLE_AI
  // TODO(P5)：Ort::Session + CPU/NNAPI EP，模型见 python/vocal_remover/
  LOGI("[18] ORT 加载 %s", modelPath);
  g_ready = true;
  return 0;
#else
  (void)modelPath;
  LOGW("[18] ONNX 未启用（-DYIEYU_ENABLE_AI=OFF），人声消除按钮置灰");
  g_ready = false;
  return -1;
#endif
}

// 整曲处理：进度回调给 UI（模块 1 显示进度条），不阻塞播放线程
int onnx_vocals_render(const char* srcPath, const char* dstPath, void (*onProgress)(float)) {
  (void)srcPath; (void)dstPath;
  if (!g_ready) return -1;
  if (onProgress) onProgress(1.0f);
  return 0;
}
}  // namespace yieyu
