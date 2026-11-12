-- 示例音源脚本（模块 7 的沙箱 API 演示）
-- 真实落雪源兼容层在 P2 做；这里只验证"白名单内能干完搜歌+取链接"。

local SOURCE = "example"

function search(kw)
  log("search: " .. kw)
  local raw = http_get("https://example.com/api/search?kw=" .. kw)
  local ok, data = pcall(json.decode, raw)
  if not ok or type(data) ~= "table" then
    return {}
  end
  local out = {}
  for _, item in ipairs(data.songs or {}) do
    table.insert(out, {
      id = tostring(item.id),
      title = item.name or "",
      artist = item.singer or "",
      album = item.album or "",
      duration = tonumber(item.duration) or 0,
      source = SOURCE,
    })
  end
  return out
end

function get_url(song)
  local raw = http_get("https://example.com/api/url?id=" .. song.id)
  local ok, data = pcall(json.decode, raw)
  if not ok then return nil end
  return data.url, data.bitrate or 0
end

return { search = search, get_url = get_url }
