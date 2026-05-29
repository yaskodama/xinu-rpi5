// llm/llm.c — a minimal on-device LLM: load a baked llama2.c model and run
// transformer inference (Llama-2 architecture) to generate text.  This is the
// "Ollama-like loader" at tiny scale: the model (Karpathy stories260K, legacy
// llama2.c format with freq_cis-in-file + grouped-query attention + shared
// classifier) is baked into the kernel image (llm/blob.S) and run here.
//
// Faithful port of the reference forward pass (validated in Python).  All math
// is float; this file is built WITHOUT -mgeneral-regs-only (FP enabled at EL1).
// The kernel runs with the D-cache OFF, so inference is slow — fine for a tiny
// model, but generation yields to the scheduler each token so it never starves
// the network for long.

#include "uart.h"
#include "kmalloc.h"
#include "proc.h"

/* Generation is slow with the D-cache off, and the GENET RX ring is pumped
 * from the wm/main loop (which we block while generating).  Drain the ring
 * between tokens so a busy link can't overflow it and wedge the NIC. */
/* weak: targets without the GENET driver (QEMU/Pi5) resolve these to 0 */
extern int  genet_rx_poll(unsigned char **out_pkt) __attribute__((weak));
extern void genet_rx_release(void) __attribute__((weak));
static void net_drain(void)
{
    if (!genet_rx_poll || !genet_rx_release) return;
    unsigned char *p; int n = 0;
    while (n++ < 64 && genet_rx_poll(&p) > 0) genet_rx_release();
}

extern const unsigned char model_data[];
extern const unsigned char tok_data[];
/* weak: HDMI runtime-monitor heartbeat (tcp_server.c); no-op on targets
 * built without it so QEMU/Pi5 still link. */
extern void app_beat(void) __attribute__((weak));

/* ---------- tiny float helpers (no libm) ---------- */
/* emit the AArch64 FP sqrt directly (no sqrtf libcall in this freestanding build) */
static float l_sqrtf(float x) { float r; __asm__ ("fsqrt %s0, %s1" : "=w"(r) : "w"(x)); return r; }

/* exp via range reduction: e^x = 2^k * e^r, r in [-ln2/2, ln2/2]. */
static double l_exp(double x)
{
    if (x >  700.0) return 1e300;
    if (x < -700.0) return 0.0;
    const double LN2 = 0.6931471805599453, LOG2E = 1.4426950408889634;
    long k = (long)(x * LOG2E + (x >= 0 ? 0.5 : -0.5));
    double r = x - (double)k * LN2;
    /* degree-6 polynomial for e^r on the reduced range */
    double p = 1.0 + r*(1.0 + r*(0.5 + r*(0.16666666666666666
              + r*(0.041666666666666664 + r*(0.008333333333333333
              + r*0.001388888888888889)))));
    union { unsigned long u; double d; } sc;
    sc.u = (unsigned long)(k + 1023) << 52;     /* 2^k */
    return p * sc.d;
}

/* ---------- model config + weights ---------- */
typedef struct {
    int dim, hidden_dim, n_layers, n_heads, n_kv_heads, vocab_size, seq_len;
} Config;

typedef struct {
    const float *tok_emb;     /* (vocab, dim) — also the shared classifier */
    const float *rms_att;     /* (layer, dim) */
    const float *wq, *wk, *wv, *wo;
    const float *rms_ffn;     /* (layer, dim) */
    const float *w1, *w2, *w3;
    const float *rms_final;   /* (dim,) */
    const float *freq_cr, *freq_ci;  /* (seq_len, head_size/2) */
} Weights;

typedef struct {
    float *x, *xb, *xb2, *hb, *hb2, *q, *k, *v, *att, *logits;
    float *key_cache, *value_cache;   /* (layer, seq, kv_dim) */
} Run;

static Config   C;
static Weights  W;
static Run      R;
static int      g_loaded;
static int      g_drain;       /* drain RX between layers? (serial: yes; HTTP: no — would re-enter genet_rx_tick) */
static void net_drain(void);   /* fwd: keep the NIC RX ring drained while we compute */

