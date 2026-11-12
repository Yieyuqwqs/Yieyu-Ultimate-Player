/**
 * 【模块 6】Axios · 只发请求，不解析
 *
 * 边界：这里不 import 任何 schema，也不做任何字段访问。
 * 拿到的响应体原样交给 clean/ 层。
 */
import axios, { type AxiosInstance } from 'axios'

let instance: AxiosInstance | null = null

export function http(): AxiosInstance {
  if (instance) return instance
  instance = axios.create({
    timeout: 8_000,
    maxContentLength: 8 * 1024 * 1024, // 单响应上限 8MB，防被灌爆
    maxRedirects: 3,
    headers: {
      'User-Agent': 'YieyuUltimate/0.1 (Android)',
      Accept: 'application/json,*/*',
    },
  })
  // 只记录"网络层事实"，不打 body（用户隐私 + 日志体积）
  instance.interceptors.response.use(
    (r) => r,
    (e) => {
      const status = e?.response?.status ?? -1
      throw Object.assign(new Error(`http ${status}`), { status })
    },
  )
  return instance
}

/** 音频流不走 axios（那是模块 9 的 libcurl）；这里只允许 JSON/短请求。 */
export async function getJsonRaw(url: string): Promise<string> {
  const r = await http().get<string>(url, { responseType: 'text', transformResponse: (x) => x })
  return typeof r.data === 'string' ? r.data : JSON.stringify(r.data)
}
