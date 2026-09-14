/* system/arm.c —— Yahboom DOFBOT のアーム層（I2C スレーブ 0x15 の拡張ボードに電文を書く）
 *
 * 板はサーボに直結していない。0x15 の STM8 が半二重シリアルで 6 サーボを回す。
 * 電文とレジスタは純正 Arm_Lib.py（V1.0.1、2026-09-14 に実物で確認）のとおり:
 *   0x10+id  [pos_H pos_L time_H time_L]   1 軸
 *   0x1E     [time_H time_L] → 0x1D [pos_H pos_L]×6   6 軸一括（先に時間、次に位置）
 *   0x30+id  0 を書いて 3ms 後に 2 バイト読む（上下入替）  現在角
 *   0x1A     1/0 トルク、 0x02 [R G B]、 0x06 ブザー(0.1s 単位, 0=止)、 0x38 ping(0xDA)、 0x01 版
 * 角度→位置: 900 + 2200·θ/180（ID5 だけ 380 + 3320·θ/270）。ID2,3,4 は 180−θ に反転。
 *
 * ここは判断をしない。角度と時間を受け取って書くだけ（レポートの「arm アクタ＝唯一 io を持つ層」）。
 * シェルと HTTP の両方から同じ arm_command() を呼ぶ。 */

#define ARM_ADDR 0x15u

extern int  rp1i2c_present(void);
extern int  rp1i2c_write(unsigned int addr, const unsigned char *buf, int n);
extern int  rp1i2c_write_read(unsigned int addr, const unsigned char *wbuf, int wn, unsigned char *rbuf, int rn);
extern int  rp1i2c_probe(unsigned int addr);
extern void rp1i2c_stats(char *out, int cap);
extern void delay_ms(unsigned int ms);

static unsigned int angle_to_pos(int id, int a)
{
    if (id == 5) { if (a < 0) a = 0; if (a > 270) a = 270; return 380u + (3320u * (unsigned)a) / 270u; }
    if (a < 0) a = 0;
    if (a > 180) a = 180;
    if (id == 2 || id == 3 || id == 4) a = 180 - a;
    return 900u + (2200u * (unsigned)a) / 180u;
}
static int pos_to_angle(int id, unsigned int pos)
{
    int a;
    if (id == 5) { if (pos < 380 || pos > 3700) return -1; a = (int)((270u * (pos - 380u)) / 3320u); }
    else {
        if (pos < 900 || pos > 3100) return -1;
        a = (int)((180u * (pos - 900u)) / 2200u);
        if (id == 2 || id == 3 || id == 4) a = 180 - a;
    }
    return a;
}

/* 1 軸: id 1..6、角度、所要ミリ秒。0 ok */
int arm_write1(int id, int angle, int ms)
{
    if (id < 1 || id > 6) return -4;
    unsigned int pos = angle_to_pos(id, angle);
    unsigned char b[5] = { (unsigned char)(0x10 + id), (unsigned char)(pos >> 8), (unsigned char)pos,
                           (unsigned char)(ms >> 8), (unsigned char)ms };
    int r = rp1i2c_write(ARM_ADDR, b, 5);
    if (r == -2) { delay_ms(5); r = rp1i2c_write(ARM_ADDR, b, 5); }
    return r;
}

/* 6 軸一括。0 ok。I2C の時間切れ（-2）は一度だけやり直す —— 22 手中 1 手が -2 で抜けた実測
   （2026-09-14、pose 180 35 65 0 90 135）。基板の STM8 がサーボと話している最中は応答が遅れる。 */
static int arm_write6_once(const int a[6], int ms)
{
    unsigned char t[3] = { 0x1E, (unsigned char)(ms >> 8), (unsigned char)ms };
    int r = rp1i2c_write(ARM_ADDR, t, 3);
    if (r < 0) return r;
    unsigned char b[13]; b[0] = 0x1D;
    for (int i = 0; i < 6; i++) { unsigned int p = angle_to_pos(i + 1, a[i]); b[1 + i * 2] = (unsigned char)(p >> 8); b[2 + i * 2] = (unsigned char)p; }
    return rp1i2c_write(ARM_ADDR, b, 13);
}
int arm_write6(const int a[6], int ms)
{
    int r = arm_write6_once(a, ms);
    if (r == -2) { delay_ms(5); r = arm_write6_once(a, ms); }
    return r;
}

/* 現在角。-1 = 読めない */
int arm_read(int id)
{
    if (id < 1 || id > 6) return -1;
    unsigned char w[2] = { (unsigned char)(0x30 + id), 0 };
    if (rp1i2c_write(ARM_ADDR, w, 2) < 0) return -1;
    delay_ms(3);
    unsigned char r[2] = { 0, 0 };
    unsigned char reg = (unsigned char)(0x30 + id);
    if (rp1i2c_write_read(ARM_ADDR, &reg, 1, r, 2) < 0) return -1;
    unsigned int pos = ((unsigned int)r[0] << 8) | r[1];   /* Arm_Lib は word を上下入替して使う＝先頭バイトが上位 */
    if (pos == 0) return -1;
    return pos_to_angle(id, pos);
}