/* tokenizer: vocab pieces + merge scores parsed from tok_data */
static const char *g_piece[1024];
static int         g_plen[1024];
static float       g_score[1024];

static int rd_i32(const unsigned char *p) { return (int)(p[0]|(p[1]<<8)|(p[2]<<16)|((unsigned)p[3]<<24)); }

static void load(void)
{
    if (g_loaded) return;
    const unsigned char *p = model_data;
    C.dim = rd_i32(p+0);  C.hidden_dim = rd_i32(p+4); C.n_layers = rd_i32(p+8);
    C.n_heads = rd_i32(p+12); C.n_kv_heads = rd_i32(p+16);
    C.vocab_size = rd_i32(p+20); C.seq_len = rd_i32(p+24);

    int dim = C.dim, L = C.n_layers, hs = dim / C.n_heads;
    int kv_dim = C.n_kv_heads * hs, hidden = C.hidden_dim, vocab = C.vocab_size;
    const float *f = (const float *)(model_data + 28);
    W.tok_emb = f;            f += (long)vocab * dim;
    W.rms_att = f;            f += (long)L * dim;
    W.wq = f;                 f += (long)L * dim * (C.n_heads * hs);
    W.wk = f;                 f += (long)L * dim * kv_dim;
    W.wv = f;                 f += (long)L * dim * kv_dim;
    W.wo = f;                 f += (long)L * (C.n_heads * hs) * dim;
    W.rms_ffn = f;            f += (long)L * dim;
    W.w1 = f;                 f += (long)L * hidden * dim;
    W.w2 = f;                 f += (long)L * dim * hidden;
    W.w3 = f;                 f += (long)L * hidden * dim;
    W.rms_final = f;          f += dim;
    W.freq_cr = f;            f += (long)C.seq_len * (hs / 2);
    W.freq_ci = f;            f += (long)C.seq_len * (hs / 2);

    R.x      = kmalloc(dim * sizeof(float));
    R.xb     = kmalloc(dim * sizeof(float));
    R.xb2    = kmalloc(dim * sizeof(float));
    R.hb     = kmalloc(hidden * sizeof(float));
    R.hb2    = kmalloc(hidden * sizeof(float));
    R.q      = kmalloc(dim * sizeof(float));
    R.k      = kmalloc(kv_dim * sizeof(float));
    R.v      = kmalloc(kv_dim * sizeof(float));
    R.att    = kmalloc(C.seq_len * sizeof(float));
    R.logits = kmalloc(vocab * sizeof(float));
    R.key_cache   = kmalloc((long)L * C.seq_len * kv_dim * sizeof(float));
    R.value_cache = kmalloc((long)L * C.seq_len * kv_dim * sizeof(float));

    /* tokenizer */
    const unsigned char *t = tok_data; (void)rd_i32(t); t += 4;  /* max_token_length */
    for (int i = 0; i < vocab && i < 1024; i++) {
        union { unsigned int u; float f; } sc; sc.u = (unsigned int)rd_i32(t); t += 4;
        g_score[i] = sc.f;
        int len = rd_i32(t); t += 4;
        g_piece[i] = (const char *)t; g_plen[i] = len; t += len;
    }
    g_loaded = 1;
}

static void rmsnorm(float *o, const float *x, const float *w, int n)
{
    float ss = 0.0f; for (int i = 0; i < n; i++) ss += x[i]*x[i];
    ss = ss / n + 1e-5f; float r = 1.0f / l_sqrtf(ss);
    for (int i = 0; i < n; i++) o[i] = w[i] * (r * x[i]);
}

static void matmul(float *o, const float *x, const float *w, int n, int d)
{
    for (int i = 0; i < d; i++) {
        float s = 0.0f; const float *row = w + (long)i * n;
        for (int j = 0; j < n; j++) s += row[j] * x[j];
        o[i] = s;
    }
}

