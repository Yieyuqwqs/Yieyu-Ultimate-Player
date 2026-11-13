// 路径唯一真相的实现。见 app_paths.h 的规矩。
#include "app_paths.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace {
char g_data[512] = {0};
char g_home[560] = {0};
char g_scripts[600] = {0};
int  g_public = 0;
bool g_inited = false;
}  // namespace

extern "C" int yp_ensure(const char* dir) {
  if (!dir || !*dir) return -1;
  char tmp[700];
  snprintf(tmp, sizeof(tmp), "%s", dir);
  for (char* p = tmp + 1; *p; ++p) {
    if (*p == '/') {
      *p = 0;
      ::mkdir(tmp, 0770);
      *p = '/';
    }
  }
  ::mkdir(tmp, 0770);
  struct stat st{};
  return (::stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

extern "C" int yp_writable(const char* dir) {
  if (!dir || !*dir) return 0;
  char probe[760];
  snprintf(probe, sizeof(probe), "%s/.write_probe", dir);
  FILE* f = fopen(probe, "wb");
  if (!f) return 0;
  const size_t n = fwrite("1", 1, 1, f);
  fclose(f);
  remove(probe);
  return n == 1 ? 1 : 0;
}

extern "C" int yp_init(const char* dataDir) {
  if (g_inited) return g_public;
  g_inited = true;

  snprintf(g_data, sizeof(g_data), "%s", (dataDir && *dataDir) ? dataDir : ".");

  char priv[560];
  snprintf(priv, sizeof(priv), "%s/yieyu", g_data);
  yp_ensure(priv);

  // 公共目录：能建 + **真能写**才算数（只 chmod 过、mount 只读、缺「所有文件访问」都会失败）
  const char* pub = "/storage/emulated/0/yieyu";
  const int pubOk = (yp_ensure(pub) == 0) && yp_writable(pub);

  const char* chosen = pubOk ? pub : priv;
  if (yp_ensure(chosen) != 0) {
    // 连私有目录都建不出来（极罕见）：退到 data 根，至少能跑
    snprintf(g_home, sizeof(g_home), "%s", g_data);
    g_public = 0;
  } else {
    snprintf(g_home, sizeof(g_home), "%s", chosen);
    g_public = pubOk ? 1 : 0;
  }

  snprintf(g_scripts, sizeof(g_scripts), "%s/scripts", g_home);
  yp_ensure(g_scripts);
  return g_public;
}

extern "C" const char* yp_home(void) { return g_home; }
extern "C" const char* yp_scripts(void) { return g_scripts; }
extern "C" const char* yp_data(void) { return g_data; }
extern "C" int yp_is_public(void) { return g_public; }
