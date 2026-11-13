/**
 * 【模块6】歌曲结构定义（**零依赖**：不引 zod，校验代码手写）。
 *
 * 为什么把 zod 拔了：为了一个字段校验引一个运行时库，是"外行的依赖"。这里用
 * 30 行手写校验替代：类型不对/越界/缺失就直接丢样本，语义与原来一致。
 *
 * 字段清单的唯一真源在 ./fields.ts（纯数据，构建期生成 Dart 模型用）。
 */
export { SONG_FIELDS } from './fields.js'

export type Source = 'netease' | 'qq' | 'kugou' | 'local'
export type Quality = 'unknown' | '128k' | '320k' | 'flac' | 'hires' | 'dsd'

export interface Song {
  id: string
  source: Source
  title: string
  artist: string
  album: string
  durationMs: number
  bitrate: number
  quality: Quality
  coverUrl?: string
  url?: string
  fingerprint: string
}

export interface SourceTrace {
  source: string
  body: string
  elapsedMs?: number
}

const SOURCES: Source[] = ['netease', 'qq', 'kugou', 'local']
const QUALITIES: Quality[] = ['unknown', '128k', '320k', 'flac', 'hires', 'dsd']

const str = (v: unknown, max: number, fallback = ''): string =>
  typeof v === 'string' ? v.slice(0, max) : v == null ? fallback : String(v).slice(0, max)

const int = (v: unknown, min: number, max: number): number => {
  const n = typeof v === 'number' ? v : Number(v)
  if (!Number.isFinite(n)) return min
  return Math.max(min, Math.min(max, Math.round(n)))
}

const url = (v: unknown): string | undefined => {
  if (typeof v !== 'string' || !v) return undefined
  if (v.startsWith('//')) return 'https:' + v
  if (!/^https?:\/\//.test(v)) return undefined
  return v.slice(0, 2048)
}

/** 脏记录 → 干净 Song；不合格返回 null（永不抛异常） */
export function validateSong(raw: unknown): Song | null {
  if (!raw || typeof raw !== 'object') return null
  const r = raw as Record<string, unknown>
  const id = str(r.id, 128)
  const title = str(r.title, 512)
  if (!id || !title) return null
  const source = SOURCES.includes(r.source as Source) ? (r.source as Source) : 'local'
  const quality = QUALITIES.includes(r.quality as Quality) ? (r.quality as Quality) : 'unknown'
  return {
    id,
    source,
    title,
    artist: str(r.artist, 512),
    album: str(r.album, 512),
    durationMs: int(r.durationMs, 0, 24 * 3600 * 1000),
    bitrate: int(r.bitrate, 0, 20_000_000),
    quality,
    ...(url(r.coverUrl) ? { coverUrl: url(r.coverUrl) } : {}),
    ...(url(r.url) ? { url: url(r.url) } : {}),
    fingerprint: str(r.fingerprint, 128),
  }
}

/** 模块 5 的整体交接格式：{ traces: [{source, body, elapsedMs}] } */
export function validateSearchPayload(raw: unknown): SourceTrace[] {
  if (!raw || typeof raw !== 'object') return []
  const traces = (raw as Record<string, unknown>).traces
  if (!Array.isArray(traces)) return []
  const out: SourceTrace[] = []
  for (const t of traces.slice(0, 16)) {
    if (!t || typeof t !== 'object') continue
    const r = t as Record<string, unknown>
    if (typeof r.source !== 'string' || typeof r.body !== 'string') continue
    out.push({
      source: r.source,
      body: r.body.slice(0, 8 * 1024 * 1024),
      elapsedMs: int(r.elapsedMs, 0, 600_000),
    })
  }
  return out
}
