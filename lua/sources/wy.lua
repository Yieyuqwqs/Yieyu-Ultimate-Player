-- 内置音源脚本（模块 7）：与模块 5 的 Go 搜歌打同一套接口，这里用沙箱独立实现一份，
-- 证明"脚本链路"是活的：导入 → 注册 → search/get_music_url → 拿直链 → 播放。
--
-- 接口（2026-11-14 实测可用）：gdstudio 聚合 API（GET，无需 key）
--   search: ?types=search&source=netease&name=<kw>&count=20&pages=1
--   url   : ?types=url&source=netease&id=<id>&br=320
-- 沙箱白名单：http_get / http_post / request / fetch / json.* / log / os.* / table.* / tostring

local GD = "https://music-api.gdstudio.xyz/api.php"
local SOURCE = "netease"
local BR = "320"

local function getJson(url)
  local body, code = http_get(url)
  if code ~= 200 or type(body) ~= "string" or body == "" then return nil end
  local ok, data = pcall(json.decode, body)
  if not ok or type(data) ~= "table" then return nil end
  return data
end

function search(kw)
  log("search: " .. tostring(kw))
  local url = GD .. "?types=search&source=" .. SOURCE .. "&name=" .. tostring(kw) .. "&count=20&pages=1"
  local data = getJson(url)
  local out = {}
  if not data then return out end
  for _, it in ipairs(data) do
    table.insert(out, {
      id = tostring(it.id),
      mid = "",
      title = it.name or "",
      artist = it.artist and table.concat(it.artist, " / ") or "",
      album = it.album or "",
      source = "wy",
    })
  end
  return out
end

function get_music_url(source, id, mid)
  local url = GD .. "?types=url&source=" .. SOURCE .. "&id=" .. tostring(id) .. "&br=" .. BR
  local data = getJson(url)
  if not data or type(data.url) ~= "string" then return nil end
  return data.url
end
