/**
 * 【模块6】洗数据：脏 JSON → 干净 Song[]（零依赖、零编译）
 *
 * 入口只有两个：cleanSourceTrace(trace) / cleanSearchPayload(json)
 * 设计取向：**永不抛异常**。任何一条脏数据只是一个"坏样本"，丢掉继续 ——
 * 这条链的出口是 UI 列表，一个源站挂掉不许让整个搜索失败。
 */
import { validateSong, validateSearchPayload } from '../schema/song.mjs'

const MAX_SONGS = 500

function tryParse(body) {
  if (!body || body.length > 8 * 1024 * 1024) return null
  try {
    return JSON.parse(body)
  } catch {
    return null
  }
}

/** 通用兜底：源站结构千奇百怪，先按常见路径捞数组 */
function pickArray(node) {
  if (Array.isArray(node)) return node
  if (node && typeof node === 'object') {
    for (const key of ['songs', 'data', 'result', 'list', 'tracks', 'itemlist']) {
      const v = node[key]
      if (Array.isArray(v)) return v
      if (v && typeof v === 'object') {
        const nested = pickArray(v)
        if (nested.length) return nested
      }
    }
  }
  return []
}

/** 把任意形状的一条记录映射成候选 Song（宽松映射，最终交给 validateSong 裁决） */
function toCandidate(raw, source) {
  if (!raw || typeof raw !== 'object') return null
  const r = raw
  const id = r.id ?? r.songid ?? r.songmid ?? r.hash ?? r.songId
  if (id === undefined || id === null) return null
  const title = r.name ?? r.title ?? r.songname ?? r.songName
  if (!title) return null
  const artists = r.artists ?? r.ar ?? r.singer ?? r.singers
  const artist = Array.isArray(artists)
    ? artists.map((a) => a?.name ?? a ?? '').join(' / ')
    : typeof artists === 'string' ? artists : ''
  const albumObj = r.album ?? r.al
  const durationMs =
    Number(r.duration ?? r.dt ?? r.interval ?? r.timelen ?? 0) || Number(albumObj?.duration ?? 0)
  return {
    id: String(id),
    source,
    title: String(title),
    artist: String(artist).slice(0, 512),
    album: String(typeof albumObj === 'string' ? albumObj : albumObj?.name ?? '').slice(0, 512),
    durationMs: Math.max(0, Math.min(Math.round(durationMs), 24 * 3600 * 1000)),
    bitrate: Math.max(0, Math.min(Number(r.bitrate ?? r.br ?? 0) || 0, 20_000_000)),
    quality: 'unknown',
    coverUrl: normalizeUrl(r.pic ?? r.cover ?? albumObj?.picUrl),
    url: normalizeUrl(r.url),
    fingerprint: `${source}:${id}`.slice(0, 128),
  }
}

function normalizeUrl(u) {
  if (typeof u !== 'string' || !u) return undefined
  if (u.startsWith('//')) return 'https:' + u
  if (!/^https?:\/\//.test(u)) return undefined
  return u.slice(0, 2048)
}

/** 单个源站的原始 body → Song[] */
export function cleanSourceTrace(trace) {
  const parsed = tryParse(trace.body)
  if (!parsed) return []
  const out = []
  for (const raw of pickArray(parsed)) {
    const cand = toCandidate(raw, trace.source)
    if (!cand) continue
    const clean = validateSong(cand)
    if (!clean) continue
    out.push(clean)
    if (out.length >= MAX_SONGS) break
  }
  return out
}

/** 跨源去重：title+artist 归一化后同一首只留码率最高的那条 */
export function dedupe(songs) {
  const rank = { unknown: 0, '128k': 1, '320k': 2, flac: 3, hires: 4, dsd: 5 }
  const best = new Map()
  for (const s of songs) {
    const key = `${s.title.toLowerCase()}|${s.artist.toLowerCase()}`
    const prev = best.get(key)
    if (!prev || rank[s.quality] > rank[prev.quality]) best.set(key, s)
  }
  return [...best.values()]
}

/** 模块5 的整体交接格式 */
export function cleanSearchPayload(json) {
  const traces = validateSearchPayload(tryParse(json))
  if (!traces.length) return []
  const all = []
  for (const t of traces) all.push(...cleanSourceTrace(t))
  return dedupe(all).slice(0, MAX_SONGS)
}
