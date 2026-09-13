/* system/update.c —— カーネルの自己更新（「最新を確認」）
 *
 * GitHub（yaskodama/xinu-kernels）の manifest.json と像を、airilab.app が平文 http で
 * 中継してくれる（板は TLS を話せない）:
 *   GET http://airilab.app/api/xinu/manifest              版・大きさ・md5・fnv64
 *   GET http://airilab.app/api/xinu/kernel?board=&off=&len= 像の一部
 *
 * 流れ: 確認 = manifest を取り、自分の build id（kversion.c）と比べる
 *       更新 = 32 KB ずつ RAM に集めて fnv64 と大きさで照合 → 起動媒体の FAT に書く → 再起動
 * 結果は xinu://update の組み込みページ（ブラウザの窓）と GET /update に出す。
 * 通信はブラウザの取得口（browser_fetch_body）を借りる。wm の巡回（browser_poll_pending）から
 * 呼ばれるので、数十秒は画面が止まる。板ごとの違い（板名・媒体・再起動）は下の 3 つの hook。 */

#define UPD_MAX (3u * 1024u * 1024u)
static unsigned char upd_img[UPD_MAX];        /* 集めた像（RAM） */
static unsigned int  upd_len;
static char upd_latest[40], upd_md5[40], upd_fnv[20];
static unsigned int upd_size;
static char upd_state[160] = "not checked";    /* 人が読む状態 */
static int  upd_result = 0;                    /* 0 未確認 1 最新 2 更新あり -1 失敗 */
static int  upd_pending = 0;                   /* 1 確認 2 更新（wm の巡回が消費） */
static long upd_progress;                      /* 集めたバイト数 */

extern const char *kernel_build_id(void);
extern int  browser_fetch_body(const char *url, const char **body, int *len);   /* browser.c */
extern void uart_puts(const char *);
/* 板ごとの hook（loader/main.c か板の側で定義） */
extern const char *update_board_name(void);                                  /* "pi5" / "pi4" */
extern int  update_write_kernel(const unsigned char *img, unsigned int len, char *why, int cap);
extern void update_reboot(void);

