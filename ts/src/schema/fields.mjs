/**
 * 【模块6】字段清单：跨语言生成的唯一来源（纯数据，零依赖、零编译）。
 *
 * 为什么从 .ts 改成 .mjs：TS 需要从 npm 拉一个 30MB 的编译器，而我们只用到"类型标注"
 * 这一层收益 —— 外行的依赖。这里直接用带 JSDoc 的 ESM，Node 原生就能跑，
 * 构建期（tools/gen-schema.mjs）与回归（tools/ts-build.mjs）都直接 import。
 *
 * 每项：[Dart 名, JSON 键, 类型]
 */
export const SONG_FIELDS = [
  ['id', 'id', 'string'],
  ['source', 'source', 'string'],
  ['title', 'name', 'string'],      // 源站字段叫 name，网络层统一成 title
  ['artist', 'artist', 'string'],
  ['album', 'album', 'string'],
  ['durationMs', 'durationMs', 'int'],
  ['quality', 'quality', 'string'],
  ['mid', 'mid', 'string'],
]
