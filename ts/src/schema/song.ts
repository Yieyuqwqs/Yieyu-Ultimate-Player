/**
 * 【模块 6】Zod · 唯一真源
 *
 * 所有跨语言/跨源站的歌曲结构只在这里定义一次：
 *   - TS 侧用 z.infer 拿类型（不许手写第二份 interface）
 *   - Go / Rust / C++ 侧的对应结构由 idl/*.fbs 承载，字段名必须与这里一一对应
 *
 * 铁律：源站返回的东西**永远不可信**。任何字段缺失、类型错、越界，都必须在边界被丢掉，
 * 不许把脏数据带进状态层。
 */
import { z } from 'zod'

export const SourceSchema = z.enum(['netease', 'qq', 'kugou', 'local'])

/** 时长：0 ~ 24h，允许 0（未知），不允许负数/NaN */
const DurationMs = z.number().int().min(0).max(24 * 3600 * 1000)

export const SongSchema = z.object({
  id: z.string().min(1).max(128),
  source: SourceSchema,
  title: z.string().min(1).max(512),
  artist: z.string().max(512).default(''),
  album: z.string().max(512).default(''),
  durationMs: DurationMs.default(0),
  /** 码率 bps；未知为 0 */
  bitrate: z.number().int().min(0).max(20_000_000).default(0),
  /** 音质标签，决定走不走模块 14（重采样）/16（DSD） */
  quality: z.enum(['unknown', '128k', '320k', 'flac', 'hires', 'dsd']).default('unknown'),
  coverUrl: z.string().url().optional(),
  /** 直链（可能过期，落库时只存 1h） */
  url: z.string().url().optional(),
  /** 源站原始字段的指纹，用于去重 */
  fingerprint: z.string().max(128).default(''),
})

export type Song = z.infer<typeof SongSchema>

/**
 * 字段清单：**跨语言生成的唯一来源**。tools/gen-schema.mjs 读它生成
 *   · app/lib/model/song.dart（Dart 侧模型）
 * 改字段只改上面那个 Zod schema + 这里，两边不会再各写一份。
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

export const SearchPayloadSchema = z.object({
  traces: z
    .array(
      z.object({
        source: z.string(),
        elapsedMs: z.number().int().min(0),
        body: z.string(),
      }),
    )
    .max(16),
})
export type SearchPayload = z.infer<typeof SearchPayloadSchema>

/** 一个源站的原始 JSON → 候选 Song[]（各源站字段天差地别，全部在这里收敛） */
export const SourceAdapter = z.object({
  source: SourceSchema,
  /** 把该源站的 body 洗成 Song[]；洗不动就返回空数组，绝不抛到上层 */
  adapt: z.function().args(z.string()).returns(z.array(SongSchema)),
})
