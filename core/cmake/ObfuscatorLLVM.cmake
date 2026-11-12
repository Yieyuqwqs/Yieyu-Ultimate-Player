# 模块11：机器码混淆与防篡改（默认关，P4 阶段才开）
#
# 现实约束（docs/ARCH_24.md §4）：NDK r23 起弃 GCC 前端，现成 O-LLVM 分支套不上 NDK r27 的 clang。
# 唯一可行三条路，按性价比排序：
#   A. LLVM Pass Plugin（-fpass-plugin=libollvmpass.so）：改动最小，但强度有限（主要是字符串/常量混淆）
#   B. 自建 O-LLVM（bogus control flow + fla + substitution）+ 自编 libc++：强度最高，维护地狱
#   C. R8/D8 侧混淆（只覆盖 Kotlin）+ 原生侧符号剥离 + 校验和自检：成本最低的"防小白"
#
# 本文件只提供开关与自检，不做任何"假装混淆"的事。

option(YIEYU_OBFUSCATE_STRICT "B 方案：自建 O-LLVM 工具链" OFF)
set(YIEYU_OLLVM_PASS "" CACHE FILEPATH "A 方案：pass 插件 .so 路径")

if(YIEYU_OBFUSCATE)
  if(YIEYU_OBFUSCATE_STRICT)
    message(STATUS "[11] O-LLVM strict：使用自建工具链 ${CMAKE_C_COMPILER}")
    add_compile_options(-mllvm -bcf -mllvm -bcf_prob=60 -mllvm -fla -mllvm -sub)
  elseif(YIEYU_OLLVM_PASS AND EXISTS "${YIEYU_OLLVM_PASS}")
    message(STATUS "[11] LLVM pass plugin：${YIEYU_OLLVM_PASS}")
    add_compile_options(-fpass-plugin=${YIEYU_OLLVM_PASS})
  else()
    message(WARNING "[11] 未提供混淆工具链 → 退 C 方案：只做符号剥离 + 段校验，见 tools/ollvm/README.md")
    add_link_options(-s)
  endif()
else()
  message(STATUS "[11] 混淆关闭（YIEYU_OBFUSCATE=OFF）")
endif()