static float *forward(int token, int pos)
{
    int dim = C.dim, H = C.n_heads, KVH = C.n_kv_heads, hs = dim / H;
    int kv_dim = KVH * hs, kv_mul = H / KVH, hidden = C.hidden_dim, L = C.n_layers;
    float *x = R.x;
    for (int i = 0; i < dim; i++) x[i] = W.tok_emb[(long)token*dim + i];
    const float *fcr = W.freq_cr + pos*(hs/2);
    const float *fci = W.freq_ci + pos*(hs/2);

    for (int l = 0; l < L; l++) {
        rmsnorm(R.xb, x, W.rms_att + (long)l*dim, dim);
        matmul(R.q, R.xb, W.wq + (long)l*dim*dim, dim, dim);
        matmul(R.k, R.xb, W.wk + (long)l*dim*kv_dim, dim, kv_dim);
        matmul(R.v, R.xb, W.wv + (long)l*dim*kv_dim, dim, kv_dim);
        /* RoPE on q (all heads) and k (kv heads) */
        for (int h = 0; h < H; h++)
            for (int i = 0; i < hs; i += 2) {
                float c = fcr[i/2], s = fci[i/2]; int b = h*hs + i;
                float a0 = R.q[b], a1 = R.q[b+1];
                R.q[b] = a0*c - a1*s; R.q[b+1] = a0*s + a1*c;
            }
        for (int h = 0; h < KVH; h++)
            for (int i = 0; i < hs; i += 2) {
                float c = fcr[i/2], s = fci[i/2]; int b = h*hs + i;
                float a0 = R.k[b], a1 = R.k[b+1];
                R.k[b] = a0*c - a1*s; R.k[b+1] = a0*s + a1*c;
            }
        float *kc = R.key_cache + ((long)l*C.seq_len + pos)*kv_dim;
        float *vc = R.value_cache + ((long)l*C.seq_len + pos)*kv_dim;
        for (int i = 0; i < kv_dim; i++) { kc[i] = R.k[i]; vc[i] = R.v[i]; }

        for (int i = 0; i < dim; i++) R.xb[i] = 0.0f;
        for (int h = 0; h < H; h++) {
            int kvh = h / kv_mul;
            const float *qh = R.q + h*hs;
            for (int t = 0; t <= pos; t++) {
                const float *kt = R.key_cache + ((long)l*C.seq_len + t)*kv_dim + kvh*hs;
                float sc = 0.0f; for (int i = 0; i < hs; i++) sc += qh[i]*kt[i];
                R.att[t] = sc / l_sqrtf((float)hs);
            }
            /* softmax over att[0..pos] */
            float mx = R.att[0]; for (int t = 1; t <= pos; t++) if (R.att[t] > mx) mx = R.att[t];
            float sum = 0.0f;
            for (int t = 0; t <= pos; t++) { R.att[t] = (float)l_exp(R.att[t]-mx); sum += R.att[t]; }
            for (int t = 0; t <= pos; t++) R.att[t] /= sum;
            float *xbh = R.xb + h*hs;
            for (int t = 0; t <= pos; t++) {
                const float *vt = R.value_cache + ((long)l*C.seq_len + t)*kv_dim + kvh*hs;
                float a = R.att[t];
                for (int i = 0; i < hs; i++) xbh[i] += a*vt[i];
            }
        }
        matmul(R.xb2, R.xb, W.wo + (long)l*dim*dim, dim, dim);
        for (int i = 0; i < dim; i++) x[i] += R.xb2[i];

        rmsnorm(R.xb, x, W.rms_ffn + (long)l*dim, dim);
        matmul(R.hb,  R.xb, W.w1 + (long)l*hidden*dim, dim, hidden);
        matmul(R.hb2, R.xb, W.w3 + (long)l*hidden*dim, dim, hidden);
        for (int i = 0; i < hidden; i++) {
            float val = R.hb[i];
            val = val * (1.0f / (1.0f + (float)l_exp(-val)));   /* SiLU */
            R.hb[i] = val * R.hb2[i];
        }
        matmul(R.xb, R.hb, W.w2 + (long)l*dim*hidden, hidden, dim);
        for (int i = 0; i < dim; i++) x[i] += R.xb[i];
        if (g_drain) net_drain();   /* bound the un-drained window to one layer */
    }
    rmsnorm(x, x, W.rms_final, dim);
    matmul(R.logits, x, W.tok_emb, dim, C.vocab_size);   /* shared classifier */
    return R.logits;
}

