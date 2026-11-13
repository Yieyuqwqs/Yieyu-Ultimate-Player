/**
 * 【模块6】歌曲结构 + 手写校验（零依赖：不引 zod，也不编译）。
 *
 * 语义与原来的 Zod 版本一致：类型不对/越界/缺失 → 直接丢样本，永不抛异常。
 * @typedef {'netease'|'qq'|'kugou'|'local'} Source
 * @typedef {'unknown'|'128k'|'320k'|'flac'|'hires'|'dsd'} Quality
 * @typedef {{id:string,source:Source,title:string,artist:string,album:string,
 *            durationMs:number,bitrate:number,quality:Quality,coverUrl?:string,url?:string,
 *            fingerprint:string}} Song
 * @typedef {{source:string, body:string, elapsedMs?:number}} SourceTrace
 */
export { SONG_FIELDS } from './fields.mjs'

const SOURCES = ['netease', 'qq', 'kugou', 'local']
const QUALITIES = ['unknown', '128k', '320k', 'flac', 'hires', 'dsd']

const str = (v, max, fallback = '') =>
  typeof v === 'string' ? v.slice(0, max) : v == null ? fallback : String(v).slice(0, max)

const int = (v, min, max) => {
  const n = typeof v === 'number' ? v : Number(v)
  if (!Number.isFinite(n)) return min
  return Math.max(min, Math.min(max, Math.round(n)))
}

const url = (v) => {
  if (typeof v !== 'string' || !v) return undefined
  if (v.startsWith('//')) return 'https:' + v
  if (!/^https?:\/\//.test(v)) return undefined
  return v.slice(0, 2048)
}

/** 脏记录 → 干净 Song；不合格返回 null（永不抛异常） @returns {Song|null} */
export function validateSong(raw) {
  if (!raw || typeof raw !== 'object') return null
  const id = str(raw.id, 128)
  const title = str(raw.title, 512)
  if (!id || !title) return null
  const source = SOURCES.includes(raw.source) ? raw.source : 'local'
  const quality = QUALITIES.includes(raw.quality) ? raw.quality : 'unknown'
  /** @type {Song} */
  const out = {
    id,
    source,
    title,
    artist: str(raw.artist, 512),
    album: str(raw.album, 512),
    durationMs: int(raw.durationMs, 0, 24 * 3600 * 1000),
    bitrate: int(raw.bitrate, 0, 20_000_000),
    quality,
    fingerprint: str(raw.fingerprint, 128),
  }
  const cover = url(raw.coverUrl)
  if (cover) out.coverUrl = cover
  const u = url(raw.url)
  if (u) out.url = u
  return out
}

/** 模块5 的整体交接格式 {traces:[{source,body}]} @returns {SourceTrace[]} */
export function validateSearchPayload(raw) {
  if (!raw || typeof raw !== 'object') return []
  const traces = raw.traces
  if (!Array.isArray(traces)) return []
  const out = []
  for (const t of traces.slice(0, 16)) {
    if (!t || typeof t !== 'object') continue
    if (typeof t.source !== 'string' || typeof t.body !== 'string') continue
    out.push({ source: t.source, body: t.body.slice(0, 8 * 1024 * 1024), elapsedMs: int(t.elapsedMs, 0, 600_000) })
  }
  return out
}