/* 基板のマイコンをリセット（0x05）。サーボ側との通信で固まると、書きは ACK されるのに
   読みが全部時間切れになる（2026-09-14 実測）。その状態を read が 3 回続けて見たら自動でこれを打つ。 */
static int g_arm_readfail = 0, g_arm_resets = 0;
int arm_reset_board(void) { unsigned char m[2] = { 0x05, 1 }; g_arm_resets++; g_arm_readfail = 0; int r = rp1i2c_write(ARM_ADDR, m, 2); delay_ms(300); return r; }
int arm_resets(void) { return g_arm_resets; }
int arm_rgb(int r, int g, int b)  { unsigned char m[4] = { 0x02, (unsigned char)r, (unsigned char)g, (unsigned char)b }; return rp1i2c_write(ARM_ADDR, m, 4); }
int arm_buzzer(int tenths)         { unsigned char m[2] = { 0x06, (unsigned char)tenths }; return rp1i2c_write(ARM_ADDR, m, 2); }
int arm_torque(int on)             { unsigned char m[2] = { 0x1A, (unsigned char)(on ? 1 : 0) }; return rp1i2c_write(ARM_ADDR, m, 2); }
int arm_ping(int id)
{
    unsigned char w[2] = { 0x38, (unsigned char)id };
    if (rp1i2c_write(ARM_ADDR, w, 2) < 0) return -1;
    delay_ms(3);
    unsigned char reg = 0x38, v = 0;
    if (rp1i2c_write_read(ARM_ADDR, &reg, 1, &v, 1) < 0) return -1;
    return v;
}
int arm_version(void)
{
    unsigned char w[2] = { 0x01, 1 };
    if (rp1i2c_write(ARM_ADDR, w, 2) < 0) return -1;
    delay_ms(1);
    unsigned char reg = 0x01, v = 0;
    if (rp1i2c_write_read(ARM_ADDR, &reg, 1, &v, 1) < 0) return -1;
    return v;
}

/* ---- 文字列インタフェース（シェル `arm ...` と HTTP /arm?cmd=... が共用） ---- */
static int a_put(char *o, int p, int cap, const char *s) { while (*s && p < cap - 1) o[p++] = *s++; o[p] = 0; return p; }
static int a_putn(char *o, int p, int cap, long v) { char nb[24]; int k = 0; if (v < 0) { p = a_put(o, p, cap, "-"); v = -v; } if (!v) nb[k++] = '0'; while (v) { nb[k++] = (char)('0' + v % 10); v /= 10; } while (k) { if (p < cap - 1) o[p++] = nb[--k]; else k = 0; } o[p] = 0; return p; }
static const char *skipsp(const char *s) { while (*s == ' ' || *s == ',' || *s == '+' || *s == '\t') s++; return s; }
static int a_word(const char **s, char *w, int cap) { const char *p = skipsp(*s); int i = 0; while (*p && *p != ' ' && *p != ',' && *p != '+' && *p != '&' && *p != '\t' && i < cap - 1) w[i++] = *p++; w[i] = 0; *s = p; return i; }
static int a_int(const char **s, int dflt, int *ok) { char w[16]; if (!a_word(s, w, sizeof w)) { *ok = 0; return dflt; } int neg = 0, i = 0, n = 0; if (w[0] == '-') { neg = 1; i = 1; } if (w[i] < '0' || w[i] > '9') { *ok = 0; return dflt; } for (; w[i] >= '0' && w[i] <= '9'; i++) n = n * 10 + (w[i] - '0'); *ok = 1; return neg ? -n : n; }
static int a_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == 0 && *b == 0; }

/* 例: "pose 90 90 90 90 90 30 1000" / "set 1 120 500" / "read" / "rgb 0 255 0" / "buzz 3" /
 *     "torque 0" / "ping 1" / "ver" / "stat" / "scan"。結果を out に書き、0/負を返す。 */