static int argmax(const float *v, int n)
{
    int mi = 0; float mv = v[0];
    for (int i = 1; i < n; i++) if (v[i] > mv) { mv = v[i]; mi = i; }
    return mi;
}

/* ---------- tokenizer encode (BPE greedy merge), output sink ---------- */
static int piece_eq(int id, const char *s, int len)
{
    if (g_plen[id] != len) return 0;
    for (int i = 0; i < len; i++) if (g_piece[id][i] != s[i]) return 0;
    return 1;
}
static int find_piece(const char *s, int len)
{
    for (int i = 0; i < C.vocab_size; i++) if (piece_eq(i, s, len)) return i;
    return -1;
}

/* emit a token's text (llama2.c: strip a leading space right after BOS) */
static void (*g_sink)(char);
static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void emit_piece(int prev, int tok)
{
    const char *p = g_piece[tok]; int n = g_plen[tok];
    int i = 0;
    if (prev == 1 && n > 0 && p[0] == ' ') i = 1;   /* drop BOS leading space */
    /* byte fallback: a piece of the form "<0xHH>" is one raw byte */
    if (n == 6 && p[0]=='<' && p[1]=='0' && p[2]=='x' && p[5]=='>') {
        int hi = hexv(p[3]), lo = hexv(p[4]);
        if (hi >= 0 && lo >= 0) { g_sink((char)((hi << 4) | lo)); return; }
    }
    for (; i < n; i++) g_sink(p[i]);
}

/* generate up to `steps` tokens from `prompt`; greedy decode.  `drain`=1 pumps
 * the NIC RX ring between layers (safe from the serial/wm context); pass 0 from
 * the HTTP path, which already runs inside genet_rx_tick (re-entering it would
 * corrupt the RX ring). */
/* BPE-encode `prompt`: a leading space + the text as single chars, greedily
 * merged by score.  Optionally prefixes BOS=1.  Returns the token count. */
static int bpe_encode(const char *prompt, int *toks, int maxtoks, int with_bos)
{
    int nt = 0;
    if (with_bos) toks[nt++] = 1;                     /* BOS */
    char tmp[260]; int tn = 0; tmp[tn++] = ' ';
    if (prompt) for (int i = 0; prompt[i] && tn < 256; i++) tmp[tn++] = prompt[i];
    for (int i = 0; i < tn && nt < maxtoks; i++) {
        int id = find_piece(&tmp[i], 1);
        if (id >= 0) toks[nt++] = id;
    }
    for (;;) {                                        /* greedy BPE merges */
        float best = -1e30f; int bi = -1, bid = -1;
        char buf[16];
        for (int i = 0; i < nt-1; i++) {
            int la = g_plen[toks[i]], lb = g_plen[toks[i+1]];
            if (la + lb > 15) continue;
            int k = 0; for (int j=0;j<la;j++) buf[k++]=g_piece[toks[i]][j];
            for (int j=0;j<lb;j++) buf[k++]=g_piece[toks[i+1]][j];
            int id = find_piece(buf, k);
            if (id >= 0 && g_score[id] > best) { best = g_score[id]; bi = i; bid = id; }
        }
        if (bi < 0) break;
        toks[bi] = bid;
        for (int i = bi+1; i < nt-1; i++) toks[i] = toks[i+1];
        nt--;
    }
    return nt;
}

int llm_generate(const char *prompt, int max_new, void (*sink)(char), int drain, int echo)
{
    load();
    g_sink = sink;
    g_drain = drain;

    static int toks[256];
    int nt = bpe_encode(prompt, toks, 250, 1);        /* with BOS */

    int token = toks[0], pos = 0, gen = 0;
    while (pos < C.seq_len && gen < max_new) {
        float *logits = forward(token, pos);
        int next;
        if (pos + 1 < nt) {                           /* still feeding the prompt */
            next = toks[pos+1];
            if (echo) emit_piece(token, next);
        } else {                                      /* generating new tokens */
            next = argmax(logits, C.vocab_size);
            if (next == 1 || next == 2) break;        /* BOS/EOS -> stop */
            emit_piece(token, next);                  /* `token` precedes `next` */
            gen++;
        }
        token = next; pos++;
        if (app_beat) app_beat();                     /* runtime-monitor heartbeat (per token) */
        if (g_drain) net_drain();                     /* keep the RX ring from overflowing */
    }
    return gen;
}

