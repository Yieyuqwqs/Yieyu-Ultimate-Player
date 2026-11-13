// [9]/[5] 音源直链解析：把 App 内部的 "yieyu://song?..." 变成能直接喂给解码器的 URL。
//
// 为什么要有这一层（别在 player.cpp 里直接调网络）：
//   播放器只认"能打开的东西"（本地路径 / http(s) 直链）。搜歌出来的只有 (source, id)，
//   需要一次网络往返换直链。这一步放在**解码线程**做（player.cpp 的 OpenAndPlay），
//   控制面（Dart/JSB 线程）永远不等网络 —— 铁律不变。
//
// 本文件只做两件事：解析内部 URI、调 Go 的 yieyu_music_url 换地址。
// 不做播放、不做搜索、不写盘。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 把 uri 解析成可打开的真实地址，写进 out（NUL 结尾）。
//   · 普通 http(s)://... 或本地绝对路径：原样拷贝，返回 0
//   · yieyu://song?source=wy&id=95328&mid=002K6...：走网络换直链，成功返回 0
// 返回：0=成功（out 可打开）；<0=失败（原因见 yieyu_song_last_error）
int yieyu_resolve_uri(const char* uri, char* out, size_t cap);

// 最近一次解析失败的原因（只在黑匣子/开发者面板里出现）
const char* yieyu_song_last_error(void);

#ifdef __cplusplus
}
#endif