int arm_command(const char *args, char *out, int cap)
{
    char cmd[16]; int p = 0, ok;
    const char *s = args;
    a_word(&s, cmd, sizeof cmd);
    if (!rp1i2c_present() && !a_eq(cmd, "stat") && !a_eq(cmd, "help") && cmd[0]) {
        a_put(out, 0, cap, "arm: no I2C bus on this board (RP1 i2c1 absent)\n"); return -3; }
    if (a_eq(cmd, "pose")) {
        int a[6];
        for (int i = 0; i < 6; i++) { a[i] = a_int(&s, 90, &ok); if (!ok) { a_put(out, 0, cap, "usage: arm pose a1 a2 a3 a4 a5 a6 [ms]\n"); return -4; } }
        int ms = a_int(&s, 1000, &ok);
        int r = arm_write6(a, ms);
        p = a_put(out, p, cap, r == 0 ? "ok pose" : "FAIL pose rc="); if (r) p = a_putn(out, p, cap, r);
        for (int i = 0; i < 6; i++) { p = a_put(out, p, cap, " "); p = a_putn(out, p, cap, a[i]); }
        p = a_put(out, p, cap, " ms="); p = a_putn(out, p, cap, ms); a_put(out, p, cap, "\n"); return r;
    }
    if (a_eq(cmd, "set")) {
        int id = a_int(&s, 0, &ok); int ang = a_int(&s, 90, &ok); int ms = a_int(&s, 1000, &ok);
        int r = arm_write1(id, ang, ms);
        p = a_put(out, p, cap, r == 0 ? "ok set " : "FAIL set "); p = a_putn(out, p, cap, id); p = a_put(out, p, cap, " -> "); p = a_putn(out, p, cap, ang);
        p = a_put(out, p, cap, " ms="); p = a_putn(out, p, cap, ms); if (r) { p = a_put(out, p, cap, " rc="); p = a_putn(out, p, cap, r); } a_put(out, p, cap, "\n"); return r;
    }
    if (a_eq(cmd, "read")) {
        int nfail = 0;
        p = a_put(out, p, cap, "angles");
        for (int id = 1; id <= 6; id++) { int a = arm_read(id); p = a_put(out, p, cap, " "); if (a < 0) { nfail++; p = a_put(out, p, cap, "?"); } else p = a_putn(out, p, cap, a); }
        if (nfail == 6) { if (++g_arm_readfail >= 3) { arm_reset_board(); p = a_put(out, p, cap, " (board reset)"); } }
        else g_arm_readfail = 0;
        a_put(out, p, cap, "\n"); return 0;
    }
    if (a_eq(cmd, "reset"))  { int rc = arm_reset_board(); a_put(out, 0, cap, rc == 0 ? "ok reset (board restarting)\n" : "FAIL reset\n"); return rc; }
    if (a_eq(cmd, "rgb"))    { int r = a_int(&s, 0, &ok), g = a_int(&s, 0, &ok), b = a_int(&s, 0, &ok); int rc = arm_rgb(r, g, b); p = a_put(out, 0, cap, rc == 0 ? "ok rgb\n" : "FAIL rgb\n"); return rc; }
    if (a_eq(cmd, "buzz"))   { int n = a_int(&s, 3, &ok); int rc = arm_buzzer(n); a_put(out, 0, cap, rc == 0 ? "ok buzz\n" : "FAIL buzz\n"); return rc; }
    if (a_eq(cmd, "torque")) { int on = a_int(&s, 1, &ok); int rc = arm_torque(on); a_put(out, 0, cap, rc == 0 ? "ok torque\n" : "FAIL torque\n"); return rc; }
    if (a_eq(cmd, "ping"))   { int id = a_int(&s, 1, &ok); int v = arm_ping(id); p = a_put(out, 0, cap, "ping "); p = a_putn(out, p, cap, id); p = a_put(out, p, cap, " -> "); p = a_putn(out, p, cap, v); a_put(out, p, cap, v == 0xDA ? " (ok)\n" : " (no servo)\n"); return v == 0xDA ? 0 : -1; }
    if (a_eq(cmd, "ver"))    { int v = arm_version(); p = a_put(out, 0, cap, "board version 0."); p = a_putn(out, p, cap, v); a_put(out, p, cap, "\n"); return v < 0 ? -1 : 0; }
    if (a_eq(cmd, "scan")) {
        p = a_put(out, 0, cap, "i2c scan:"); int n = 0;
        for (unsigned int a = 0x08; a <= 0x77; a++) {
            if (!rp1i2c_probe(a)) continue;
            const char *hx = "0123456789abcdef"; char h[3] = { hx[a >> 4], hx[a & 15], 0 };
            p = a_put(out, p, cap, " 0x"); p = a_put(out, p, cap, h); n++;
        }
        if (!n) p = a_put(out, p, cap, " (none)");
        a_put(out, p, cap, "\n"); return 0;
    }
    if (a_eq(cmd, "stat"))   { rp1i2c_stats(out, cap); return 0; }
    a_put(out, 0, cap, "arm: pose a1..a6 [ms] | set id ang [ms] | read | rgb r g b | buzz n | torque 0|1 | ping id | ver | scan | stat | reset\n");
    return cmd[0] ? -4 : 0;
}

/* ---- 起動時に自動で載せるアクター（~/dofbot_pi5/aipl/dofbot_arm.aipl と同じ） ----
   再起動のたびに POST /cc し直さなくても remote_call("…:9010","dofbot","cmd",…) が通るように。 */
const char dofbot_arm_aipl[] =
"class Arm {\n"
"  var count = 0;\n"
"  method cmd(s: string) : string !{io, mut} {\n"
"    count = count + 1;\n"
"    reply(arm_cmd(s));\n"
"  }\n"
"  method served() : int !{} { reply(count); }\n"
"}\n"
"var arm = new Arm();\n"
"web_expose(\"/dofbot\", \"arm\");\n";
int dofbot_arm_aipl_len(void) { int n = 0; while (dofbot_arm_aipl[n]) n++; return n; }
