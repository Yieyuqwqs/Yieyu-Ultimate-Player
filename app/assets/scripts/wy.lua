-- 内置音源脚本：与模块 5 的 Go 搜歌打同一个接口（这里用模块 7 的沙箱独立实现一份，
-- 用来证明"脚本链路"是活的：导入 → 注册 → 调 search/get_music_url → 拿到直链 → 播放）。
--
-- 沙箱白名单里只有这些能用：http_get / http_post / request / fetch / http.* / json.* / log / os.time / os.date
-- 返回约定（与 Yieyu_ScriptCall 一致）：搜索返回表数组；取地址返回 URL 字符串，失败返回 nil。

local BASE = "https://c.wwwweb.top"
local SOURCE = "wy"
local QUALITY = "master"

local function post(path, payload)
  local body, code = http_post {
    url = BASE .. path,
    method = "POST",
    headers = { ["Content-Type"] = "application/json", ["Accept"] = "application/json" },
    body = json.encode(payload),
  }
  if code ~= 200 or type(body) ~= "string" or body == "" then return nil end
  local ok, data = pcall(json.decode, body)
  if not ok or type(data) ~= "table" then return nil end
  if data.code ~= 200 then return nil end
  return data
end

function search(kw)
  log("search: " .. tostring(kw))
  local data = post("/music/search", { keyword = tostring(kw), page = 1, limit = 20 })
  local out = {}
  if not data or type(data.data) ~= "table" then return out end
  for _, it in ipairs(data.data) do
    table.insert(out, {
      id = tostring(it.id),
      mid = it.mid or "",
      title = it.name or "",
      artist = it.artist or "",
      album = it.album or "",
      source = SOURCE,
    })
  end
  return out
end

function get_music_url(source, id, mid)
  local data = post("/music/url", {
    source = source or SOURCE,
    musicId = tostring(id),
    quality = QUALITY,
  })
  if not data or type(data.url) ~= "string" then return nil end
  return data.url
end