static int u_len(const char *s){ int n=0; while(s[n]) n++; return n; }
static int u_eq(const char *a, const char *b, int n){ for(int i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static void u_cpy(char *d, const char *s, int cap){ int i=0; while(s[i]&&i<cap-1){d[i]=s[i];i++;} d[i]=0; }
static void u_cat(char *d, const char *s, int cap){ int o=u_len(d); for(int i=0;s[i]&&o<cap-1;i++) d[o++]=s[i]; d[o]=0; }
static void u_catn(char *d, long v, int cap){ char nb[16]; int k=0; if(v<0){u_cat(d,"-",cap); v=-v;} if(v==0) nb[k++]='0'; while(v>0&&k<15){nb[k++]=(char)('0'+v%10); v/=10;} char t[17]; int o=0; while(k>0) t[o++]=nb[--k]; t[o]=0; u_cat(d,t,cap); }

/* manifest（JSON）から自分の板の項目を読む。"pi5":{"file":..,"size":N,"md5":"..","fnv64":"..","build":".."} */
static int upd_parse(const char *j, int n)
{
    char key[16]; u_cpy(key, "\"", sizeof key); u_cat(key, update_board_name(), sizeof key); u_cat(key, "\"", sizeof key);
    int kl = u_len(key), p = -1;
    for (int i = 0; i + kl < n; i++) if (u_eq(j + i, key, kl)) { p = i + kl; break; }
    if (p < 0) return 0;
    int end = p; int depth = 0;
    for (; end < n; end++) { if (j[end] == '{') depth++; else if (j[end] == '}') { if (--depth == 0) break; } }
    #define FIELD(nm, dst, cap) do { const char *k_ = "\"" nm "\""; int l_ = u_len(k_); (dst)[0] = 0; \
        for (int i = p; i + l_ < end; i++) if (u_eq(j + i, k_, l_)) { int q = i + l_; while (q < end && (j[q]==' '||j[q]==':')) q++; \
            if (q < end && j[q]=='"') { q++; int o=0; while (q < end && j[q] != '"' && o < (cap)-1) (dst)[o++] = j[q++]; (dst)[o]=0; } \
            else { int o=0; while (q < end && j[q] >= '0' && j[q] <= '9' && o < (cap)-1) (dst)[o++] = j[q++]; (dst)[o]=0; } break; } } while (0)
    char sz[16];
    FIELD("build", upd_latest, sizeof upd_latest);
    FIELD("md5", upd_md5, sizeof upd_md5);
    FIELD("fnv64", upd_fnv, sizeof upd_fnv);
    FIELD("size", sz, sizeof sz);
    #undef FIELD
    upd_size = 0; for (int i = 0; sz[i]; i++) upd_size = upd_size * 10 + (unsigned)(sz[i] - '0');
    return upd_latest[0] != 0;
}

int update_check(void)
{
    const char *body; int len;
    upd_result = 0; u_cpy(upd_state, "checking...", sizeof upd_state);
    if (browser_fetch_body("http://airilab.app/api/xinu/manifest", &body, &len) <= 0) {
        u_cpy(upd_state, "check failed: manifest unreachable", sizeof upd_state); upd_result = -1; return -1; }
    if (!upd_parse(body, len)) { u_cpy(upd_state, "check failed: no entry for this board", sizeof upd_state); upd_result = -1; return -1; }
    const char *me = kernel_build_id();
    if (u_eq(me, upd_latest, u_len(upd_latest) + 1)) { u_cpy(upd_state, "up to date", sizeof upd_state); upd_result = 1; }
    else { u_cpy(upd_state, "update available", sizeof upd_state); upd_result = 2; }
    return upd_result;
}

static unsigned long long upd_fnv64(const unsigned char *d, unsigned int n)
{
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (unsigned int i = 0; i < n; i++) { h ^= d[i]; h *= 0x100000001b3ULL; }
    return h;
}

int update_install(void)
{
    if (upd_result != 2) { int r = update_check(); if (r != 2) return r; }
    if (upd_size == 0 || upd_size > UPD_MAX) { u_cpy(upd_state, "install failed: bad size", sizeof upd_state); return -1; }
    upd_len = 0; upd_progress = 0;
    static char url[160];
    while (upd_len < upd_size) {
        unsigned int want = upd_size - upd_len; if (want > 32768) want = 32768;
        u_cpy(url, "http://airilab.app/api/xinu/kernel?board=", sizeof url); u_cat(url, update_board_name(), sizeof url);
        u_cat(url, "&off=", sizeof url); u_catn(url, (long)upd_len, sizeof url); u_cat(url, "&len=", sizeof url); u_catn(url, (long)want, sizeof url);
        const char *body; int len; int got = -1;
        for (int t = 0; t < 3 && got <= 0; t++) { if (browser_fetch_body(url, &body, &len) > 0 && len > 0) got = len; }
        if (got <= 0) { u_cpy(upd_state, "download failed at ", sizeof upd_state); u_catn(upd_state, (long)upd_len, sizeof upd_state); upd_result = -1; return -1; }
        if ((unsigned)got > want) got = (int)want;
        for (int i = 0; i < got; i++) upd_img[upd_len + i] = (unsigned char)body[i];
        upd_len += (unsigned)got; upd_progress = upd_len;
        u_cpy(upd_state, "downloading ", sizeof upd_state); u_catn(upd_state, (long)(upd_len / 1024), sizeof upd_state); u_cat(upd_state, " KB", sizeof upd_state);
    }
    /* 照合: 大きさと fnv64 */
    { unsigned long long h = upd_fnv64(upd_img, upd_len); char hx[17]; const char *hex = "0123456789abcdef";
      for (int i = 15; i >= 0; i--) { hx[i] = hex[h & 15]; h >>= 4; } hx[16] = 0;
      if (!u_eq(hx, upd_fnv, 17)) { u_cpy(upd_state, "verify failed: hash mismatch", sizeof upd_state); upd_result = -1; return -1; } }
    u_cpy(upd_state, "writing to boot media...", sizeof upd_state);
    { char why[96]; why[0] = 0;
      if (update_write_kernel(upd_img, upd_len, why, sizeof why) != 0) {
          u_cpy(upd_state, "write failed: ", sizeof upd_state); u_cat(upd_state, why, sizeof upd_state); upd_result = -1; return -1; } }
    u_cpy(upd_state, "written; rebooting", sizeof upd_state);
    uart_puts("update: kernel written, rebooting\n");
    update_reboot();
    return 0;
}

/* 組み込みページ xinu://update の HTML */
int update_page(char *out, int cap)
{
    out[0] = 0;
    u_cat(out, "<html><body><h1>Kernel update</h1><table>", cap);
    u_cat(out, "<tr><td>Board</td><td>", cap); u_cat(out, update_board_name(), cap); u_cat(out, "</td></tr>", cap);
    u_cat(out, "<tr><td>Running</td><td>", cap); u_cat(out, kernel_build_id(), cap); u_cat(out, "</td></tr>", cap);
    u_cat(out, "<tr><td>Latest (GitHub)</td><td>", cap); u_cat(out, upd_latest[0] ? upd_latest : "?", cap); u_cat(out, "</td></tr>", cap);
    u_cat(out, "<tr><td>Status</td><td><b>", cap); u_cat(out, upd_state, cap); u_cat(out, "</b></td></tr></table>", cap);
    if (upd_result == 2) u_cat(out, "<p><a href=\"xinu://update?install=1\">[ Download and reboot ]</a></p>", cap);
    if (upd_result == 1) u_cat(out, "<p>This board runs the latest kernel.</p>", cap);
    u_cat(out, "<p><a href=\"xinu://update?check=1\">Check again</a> &middot; <a href=\"http://airilab.app/\">Home</a></p>", cap);
    u_cat(out, "<hr><p><small>manifest: http://airilab.app/api/xinu/manifest (GitHub yaskodama/xinu-kernels)</small></p></body></html>", cap);
    return u_len(out);
}
const char *update_state(void) { return upd_state; }
int  update_result(void) { return upd_result; }
void update_request(int what) { upd_pending = what; }
int  update_take_request(void) { int w = upd_pending; upd_pending = 0; return w; }
