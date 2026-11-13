/**
 * 【模块6】字段清单：跨语言生成的**唯一来源**（纯数据，不 import 任何东西）。
 *
 * 为什么单独一个文件：构建期 `tools/gen-schema.mjs` 要直接 import 这个模块的 tsc 产物，
 * 如果它 import 了 zod，Node 从 out/ts/esm/ 解析不到 ts/node_modules 就会炸（踩过）。
 * 类型 schema 在 song.ts；这里只放"字段名 + JSON 键 + 类型"。
 *
 * 每项：[Dart 名, JSON 键, 类型]
 */
export const SONG_FIELDS: ReadonlyArray<readonly [string, string, 'string' | 'int']> = [
  ['id', 'id', 'string'],
  ['source', 'source', 'string'],
  ['title', 'name', 'string'],      // 源站字段叫 name，网络层统一成 title
  ['artist', 'artist', 'string'],
  ['album', 'album', 'string'],
  ['durationMs', 'durationMs', 'int'],
  ['quality', 'quality', 'string'],
  ['mid', 'mid', 'string'],
]