/* HTTP entry point: generate into `out` (text/plain), no RX draining (we are
 * already inside genet_rx_tick).  Returns tokens produced. */
static char *g_out; static int g_outcap, g_outlen;
static void buf_sink(char c) { if (g_outlen < g_outcap - 1) g_out[g_outlen++] = c; }
int llm_run(const char *prompt, int max_new, char *out, int outcap, int echo)
{
    g_out = out; g_outcap = outcap; g_outlen = 0; if (outcap > 0) out[0] = 0;
    int n = llm_generate(prompt, max_new, buf_sink, 0, echo);
    g_out[(g_outlen < g_outcap) ? g_outlen : (g_outcap - 1)] = 0;
    g_out = 0;
    return n;
}

/* ---------- stateful chat session ----------
 * The KV cache (R.key_cache/value_cache) and position persist across turns,
 * so each turn only processes the new message + the generated tokens — no
 * re-encoding of the whole conversation.  Cost is ~constant per turn instead
 * of growing.  llm_session_reset() starts a fresh conversation. */
static int g_sess_pos;     /* next free position in the KV cache */
static int g_sess_last;    /* last token fed/generated (for space handling) */
void llm_session_reset(void) { load(); g_sess_pos = 0; g_sess_last = 1; }

int llm_chat(const char *msg, int max_new, char *out, int outcap)
{
    load();
    g_sink = buf_sink;                   /* emit_piece writes here */
    g_out = out; g_outcap = outcap; g_outlen = 0; if (outcap > 0) out[0] = 0;
    g_drain = 0;                         /* runs inside genet_rx_tick */

    float *logits = 0;
    int prev = g_sess_last;
    if (g_sess_pos == 0) {               /* new session: feed BOS at pos 0 */
        logits = forward(1, g_sess_pos); g_sess_pos++; prev = 1;
    }
    static int mt[256];
    int nt = bpe_encode(msg, mt, 250, 0);            /* message tokens, no BOS */
    for (int i = 0; i < nt && g_sess_pos < C.seq_len; i++) {
        logits = forward(mt[i], g_sess_pos); g_sess_pos++; prev = mt[i];
    }
    if (!logits) { g_out = 0; return 0; }            /* nothing to predict from */

    int gen = 0;
    while (gen < max_new && g_sess_pos < C.seq_len) {
        int next = argmax(logits, C.vocab_size);
        if (next == 1 || next == 2) break;           /* BOS/EOS */
        emit_piece(prev, next);
        prev = next; gen++;
        logits = forward(next, g_sess_pos); g_sess_pos++;
    }
    g_sess_last = prev;
    g_out[(g_outlen < g_outcap) ? g_outlen : (g_outcap - 1)] = 0;
    g_out = 0;
    return gen;
}

/* ---------- shell command: `llm [prompt...]` ---------- */
static void uart_sink(char c) { uart_putc(c); }

int cmd_llm(int argc, char **argv)
{
    static char prompt[200];
    int p = 0;
    for (int a = 1; a < argc && p < (int)sizeof(prompt)-1; a++) {
        if (a > 1 && p < (int)sizeof(prompt)-1) prompt[p++] = ' ';
        for (char *s = argv[a]; *s && p < (int)sizeof(prompt)-1; s++) prompt[p++] = *s;
    }
    prompt[p] = 0;
    load();
    uart_puts("llm: stories260K (dim=");
    char b[12]; int n=0,v=C.dim; if(!v)b[n++]='0'; while(v){b[n++]=(char)('0'+v%10);v/=10;} while(n--)uart_putc(b[n]);
    uart_puts(", "); n=0; v=C.n_layers; if(!v)b[n++]='0'; while(v){b[n++]=(char)('0'+v%10);v/=10;} while(n--)uart_putc(b[n]);
    uart_puts(" layers)\n");
    llm_generate(p > 0 ? prompt : (const char *)0, 64, uart_sink, 1, 1);  /* echo prompt + 64 new */
    uart_putc('\n');
    return 0;
}
