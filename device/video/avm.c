// device/video/avm.c — AIPL actor-bytecode VM ("AVM") + a "Blender" polygon
// display for the Pi 4 kernel.  Loads an .avm module (magic AVM1) posted over
// HTTP (POST /actor/loadvm, chunked like /chainload), runs its actors on a
// dedicated kernel thread, and draws their cls()/line()/tri() output into a
// "VM graphics" window — the same opcodes + 16-colour palette as the Pi 3
// (xinu-raz) so the SAME compiled .avm renders identically on both boards.
//
// Self-contained: it does NOT use the kernel's actor.c — it carries its own
// tiny object table + FIFO message queue + scheduler, which is all the AVM
// send/wait/spawn model needs.

#include "video.h"
#include "wm.h"
#include "smp.h"
/* rpi5 port of the Pi4 "Blender display system" (device/video/avm.c).
 * Same AVM bytecode + 16-colour palette + tri()/line()/cls() + AVM2 binary mesh
 * as the Pi4, so the SAME compiled .avm renders identically.  The Pi4's
 * launch-from-SD / USB-MSD / RAM-app-library paths are dropped here (rpi5 has no
 * FAT32/usbmsd layer yet); actors arrive only via POST /actor/loadvm and the VM
 * graphics window opens automatically. */

/* ---- small freestanding helpers (no libc) ---- */
/* The VM is driven COOPERATIVELY from the WM redraw (avm_tick, one VM frame per
 * WM frame) — no separate thread, no busy-wait — so it can never starve the WM
 * or network.  vm_frame_done marks the WAIT (frame boundary) that ends a tick. */
static volatile int vm_frame_done;
static int          vm_wait_ms;             /* the WAIT() that ended the last frame */
static void avm_tick(void);                 /* forward decl (used by vmgfx_draw) */
void        avm_ctl(int cmd);               /* forward decl (used by vmgfx_click) */
static int  a_abs(int v) { return v < 0 ? -v : v; }
static unsigned long avm_now_ms(void)
{
    unsigned long freq, cnt;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(cnt));
    return freq ? (cnt / (freq / 1000UL)) : 0;
}
static int  a_streq(const char *x, const char *y)
{ while (*x && *y) { if (*x != *y) return 0; x++; y++; } return *x == *y; }
static unsigned vm_u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static int      vm_i32(const unsigned char *p)
{ return (int)(p[0] | (p[1]<<8) | (p[2]<<16) | ((unsigned)p[3]<<24)); }
static unsigned vm_u32(const unsigned char *p)
{ return p[0] | (p[1]<<8) | (p[2]<<16) | ((unsigned)p[3]<<24); }
static int vm_i16(const unsigned char *p)
{ int v = p[0] | (p[1]<<8); return (v & 0x8000) ? v - 0x10000 : v; }

/* ===== AVM2: fixed-point 3-D math for the binary-mesh display ============= *
 * No FPU use in the kernel VM thread: angles are milliradians, trig is Q15
 * (value/32768), so the embedded mesh can be projected/shaded with longs. */
#define FX_PI 3142                                 /* pi in millirad (Q0) */
static int isin_q15(int mrad)                      /* sin(mrad/1000) in Q15 */
{
    int m = mrad % (2*FX_PI);
    if (m < -FX_PI) m += 2*FX_PI; else if (m > FX_PI) m -= 2*FX_PI;
    long xn = (long)m * 32768 / FX_PI;             /* x/pi in Q15, [-1,1]   */
    long axn = xn < 0 ? -xn : xn;
    long y = 4*xn - ((4*xn*axn) >> 15);            /* parabola approx       */
    long ay = y < 0 ? -y : y;
    y = y + (225 * (((y*ay) >> 15) - y)) / 1000;   /* precision refinement  */
    return (int)y;
}
static int icos_q15(int mrad) { return isin_q15(mrad + FX_PI/2); }
static unsigned long a_isqrt(unsigned long v)
{
    unsigned long r = 0, b = 1UL << 62;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}

/* ===== AVM2 embedded mesh (binary vertex-buffer region) ================== */
#define MESH_MAXV 32768
#define MESH_MAXT 49152
static int   mesh_has, mesh_mode;                  /* loaded / a mesh frame is up */
static int   mesh_nv, mesh_nt, mesh_scale;
static short mesh_px[MESH_MAXV], mesh_py[MESH_MAXV], mesh_pz[MESH_MAXV];
static unsigned char mesh_cr[MESH_MAXV], mesh_cg[MESH_MAXV], mesh_cb[MESH_MAXV];
static short mesh_nx[MESH_MAXV], mesh_ny[MESH_MAXV], mesh_nz[MESH_MAXV];
static int   mesh_idx[MESH_MAXT*3];
static int   macc_x[MESH_MAXV], macc_y[MESH_MAXV], macc_z[MESH_MAXV];   /* load-time */
/* smooth (area-weighted) per-vertex normals in object space, Q15 */
static void avm_mesh_normals(void)
{
    for (int i = 0; i < mesh_nv; i++) { macc_x[i] = macc_y[i] = macc_z[i] = 0; }
    for (int t = 0; t < mesh_nt; t++) {
        int a = mesh_idx[t*3], b = mesh_idx[t*3+1], c = mesh_idx[t*3+2];
        long ux = mesh_px[b]-mesh_px[a], uy = mesh_py[b]-mesh_py[a], uz = mesh_pz[b]-mesh_pz[a];
        long vx = mesh_px[c]-mesh_px[a], vy = mesh_py[c]-mesh_py[a], vz = mesh_pz[c]-mesh_pz[a];
        long fx = (uy*vz - uz*vy) >> 10, fy = (uz*vx - ux*vz) >> 10, fz = (ux*vy - uy*vx) >> 10;
        macc_x[a]+=fx; macc_y[a]+=fy; macc_z[a]+=fz;
        macc_x[b]+=fx; macc_y[b]+=fy; macc_z[b]+=fz;
        macc_x[c]+=fx; macc_y[c]+=fy; macc_z[c]+=fz;
    }
    for (int i = 0; i < mesh_nv; i++) {
        long x = macc_x[i], y = macc_y[i], z = macc_z[i];
        unsigned long l = a_isqrt((unsigned long)(x*x + y*y + z*z)); if (!l) l = 1;
        mesh_nx[i] = (short)(x*32767/(long)l);
        mesh_ny[i] = (short)(y*32767/(long)l);
        mesh_nz[i] = (short)(z*32767/(long)l);
    }
}

/* ===== staged upload (filled by /actor/loadvm chunks) ===================== */
#define AVM_STAGE_MAX (12*1024*1024)   /* room for a solid turntable (many frames) */
static unsigned char avm_stage[AVM_STAGE_MAX];
static int           avm_stage_len;
/* On-screen upload progress (drawn by the WM via avm_draw_loadbar). */
static volatile int  avm_ld_active;    /* a binary upload is in flight */
static volatile int  avm_ld_cur;       /* bytes received so far */
static volatile int  avm_ld_total;     /* expected total (from ?total=) */
int                 avm_loadrun(int len);   /* forward decl */
void avm_stage_reset(void) { avm_stage_len = 0; avm_ld_cur = 0; avm_ld_active = 1; }
int  avm_stage_put(int off, const unsigned char *b, int n)
{
    if (off < 0 || n < 0 || off + n > AVM_STAGE_MAX) return -1;
    for (int i = 0; i < n; i++) avm_stage[off + i] = b[i];
    if (off + n > avm_stage_len) avm_stage_len = off + n;
    return 0;
}
/* Called per chunk from the HTTP handler so the bar tracks the upload. */
void avm_load_progress(int cur, int total)
{
    avm_ld_active = 1; avm_ld_cur = cur;
    if (total > 0) avm_ld_total = total;
}

/* ===== parsed module ===================================================== */
#define VM_STR_MAX     512
#define VM_STRBUF_MAX  16384
#define VM_MAX_CLASSES 16
#define VM_MAX_METHODS 320     /* a turntable packs ~14 chunk methods x many frames into one class */
typedef struct { unsigned short name; unsigned char n_params; int code_len, code_off; } vmmeth_t;
typedef struct { unsigned short name, n_fields, n_methods; vmmeth_t m[VM_MAX_METHODS]; } vmclass_t;
static const char *vm_str[VM_STR_MAX];
static char        vm_strbuf[VM_STRBUF_MAX];
static int         vm_n_str;
static vmclass_t   vm_class[VM_MAX_CLASSES];
static int         vm_n_class;
static unsigned char *vm_mod;          /* = avm_stage */
static int            vm_mod_len;

/* ===== objects (actor instances) + FIFO message queue ==================== */
#define VM_MAXOBJ 64
#define VM_MAXF   24
typedef struct { int used, cls; long f[VM_MAXF]; } vmobj_t;
static vmobj_t vm_obj[VM_MAXOBJ];
static int     vm_nobj;

#define VM_Q 512
typedef struct { int self, sender, na; const char *method; long a[8]; } vmmsg_t;
static vmmsg_t vm_q[VM_Q];
static int     vm_qh, vm_qt;
static void vm_enqueue(int self, int recv, const char *method, int na, long *a)
{
    int nx = (vm_qt + 1) % VM_Q;
    if (nx == vm_qh) return;             /* queue full: drop */
    vm_q[vm_qt].self = recv; vm_q[vm_qt].sender = self;
    vm_q[vm_qt].method = method; vm_q[vm_qt].na = na;
    for (int i = 0; i < na && i < 8; i++) vm_q[vm_qt].a[i] = a[i];
    vm_qt = nx;
}

/* ===== multi-core actor dispatch ========================================
 * Messages bound for DISTINCT, draw-free actors are dispatched across the 4
 * Cortex-A76 cores via the SMP fork-join pool (smp_parallel_sum).  Each core
 * runs independent method bodies that touch only their own actor's fields
 * (vm_obj[self].f[]) plus a private value stack — no shared mutable state to
 * lock, except the three things a method can PRODUCE:
 *   - sends  -> a per-core buffer (par_q[core]), merged into vm_q after join;
 *   - spawns -> a per-core disjoint slot range of the object table;
 *   - draws  -> EXCLUDED: any class containing WAIT or a draw opcode is marked
 *               par_safe=0 and always takes the original single-core path, so
 *               the visual demos (Rotate/MAKINA/philosophers) are unchanged.
 * The D-cache is off, so per-core buffers are coherent without locks/atomics
 * (every access hits RAM); the smp_parallel_sum dsb barriers order publication. */
#define PAR_NCORES    4
#define PAR_SENDQ     512
#define PAR_BATCH_MAX 32                                  /* <= VM_MAXOBJ */
extern int  smp_cores_online(void);
static unsigned char vm_class_parsafe[VM_MAX_CLASSES];   /* 1 = no WAIT/draw ops */
static int           avm_par_enable = 0;                 /* OFF by default; /avm-par?on=1 enables */
static volatile int  avm_par_nbatch = 0;                 /* diag: parallel batches dispatched */
static volatile int  avm_par_lastbn = 0;                 /* diag: size of the last batch */
void avm_set_par(int on) { avm_par_enable = on ? 1 : 0; }
int  avm_get_par(void)   { return avm_par_enable; }
int  avm_get_par_nbatch(void) { return avm_par_nbatch; }
int  avm_get_par_lastbn(void) { return avm_par_lastbn; }
static volatile int  par_active = 0;                     /* set while a batch runs */
static vmmsg_t   par_q[PAR_NCORES][PAR_SENDQ];           /* per-core produced sends */
static int       par_qn[PAR_NCORES];
static int       par_spawn_next[PAR_NCORES], par_spawn_hi[PAR_NCORES];
static vmmsg_t  *par_batch;                              /* the batch being run     */

/* True if a method body contains a parallel-unsafe opcode (WAIT or a draw).
 * Walks operands correctly so an operand byte is never read as an opcode. */
static int code_has_unsafe(const unsigned char *code, int len)
{
    int pc = 0;
    while (pc < len) {
        unsigned char op = code[pc++];
        switch (op) {
            case 0x07: case 0x45: case 0x46: case 0x47: case 0x48: return 1; /* WAIT/LINE/CLS/TRI/MESH3D */
            case 0x01: pc += 4; break;                       /* PUSHI         */
            case 0x02: case 0x03: case 0x04: pc += 1; break; /* field/arg     */
            case 0x30: case 0x31: case 0x41: pc += 2; break; /* JMP/JMPZ/SPAWN */
            case 0x40: case 0x44: pc += 3; break;            /* SEND/PRINTF   */
            default: break;                                  /* 0-operand     */
        }
    }
    return 0;
}

/* Per-core, lock-free during a batch (single writer = this core). */
static void par_send(int core, int self, int recv, const char *method, int na, long *a)
{
    int n = par_qn[core];
    if (n >= PAR_SENDQ) return;                            /* overflow: drop */
    par_q[core][n].self = recv; par_q[core][n].sender = self;
    par_q[core][n].method = method; par_q[core][n].na = na;
    for (int i = 0; i < na && i < 8; i++) par_q[core][n].a[i] = a[i];
    par_qn[core] = n + 1;
}

/* Allocate a fresh object from this core's disjoint slot range (no race). */
static int par_spawn(int core, int cls)
{
    if (cls < 0 || cls >= vm_n_class) return -1;
    for (int i = par_spawn_next[core]; i < par_spawn_hi[core]; i++) {
        if (!vm_obj[i].used) {
            vm_obj[i].used = 1; vm_obj[i].cls = cls;
            for (int f = 0; f < VM_MAXF; f++) vm_obj[i].f[f] = 0;
            par_spawn_next[core] = i + 1;
            return i;
        }
    }
    return -1;                                             /* range exhausted */
}

static int vm_spawn(int cls)
{
    if (cls < 0 || cls >= vm_n_class) return -1;
    for (int i = 0; i < VM_MAXOBJ; i++) if (!vm_obj[i].used) {
        vm_obj[i].used = 1; vm_obj[i].cls = cls;
        for (int f = 0; f < VM_MAXF; f++) vm_obj[i].f[f] = 0;
        if (i >= vm_nobj) vm_nobj = i + 1;
        return i;
    }
    return -1;
}

/* ===== draw buffers + off-screen raster (the "Blender" display) ========== */
#define BW 760
#define BH 620
static unsigned int g_buf[BW * BH];      /* off-screen ARGB raster          */
static int          g_zbuf[BW * BH];     /* per-pixel depth for AVM2 mesh    */
static int          g_buf_ready;
#define VLINE_MAX 512
#define VTRI_MAX  24000
/* col: a 16-colour palette index (0..15) OR, when bit 24 is set, a full 24-bit
 * 0xRRGGBB true colour — so actors can drive the display in full colour. */
typedef struct { short x1,y1,x2,y2; unsigned int col; } avm_line_t;
typedef struct { short x1,y1,x2,y2,x3,y3; unsigned int col; } avm_tri_t;
static avm_line_t v_line[VLINE_MAX];
static int v_line_n;
static avm_tri_t  v_tri[VTRI_MAX];
static int v_tri_n;
#define G_BG 0xFF2C3238U                          /* content background (soft dark slate) */

/* ---- SMP: rasterise across the 4 Cortex-A72 cores -------------------------
 * The frame is split into horizontal scanline bands; each core fills ONE band
 * (clears it + draws every triangle/line clipped to its rows).  Bands are
 * disjoint pixel rows, so painter order is preserved and there are no races. */
extern int  smp_cores_online(void);
typedef long (*avm_range_fn)(long lo, long hi, int core);
extern long smp_parallel_sum(avm_range_fn fn, long n, int ncores);

/* ---- turntable frame cache + playback control (Start/Stop/Pause/arrows) ----
 * As the actor plays its first full turn we cache each frame's tri/line lists;
 * once a cycle completes the WM-side controls drive which frame shows. */
#define FC_MAX 24
static avm_tri_t  fc_tri[FC_MAX][VTRI_MAX];
static int fc_ntri[FC_MAX];
static avm_line_t fc_line[FC_MAX][VLINE_MAX];
static int fc_nline[FC_MAX];
static int  ctl_cached;        /* a full turn is cached -> control mode */
static int  ctl_nframes;       /* frames in the turntable */
static int  ctl_prevf = -1;    /* last frame index seen (cycle detect) */
static int  ctl_play  = 1;     /* auto-advance on/off */
static int  ctl_idx;           /* frame shown in control mode */
static int  ctl_dir   = 1;     /* +1 / -1 rotation direction */
static int  ctl_speed = 240;   /* ms per frame (from WAIT) */
static unsigned long ctl_last; /* pacing timestamp */

static unsigned int pal16(int c)
{
    static const unsigned int p[16] = {
        0xFF000000U,0xFF3060FFU,0xFF30D040U,0xFF30D0D0U,
        0xFFE03030U,0xFFE040E0U,0xFFE0E040U,0xFFF0F0F0U,
        0xFF808080U,0xFF80A0FFU,0xFF80FF80U,0xFF80FFFFU,
        0xFFFF8080U,0xFFFF80FFU,0xFFFFFF80U,0xFFFFFFFFU };
    return p[c & 15];
}
/* col -> ARGB: bit 24 set => true 24-bit colour, else a 16-colour palette index. */
static unsigned int avm_color(unsigned int c)
{
    return (c & 0x1000000u) ? (0xFF000000u | (c & 0xFFFFFFu)) : pal16((int)c);
}

/* scanline-fill a triangle into g_buf, clipped to scanline band [ylo,yhi). */
static void buf_tri_band(int x0,int y0,int x1,int y1,int x2,int y2,unsigned int col,int ylo,int yhi)
{
    int s;
    if (y1<y0){s=x0;x0=x1;x1=s;s=y0;y0=y1;y1=s;}
    if (y2<y0){s=x0;x0=x2;x2=s;s=y0;y0=y2;y2=s;}
    if (y2<y1){s=x1;x1=x2;x2=s;s=y1;y1=y2;y2=s;}
    if (y2==y0) return;
    int ya = y0 < ylo ? ylo : y0;
    int yb = y2 >= yhi ? yhi - 1 : y2;
    for (int yy=ya; yy<=yb; yy++) {
        if (yy<0 || yy>=BH) continue;
        int xa = x0 + (int)((long)(x2-x0)*(yy-y0)/(y2-y0)), xb;
        if (yy<y1 && y1!=y0) xb = x0 + (int)((long)(x1-x0)*(yy-y0)/(y1-y0));
        else if (y2!=y1)     xb = x1 + (int)((long)(x2-x1)*(yy-y1)/(y2-y1));
        else                 xb = x1;
        if (xa>xb){s=xa;xa=xb;xb=s;}
        if (xa<0) xa=0; if (xb>=BW) xb=BW-1;
        unsigned int *row = g_buf + yy*BW;
        for (int xx=xa; xx<=xb; xx++) row[xx]=col;
    }
}
static void buf_line_band(int x0,int y0,int x1,int y1,unsigned int col,int ylo,int yhi)
{
    int dx=a_abs(x1-x0), sx=x0<x1?1:-1, dy=-a_abs(y1-y0), sy=y0<y1?1:-1, err=dx+dy, e2;
    for (;;) {
        if (x0>=0&&x0<BW&&y0>=ylo&&y0<yhi&&y0<BH) g_buf[y0*BW+x0]=col;
        if (x0==x1&&y0==y1) break;
        e2=2*err; if (e2>=dy){err+=dy;x0+=sx;} if (e2<=dx){err+=dx;y0+=sy;}
    }
}
/* One SMP worker: clear + draw the rows [lo,hi) (called on each core). */
static long avm_render_band(long lo, long hi, int core)
{
    (void)core;
    for (int yy=(int)lo; yy<(int)hi; yy++) {
        unsigned int *row = g_buf + yy*BW;
        for (int x=0;x<BW;x++) row[x]=G_BG;
    }
    for (int i=0;i<v_tri_n;i++)
        buf_tri_band(v_tri[i].x1,v_tri[i].y1,v_tri[i].x2,v_tri[i].y2,v_tri[i].x3,v_tri[i].y3,
                     avm_color(v_tri[i].col),(int)lo,(int)hi);
    for (int i=0;i<v_line_n;i++)
        buf_line_band(v_line[i].x1,v_line[i].y1,v_line[i].x2,v_line[i].y2,
                      avm_color(v_line[i].col),(int)lo,(int)hi);
    return 0;
}
/* rasterise the accumulated frame into g_buf, fanned out over all cores. */
static void avm_render(void)
{
    int nc = smp_cores_online();
    if (nc < 1) nc = 1;
    smp_parallel_sum(avm_render_band, BH, nc);
    g_buf_ready = 1;
}

/* ===== AVM2 mesh: project + shade + z-buffer rasterise (the 3-D display) ==
 * Draws the embedded binary-vertex-buffer mesh straight into g_buf with a real
 * per-pixel z-buffer (so occlusion is correct — closed eyes stay closed), at the
 * runtime (yaw,pitch).  All integer/Q15 math; colours are R/B pre-swapped to
 * match this Pi4 framebuffer.  This is the kernel twin of the host render_glb. */
/* Render target (so the mesh can rasterise either straight into g_buf or into a
 * 2x supersample buffer for the AA checkbox).  Set by vm_mesh3d before drawing. */
#define SSW (BW*2)
#define SSH (BH*2)
static unsigned int ss_buf[SSW*SSH];     /* 2x supersample colour  (AA on) */
static int          ss_zbuf[SSW*SSH];    /* 2x supersample depth           */
static unsigned int *rbuf; static int *rz, rw, rh;

/* ---- SMP fan-out: project once, then rasterise the frame across all 4 cores --
 * Each core owns a disjoint horizontal scanline band [lo,hi), so it clears + draws
 * into its own rows of rbuf/rz with no locks (D-cache is off -> writes are seen).*/
typedef struct { short x0,y0,x1,y1,x2,y2; int z0,z1,z2; unsigned int col; } mtri_t;
static mtri_t mproj[MESH_MAXT];               /* projected+shaded triangles */
static int    mproj_n;
typedef struct { short x0,y0,x1,y1; } gseg_t;
static gseg_t mgseg[512];                     /* projected floor-grid segments */
static int    mgseg_n;
static int    mesh_wire_flag;                 /* snapshot for the band workers */

/* one projected triangle, z-buffered, clipped to scanline band [ylo,yhi) */
static void mtri_band(const mtri_t *t, int ylo, int yhi)
{
    int x0=t->x0,y0=t->y0, x1=t->x1,y1=t->y1, x2=t->x2,y2=t->y2;
    long z0=t->z0,z1=t->z1,z2=t->z2; unsigned int col=t->col; int s; long sz;
    if (y1<y0){s=x0;x0=x1;x1=s;s=y0;y0=y1;y1=s;sz=z0;z0=z1;z1=sz;}
    if (y2<y0){s=x0;x0=x2;x2=s;s=y0;y0=y2;y2=s;sz=z0;z0=z2;z2=sz;}
    if (y2<y1){s=x1;x1=x2;x2=s;s=y1;y1=y2;y2=s;sz=z1;z1=z2;z2=sz;}
    if (y2==y0) return;
    int ya = y0<ylo?ylo:y0, yb = y2>=yhi?yhi-1:y2;
    for (int yy=ya; yy<=yb; yy++) {
        if (yy<0 || yy>=rh) continue;
        int xa = x0 + (int)((long)(x2-x0)*(yy-y0)/(y2-y0));
        long za = z0 + (long)(z2-z0)*(yy-y0)/(y2-y0);
        int xb; long zb;
        if (yy<y1 && y1!=y0) { xb = x0 + (int)((long)(x1-x0)*(yy-y0)/(y1-y0));
                               zb = z0 + (long)(z1-z0)*(yy-y0)/(y1-y0); }
        else if (y2!=y1)     { xb = x1 + (int)((long)(x2-x1)*(yy-y1)/(y2-y1));
                               zb = z1 + (long)(z2-z1)*(yy-y1)/(y2-y1); }
        else                 { xb = x1; zb = z1; }
        if (xa>xb){ s=xa;xa=xb;xb=s; sz=za;za=zb;zb=sz; }
        int span = xb-xa; if (span<0) continue;
        unsigned int *row = rbuf + yy*rw; int *zr = rz + yy*rw;
        int lx = xa<0?0:xa, hx = xb>=rw?rw-1:xb;
        for (int xx=lx; xx<=hx; xx++) {
            long z = span ? (za + (zb-za)*(xx-xa)/span) : za;
            if (z > zr[xx]) { zr[xx] = (int)z; row[xx] = col; }
        }
    }
}
/* a line clipped to band [ylo,yhi) (no z; for wireframe + floor grid) */
static void mline_band(int x0,int y0,int x1,int y1,unsigned int col,int ylo,int yhi)
{
    int dx=a_abs(x1-x0), sx=x0<x1?1:-1, dy=-a_abs(y1-y0), sy=y0<y1?1:-1, err=dx+dy, e2;
    for (;;) {
        if (x0>=0&&x0<rw&&y0>=ylo&&y0<yhi&&y0<rh) rbuf[y0*rw+x0]=col;
        if (x0==x1&&y0==y1) break;
        e2=2*err; if (e2>=dy){err+=dy;x0+=sx;} if (e2<=dx){err+=dx;y0+=sy;}
    }
}
/* SMP worker: clear band, draw grid, then every triangle clipped to the band. */
static long mesh_render_band(long lo, long hi, int core)
{
    (void)core;
    for (int yy=(int)lo; yy<(int)hi; yy++) {
        unsigned int *row = rbuf + yy*rw; int *zr = rz + yy*rw;
        for (int x=0;x<rw;x++){ row[x]=G_BG; zr[x]=-0x40000000; }
    }
    for (int i=0;i<mgseg_n;i++)
        mline_band(mgseg[i].x0,mgseg[i].y0,mgseg[i].x1,mgseg[i].y1,0xFF384848u,(int)lo,(int)hi);
    if (mesh_wire_flag) {
        for (int i=0;i<mproj_n;i++) { const mtri_t *t=&mproj[i];
            mline_band(t->x0,t->y0,t->x1,t->y1,t->col,(int)lo,(int)hi);
            mline_band(t->x1,t->y1,t->x2,t->y2,t->col,(int)lo,(int)hi);
            mline_band(t->x2,t->y2,t->x0,t->y0,t->col,(int)lo,(int)hi); }
    } else {
        for (int i=0;i<mproj_n;i++) mtri_band(&mproj[i],(int)lo,(int)hi);
    }
    return 0;
}
/* SMP worker: box-downsample the 2x supersample buffer into g_buf for band rows. */
static long mesh_down_band(long lo, long hi, int core)
{
    (void)core;
    for (int y=(int)lo; y<(int)hi; y++) {
        unsigned int *o = g_buf + y*BW;
        unsigned int *a = ss_buf + (2*y)*SSW, *b = ss_buf + (2*y+1)*SSW;
        for (int x=0;x<BW;x++) {
            unsigned int p0=a[2*x],p1=a[2*x+1],p2=b[2*x],p3=b[2*x+1];
            int rr=((p0>>16&255)+(p1>>16&255)+(p2>>16&255)+(p3>>16&255))>>2;
            int gg=((p0>>8&255)+(p1>>8&255)+(p2>>8&255)+(p3>>8&255))>>2;
            int bb=((p0&255)+(p1&255)+(p2&255)+(p3&255))>>2;
            o[x]=0xFF000000u|(rr<<16)|(gg<<8)|bb;
        }
    }
    return 0;
}
/* light direction (0.40,0.60,0.70) normalised, Q15 */
#define MESH_LX 13044
#define MESH_LY 19565
#define MESH_LZ 22825
/* ---- AVM2 mesh turntable + feature toggles (driven by the toolbar/checkboxes) */
static int mesh_yaw_mrad, mesh_pitch_mrad, mesh_yaw0, mesh_pitch0;
static int mesh_spin, mesh_wire, mesh_flat;        /* shading / motion          */
static int mesh_aa, mesh_grid, mesh_persp;         /* AA / floor grid / perspective */
#define MESH_SPIN_RATE 1500                        /* mrad/sec (~86 deg/s, ~4.2 s/rev) */
static unsigned long mesh_last;
static void vm_mesh3d(int yaw_mrad, int pitch_mrad)
{
    if (!mesh_has) return;
    int cyq = icos_q15(yaw_mrad), syq = isin_q15(yaw_mrad);
    int cpq = icos_q15(pitch_mrad), spq = isin_q15(pitch_mrad);
    int SC = mesh_scale ? mesh_scale : 32000;
    /* pick render target: 2x supersample buffer when AA is on, else g_buf */
    int ssf = mesh_aa ? 2 : 1;
    if (ssf == 2) { rbuf = ss_buf; rz = ss_zbuf; rw = SSW; rh = SSH; }
    else          { rbuf = g_buf;  rz = g_zbuf;  rw = BW;  rh = BH;  }
    int CXp = 380*ssf, CYp = 300*ssf, FS = 260*ssf;
    /* ---- phase 1 (single core): project + shade every triangle into mproj ---- */
    mesh_wire_flag = mesh_wire;
    mgseg_n = 0;
    if (mesh_grid) {                                /* project the floor grid once */
        int g1 = (3*SC)/2, step = SC/4;
        for (int a = -g1; a <= g1 && mgseg_n < 510; a += step) {
            for (int dir = 0; dir < 2; dir++) {
                int ex[2], ey[2];
                for (int e = 0; e < 2; e++) {
                    long wx = (dir==0) ? a : (e? g1 : -g1);
                    long wz = (dir==0) ? (e? g1 : -g1) : a, wy = -SC;
                    long x2 = (wx*cyq + wz*syq) >> 15, z2 = (-wx*syq + wz*cyq) >> 15;
                    long y3 = (wy*cpq - z2*spq) >> 15, z3 = (wy*spq + z2*cpq) >> 15;
                    if (mesh_persp) { long zc = z3 + 3*SC; if (zc < 1) zc = 1;
                        ex[e] = CXp + (int)((FS*3*x2)/zc); ey[e] = CYp - (int)((FS*3*y3)/zc); }
                    else { ex[e] = CXp + (int)((FS*x2)/SC); ey[e] = CYp - (int)((FS*y3)/SC); }
                }
                mgseg[mgseg_n].x0=ex[0]; mgseg[mgseg_n].y0=ey[0];
                mgseg[mgseg_n].x1=ex[1]; mgseg[mgseg_n].y1=ey[1]; mgseg_n++;
            }
        }
    }
    mproj_n = 0;
    for (int t = 0; t < mesh_nt; t++) {
        int sx[3], sy[3]; long sz[3], cx3[3], cy3[3], cz3[3];
        long fsum = 0, rsum = 0, gsum = 0, bsum = 0;
        for (int k = 0; k < 3; k++) {
            int i = mesh_idx[t*3+k];
            long vx = mesh_px[i], vy = mesh_py[i], vz = mesh_pz[i];
            long x2 = (vx*cyq + vz*syq) >> 15;
            long z2 = (-vx*syq + vz*cyq) >> 15;
            long y3 = (vy*cpq - z2*spq) >> 15;
            long z3 = (vy*spq + z2*cpq) >> 15;
            if (mesh_persp) {
                long zc = z3 + 3*SC; if (zc < 1) zc = 1;
                sx[k] = CXp + (int)((FS*3*x2)/zc);
                sy[k] = CYp - (int)((FS*3*y3)/zc);
            } else {
                sx[k] = CXp + (int)((FS*x2)/SC);
                sy[k] = CYp - (int)((FS*y3)/SC);
            }
            sz[k] = z3; cx3[k] = x2; cy3[k] = y3; cz3[k] = z3;
            if (!mesh_flat) {
                long nx = mesh_nx[i], ny = mesh_ny[i], nz = mesh_nz[i];
                long rx = (nx*cyq + nz*syq) >> 15;
                long rzz = (-nx*syq + nz*cyq) >> 15;
                long ry = (ny*cpq - rzz*spq) >> 15;
                long rz2 = (ny*spq + rzz*cpq) >> 15;
                long ndl = (rx*MESH_LX + ry*MESH_LY + rz2*MESH_LZ) >> 15;
                if (ndl < 0) ndl = 0;
                fsum += 14746 + ((18022*ndl) >> 15);
            }
            rsum += mesh_cr[i]; gsum += mesh_cg[i]; bsum += mesh_cb[i];
        }
        long f;
        if (mesh_flat) {
            long ux = cx3[1]-cx3[0], uy = cy3[1]-cy3[0], uz = cz3[1]-cz3[0];
            long vx = cx3[2]-cx3[0], vy = cy3[2]-cy3[0], vz = cz3[2]-cz3[0];
            long fx = uy*vz-uz*vy, fy = uz*vx-ux*vz, fz = ux*vy-uy*vx;
            unsigned long l = a_isqrt((unsigned long)(fx*fx+fy*fy+fz*fz)); if (!l) l = 1;
            long ndl = ((fx*32767/(long)l)*MESH_LX + (fy*32767/(long)l)*MESH_LY
                        + (fz*32767/(long)l)*MESH_LZ) >> 15;
            if (ndl < 0) ndl = -ndl;
            f = 14746 + ((18022*ndl) >> 15);
        } else {
            f = fsum/3;
        }
        int r = (int)(((rsum/3)*f) >> 15); if (r > 255) r = 255;
        int g = (int)(((gsum/3)*f) >> 15); if (g > 255) g = 255;
        int b = (int)(((bsum/3)*f) >> 15); if (b > 255) b = 255;
        mtri_t *m = &mproj[mproj_n++];
        m->x0=sx[0]; m->y0=sy[0]; m->x1=sx[1]; m->y1=sy[1]; m->x2=sx[2]; m->y2=sy[2];
        m->z0=sz[0]; m->z1=sz[1]; m->z2=sz[2];
        /* rpi5 framebuffer is straight RGB (matches fill_rect / tri()); pack
         * standard 0xRRGGBB (NOT the Pi4 R/B-swapped order). */
        m->col = 0xFF000000u | ((unsigned)r<<16) | ((unsigned)g<<8) | (unsigned)b;
    }
    /* ---- phase 2 (all cores): clear+grid+rasterise, one scanline band/core --- */
    int nc = smp_cores_online(); if (nc < 1) nc = 1;
    smp_parallel_sum(mesh_render_band, rh, nc);
    /* ---- phase 3 (all cores): AA box-downsample 2x -> g_buf -------------------*/
    if (ssf == 2) smp_parallel_sum(mesh_down_band, BH, nc);
    g_buf_ready = 1; mesh_mode = 1;
}

/* Load a cached turntable frame into the live buffers and rasterise it. */
static void ctl_show(int idx)
{
    if (idx < 0 || idx >= ctl_nframes) return;
    v_tri_n = fc_ntri[idx];
    for (int i=0;i<v_tri_n;i++) v_tri[i] = fc_tri[idx][i];
    v_line_n = fc_nline[idx];
    for (int i=0;i<v_line_n;i++) v_line[i] = fc_line[idx][i];
    avm_render();
}

/* opcode-facing draw API (called from dispatch) */
static void vm_cls(void)  { v_line_n = 0; v_tri_n = 0; }
static void vm_line(int x1,int y1,int x2,int y2,int col)
{ if (v_line_n<VLINE_MAX){ v_line[v_line_n].x1=x1;v_line[v_line_n].y1=y1;v_line[v_line_n].x2=x2;v_line[v_line_n].y2=y2;v_line[v_line_n].col=(unsigned int)col;v_line_n++; } }
static void vm_tri(int x1,int y1,int x2,int y2,int x3,int y3,int col)
{ if (v_tri_n<VTRI_MAX){ v_tri[v_tri_n].x1=x1;v_tri[v_tri_n].y1=y1;v_tri[v_tri_n].x2=x2;v_tri[v_tri_n].y2=y2;v_tri[v_tri_n].x3=x3;v_tri[v_tri_n].y3=y3;v_tri[v_tri_n].col=(unsigned int)col;v_tri_n++; } }

/* ===== VM graphics window (toolbar + checkbox row + Blender display) ===== */
#define AVM_TB 22                                   /* button row height   */
#define AVM_CB 20                                   /* checkbox row height */
#define AVM_NCB 6
static const char *avm_btn[5] = { "Play", "Pause", "Stop", " <", " >" };
static const char *avm_cbl[AVM_NCB] = { "Wire", "Flat", "Spin", "AA", "Grid", "Persp" };
static int *avm_cbv(int i)                           /* checkbox -> state cell */
{
    switch (i) {
        case 0: return &mesh_wire;  case 1: return &mesh_flat;  case 2: return &mesh_spin;
        case 3: return &mesh_aa;    case 4: return &mesh_grid;  default: return &mesh_persp;
    }
}
static window_t vmgfx_win;
static int      vmgfx_added;
static void vmgfx_draw(window_t *self, unsigned int frame)
{
    (void)frame;
    avm_tick();                             /* advance / control the VM */
    /* row 1: Play / Pause / Stop / prev / next */
    int tx = self->x + 2, ty = self->y + WM_TITLEBAR_H + 2, bw = BW / 5;
    int playing = mesh_mode ? mesh_spin : ctl_play;
    for (int i = 0; i < 5; i++) {
        int bx = tx + i * bw;
        unsigned int bg = 0xFF1F3550U;
        if ((i == 0 && playing) || (i == 1 && !playing)) bg = 0xFF1F6E2EU;    /* active */
        fill_rect(bx, ty, bw - 2, AVM_TB, bg);
        draw_rect(bx, ty, bw - 2, AVM_TB, 0xFF6080A0U);
        draw_string_at(bx + 10, ty + (AVM_TB - 8) / 2, avm_btn[i], 0xFFE8F0F8U, bg);
    }
    /* row 2: feature checkboxes (Wire / Flat / Spin) */
    int cy = ty + AVM_TB + 1, cw = BW / AVM_NCB;
    for (int i = 0; i < AVM_NCB; i++) {
        int cx = tx + i * cw, on = *avm_cbv(i);
        unsigned int bg = 0xFF152535U;
        fill_rect(cx, cy, cw - 2, AVM_CB, bg);
        draw_rect(cx, cy, cw - 2, AVM_CB, 0xFF40607FU);
        int bxx = cx + 6, byy = cy + (AVM_CB - 12) / 2;     /* 12x12 tick box */
        fill_rect(bxx, byy, 12, 12, on ? 0xFF30D040U : 0xFF0A1018U);
        draw_rect(bxx, byy, 12, 12, 0xFF80A0C0U);
        draw_string_at(bxx + 18, cy + (AVM_CB - 8) / 2, avm_cbl[i],
                       0xFFE8F0F8U, bg);
    }
    if (g_buf_ready)
        video_blit(self->x + 2, self->y + WM_TITLEBAR_H + 2 + AVM_TB + 1 + AVM_CB + 1,
                   BW, BH, g_buf, BW);
}
/* Toolbar / checkbox click -> control + feature toggle.  (lx,ly) window-local. */
static void vmgfx_click(window_t *self, int lx, int ly)
{
    (void)self;
    int r1top = WM_TITLEBAR_H + 2, r1bot = r1top + AVM_TB;
    int r2top = r1bot + 1,         r2bot = r2top + AVM_CB;
    int bx = lx - 2; if (bx < 0) return;
    if (ly >= r1top && ly < r1bot) {                 /* button row */
        int bw = BW / 5; if (bw < 1) bw = 1;
        int i = bx / bw;
        if (i >= 0 && i <= 4) avm_ctl(i);
    } else if (ly >= r2top && ly < r2bot) {          /* checkbox row */
        int cw = BW / AVM_NCB; if (cw < 1) cw = 1;
        int i = bx / cw;
        if (i >= 0 && i < AVM_NCB) {
            int *v = avm_cbv(i); *v = !*v;
            if (i == 2) { if (mesh_spin) mesh_last = 0; }   /* Spin -> turntable */
            else if (mesh_mode) vm_mesh3d(mesh_yaw_mrad, mesh_pitch_mrad); /* Wire/Flat */
        }
    }
}
/* Arrow / space keys when the VM window is active: rotate / play / pause.
 * The caller (main.c) only invokes this when wm_kbd_target() is this window. */
window_t *avm_window(void) { return &vmgfx_win; }
void avm_key(char c)
{
    /* ESC [ A/B/C/D arrows arrive as a 3-byte sequence; main.c forwards the
     * final letter.  Also accept WASD + space as direct controls. */
    switch (c) {
        case 'D': case 'a': avm_ctl(3); break;      /* left  = prev */
        case 'C': case 'd': avm_ctl(4); break;      /* right = next */
        case 'A': case 'w': avm_ctl(0); break;      /* up    = play */
        case 'B': case 's': avm_ctl(1); break;      /* down  = pause */
        case ' ': avm_ctl((mesh_mode ? mesh_spin : ctl_play) ? 1 : 0); break; /* space toggle */
        default: break;
    }
}

/* ===== on-screen upload load bar (drawn by the WM each frame) ============= *
 * Shows .avm upload progress while a POST /actor/loadvm is streaming in, so the
 * rpi5 screen reports how far the transfer has got.  Screen-space; (sw,sh) are
 * the framebuffer dimensions.  No SD/launch streaming (rpi5 uploads via RAM). */
void avm_draw_loadbar(int sw, int sh)
{
    if (!avm_ld_active) return;
    int bw = 460, bh = 30, bx = (sw - bw) / 2, by = sh - 100;
    int pct = (avm_ld_total > 0) ? (int)((long)avm_ld_cur * 100 / avm_ld_total) : 0;
    if (pct > 100) pct = 100;
    fill_rect(bx - 6, by - 22, bw + 12, bh + 46, 0xFF0A1420U);     /* panel       */
    draw_rect(bx - 6, by - 22, bw + 12, bh + 46, 0xFF40A0FFU);     /* blue border */
    draw_string_at(bx, by - 16, "Loading .avm actor (uploading)...", 0xFFE8F0F8U, 0xFF0A1420U);
    draw_rect(bx, by, bw, bh, 0xFF6080A0U);                        /* track       */
    fill_rect(bx + 1, by + 1, (bw - 2) * pct / 100, bh - 2, 0xFF30D040U); /* fill */
    /* percent (centred on the bar) */
    char p[6]; int n = 0;
    if (pct >= 100) { p[n++]='1';p[n++]='0';p[n++]='0'; }
    else if (pct >= 10) { p[n++]=(char)('0'+pct/10); p[n++]=(char)('0'+pct%10); }
    else p[n++]=(char)('0'+pct);
    p[n++]='%'; p[n]=0;
    draw_string_at(bx + bw/2 - 12, by + (bh-8)/2, p, 0xFF000000U, 0xFF30D040U);
    /* KB counter "cur / total KB" under the bar */
    char m[40]; int q = 0;
    unsigned int cur_kb = (unsigned int)avm_ld_cur / 1024;
    unsigned int tot_kb = (unsigned int)(avm_ld_total > 0 ? avm_ld_total : 0) / 1024;
    char tmp[12]; int tn;
    unsigned int v = cur_kb; tn = 0; if (!v) tmp[tn++]='0'; while (v){ tmp[tn++]=(char)('0'+v%10); v/=10; }
    while (tn) m[q++] = tmp[--tn];
    m[q++]=' '; m[q++]='/'; m[q++]=' ';
    v = tot_kb; tn = 0; if (!v) tmp[tn++]='0'; while (v){ tmp[tn++]=(char)('0'+v%10); v/=10; }
    while (tn) m[q++] = tmp[--tn];
    m[q++]=' '; m[q++]='K'; m[q++]='B'; m[q]=0;
    draw_string_at(bx, by + bh + 4, m, 0xFFBfD8F0U, 0xFF0A1420U);
}

/* ===== module loader (AVM1) ============================================== */
static int avm_load(unsigned char *buf, int len)
{
    if (len < 6) return -1;
    if (buf[0]!='A'||buf[1]!='V'||buf[2]!='M'||(buf[3]!='1'&&buf[3]!='2')) return -1;
    vm_mod = buf; vm_mod_len = len; mesh_has = 0; mesh_mode = 0;
    const unsigned char *p = buf + 4, *end = buf + len;
    int ns = vm_u16(p); p += 2; vm_n_str = 0; int sb = 0, i;
    for (i = 0; i < ns && i < VM_STR_MAX; i++) {
        if (p + 2 > end) return -1;
        int l = vm_u16(p); p += 2;
        if (p + l > end || sb + l + 1 > VM_STRBUF_MAX) return -1;
        vm_str[i] = &vm_strbuf[sb];
        for (int k=0;k<l;k++) vm_strbuf[sb+k]=p[k];
        vm_strbuf[sb+l]=0; sb += l+1; p += l;
    }
    vm_n_str = i;
    if (p + 2 > end) return -1;
    int nc = vm_u16(p); p += 2; vm_n_class = 0;
    int c, mi;
    for (c = 0; c < nc && c < VM_MAX_CLASSES; c++) {
        if (p + 6 > end) return -1;
        vmclass_t *cl = &vm_class[c];
        cl->name = vm_u16(p); p+=2; cl->n_fields = vm_u16(p); p+=2; cl->n_methods = vm_u16(p); p+=2;
        for (mi = 0; mi < cl->n_methods && mi < VM_MAX_METHODS; mi++) {
            if (p + 5 > end) return -1;
            cl->m[mi].name = vm_u16(p); p+=2; cl->m[mi].n_params = *p++;
            cl->m[mi].code_len = vm_u16(p); p+=2; cl->m[mi].code_off = (int)(p - buf);
            if (p + cl->m[mi].code_len > end) return -1;
            p += cl->m[mi].code_len;
        }
        vm_n_class++;
    }
    /* AVM2: optional binary MESH region after the class table. */
    if (buf[3] == '2' && p < end && *p == 1) {
        p++;
        if (p + 13 > end) return -1;
        int nv = (int)vm_u32(p), nt = (int)vm_u32(p+4), idxw = p[8], scale = vm_i32(p+9);
        p += 13;
        if (nv < 0 || nt < 0 || nv > MESH_MAXV || nt > MESH_MAXT) return -1;
        if (p + (long)nv*9 + (long)nt*3*idxw > end) return -1;
        mesh_nv = nv; mesh_nt = nt; mesh_scale = scale;
        for (int i = 0; i < nv; i++) {
            mesh_px[i] = (short)vm_i16(p);   mesh_py[i] = (short)vm_i16(p+2);
            mesh_pz[i] = (short)vm_i16(p+4); mesh_cr[i] = p[6]; mesh_cg[i] = p[7];
            mesh_cb[i] = p[8]; p += 9;
        }
        for (int t = 0; t < nt*3; t++) {
            mesh_idx[t] = (idxw == 2) ? (int)vm_u16(p) : (int)vm_u32(p); p += idxw;
        }
        avm_mesh_normals();
        mesh_has = 1;
        mesh_spin = 1;          /* AVM2 mesh -> auto-rotate (turntable) on load */
    }
    /* Mark which classes are safe for parallel dispatch: a class with NO method
     * containing WAIT or a draw opcode can run on a worker core (it only touches
     * its own fields + produces sends/spawns, which we buffer per-core). */
    for (c = 0; c < vm_n_class; c++) {
        vmclass_t *cl = &vm_class[c];
        int safe = 1;
        for (mi = 0; mi < cl->n_methods && mi < VM_MAX_METHODS; mi++)
            if (code_has_unsafe(buf + cl->m[mi].code_off, cl->m[mi].code_len)) { safe = 0; break; }
        vm_class_parsafe[c] = (unsigned char)safe;
    }
    return vm_n_class > 0 ? 0 : -1;
}

/* ===== bytecode dispatch (one message) ================================== */
#define VM_VSTACK 128
static void avm_dispatch(int self, int sender, const char *method, long *args, int n_args, int core)
{
    if (self < 0 || self >= VM_MAXOBJ || !vm_obj[self].used) return;
    vmclass_t *cl = &vm_class[vm_obj[self].cls];
    vmmeth_t *mt = 0;
    for (int i = 0; i < cl->n_methods; i++) {
        unsigned short nm = cl->m[i].name;
        if (nm < vm_n_str && a_streq(vm_str[nm], method)) { mt = &cl->m[i]; break; }
    }
    if (!mt) return;
    const unsigned char *code = vm_mod + mt->code_off; int clen = mt->code_len;
    long stk[VM_VSTACK]; int sp = 0, pc = 0; long guard = 0;
#define VPUSH(v) do { if (sp < VM_VSTACK) stk[sp++] = (long)(v); } while (0)
#define VPOP()   (sp > 0 ? stk[--sp] : 0)
    while (pc < clen) {
        if (++guard > 5000000L) break;
        unsigned char op = code[pc++];
        switch (op) {
        case 0x01: VPUSH(vm_i32(code+pc)); pc+=4; break;                       /* PUSHI */
        case 0x02: { int f=code[pc++]; VPUSH(f<VM_MAXF?vm_obj[self].f[f]:0); } break;  /* GETFIELD */
        case 0x03: { int f=code[pc++]; long v=VPOP(); if (f<VM_MAXF) vm_obj[self].f[f]=v; } break; /* SETFIELD */
        case 0x04: { int a=code[pc++]; VPUSH(a<n_args?args[a]:0); } break;     /* GETARG */
        case 0x05: VPUSH(self); break;                                          /* SELF */
        case 0x06: VPUSH(sender); break;                                        /* SENDER */
        case 0x07: {                                                            /* WAIT = frame boundary */
            vm_wait_ms = (int)VPOP();
            if (vm_wait_ms > 0) ctl_speed = vm_wait_ms;
            if (mesh_mode) { vm_frame_done = 1; break; }   /* mesh already in g_buf */
            if (!ctl_cached) {
                int cf = (self >= 0 && self < VM_MAXOBJ) ? (int)vm_obj[self].f[0] : 0;
                if (cf >= 0 && cf < FC_MAX) {                /* cache this frame */
                    fc_ntri[cf] = v_tri_n;
                    for (int i=0;i<v_tri_n;i++) fc_tri[cf][i] = v_tri[i];
                    fc_nline[cf] = v_line_n;
                    for (int i=0;i<v_line_n;i++) fc_line[cf][i] = v_line[i];
                    if (cf + 1 > ctl_nframes) ctl_nframes = cf + 1;
                }
                if (cf < ctl_prevf) ctl_cached = 1;          /* frame index wrapped -> full turn cached */
                ctl_prevf = cf;
                avm_render();                                /* show the live frame while caching */
            }
            vm_frame_done = 1;
        } break;
        case 0x08: { long v = sp>0?stk[sp-1]:0; VPUSH(v); } break;             /* DUP */
        case 0x10: { long b=VPOP(),a=VPOP(); VPUSH(a+b); } break;
        case 0x11: { long b=VPOP(),a=VPOP(); VPUSH(a-b); } break;
        case 0x12: { long b=VPOP(),a=VPOP(); VPUSH(a*b); } break;
        case 0x13: { long b=VPOP(),a=VPOP(); VPUSH(b?a/b:0); } break;
        case 0x14: { long b=VPOP(),a=VPOP(); VPUSH(b?a%b:0); } break;
        case 0x20: { long b=VPOP(),a=VPOP(); VPUSH(a<b); } break;
        case 0x21: { long b=VPOP(),a=VPOP(); VPUSH(a<=b); } break;
        case 0x22: { long b=VPOP(),a=VPOP(); VPUSH(a>b); } break;
        case 0x23: { long b=VPOP(),a=VPOP(); VPUSH(a>=b); } break;
        case 0x24: { long b=VPOP(),a=VPOP(); VPUSH(a==b); } break;
        case 0x25: { long b=VPOP(),a=VPOP(); VPUSH(a!=b); } break;
        case 0x30: pc = vm_u16(code+pc); break;                                 /* JMP */
        case 0x31: { int t=vm_u16(code+pc); pc+=2; if (VPOP()==0) pc=t; } break;/* JMPZ */
        case 0x40: { int mn=vm_u16(code+pc); pc+=2; int na=code[pc++], i;       /* SEND */
                     long va[8]; if (na>8) na=8;
                     for (i=na-1;i>=0;i--) va[i]=VPOP();
                     int recv=(int)VPOP();
                     if (mn>=0 && mn<vm_n_str) {
                         if (par_active) par_send(core, self, recv, vm_str[mn], na, va);
                         else            vm_enqueue(self, recv, vm_str[mn], na, va);
                     } } break;
        case 0x41: { int ci=vm_u16(code+pc); pc+=2;
                     VPUSH(par_active ? par_spawn(core, ci) : vm_spawn(ci)); } break; /* SPAWN */
        case 0x42: { (void)VPOP(); } break;                                     /* PRINT (ignored) */
        case 0x43: pc = clen; break;                                            /* RET */
        case 0x44: { int fi=vm_u16(code+pc); pc+=2; int na=code[pc++]; (void)fi; /* PRINTF (ignored) */
                     for (int i=0;i<na;i++) VPOP(); } break;
        case 0x45: { long col=VPOP(),y2=VPOP(),x2=VPOP(),y1=VPOP(),x1=VPOP();   /* LINE */
                     vm_line((int)x1,(int)y1,(int)x2,(int)y2,(int)col); } break;
        case 0x46: vm_cls(); break;                                             /* CLS */
        case 0x47: { long col=VPOP(),y3=VPOP(),x3=VPOP(),y2=VPOP(),x2=VPOP(),y1=VPOP(),x1=VPOP(); /* TRI */
                     vm_tri((int)x1,(int)y1,(int)x2,(int)y2,(int)x3,(int)y3,(int)col); } break;
        case 0x48: { long pitch=VPOP(), yaw=VPOP();                              /* MESH3D */
                     mesh_yaw0 = mesh_yaw_mrad = (int)yaw;
                     mesh_pitch0 = mesh_pitch_mrad = (int)pitch;
                     vm_mesh3d((int)yaw,(int)pitch); } break;
        default: pc = clen; break;
        }
    }
#undef VPUSH
#undef VPOP
}

/* SMP range fn: dispatch par_batch[lo..hi) on this core (core = 0..3). */
static long par_dispatch_range(long lo, long hi, int core)
{
    for (long i = lo; i < hi; i++)
        avm_dispatch(par_batch[i].self, par_batch[i].sender, par_batch[i].method,
                     par_batch[i].a, par_batch[i].na, core);
    return 0;
}

/* ===== cooperative tick: advance the VM by ONE frame ===================== */
/* Called once per WM redraw (from vmgfx_draw).  Processes queued messages until
 * a WAIT (frame boundary) or the queue empties, then returns control to the WM
 * so the screen + network keep running.  No thread, no busy-wait. */
static int           avm_active;
static unsigned long  avm_last_ms;
static void avm_tick(void)
{
    if (!avm_active) return;

    /* Control mode: a full turn is cached -> the WM controls play/pause/step
     * instead of running the actor.  Auto-advance honours ctl_speed (the WAIT
     * ms); when paused the current cached frame just stays on screen. */
    if (ctl_cached) {
        if (ctl_play && ctl_nframes > 0) {
            unsigned long now = avm_now_ms();
            if (now - ctl_last >= (unsigned long)ctl_speed) {
                ctl_idx = (ctl_idx + ctl_dir + ctl_nframes) % ctl_nframes;
                ctl_show(ctl_idx);
                ctl_last = now;
            }
        }
        return;
    }

    /* AVM2 mesh mode: the toolbar Play/Pause/Stop/</> + checkboxes drive a live
     * turntable.  When spinning, advance yaw and re-render the 3-D mesh; once the
     * first MESH3D has run (mesh_mode set) the actor queue is idle. */
    if (mesh_mode) {
        if (mesh_spin) {
            /* time-based: constant angular velocity regardless of frame rate, and
             * re-render every WM tick for the smoothest motion the 4 cores allow. */
            unsigned long now = avm_now_ms();
            unsigned long dt = mesh_last ? (now - mesh_last) : 16;
            if (dt > 250) dt = 250;                 /* clamp after a stall */
            mesh_yaw_mrad += (int)(dt * MESH_SPIN_RATE / 1000);   /* mrad */
            while (mesh_yaw_mrad > 2*FX_PI) mesh_yaw_mrad -= 2*FX_PI;
            vm_mesh3d(mesh_yaw_mrad, mesh_pitch_mrad);
            mesh_last = now;
        }
        return;
    }

    /* Caching phase: pace by the WAIT ms, advancing one actor frame at a time. */
    if (g_buf_ready && vm_wait_ms > 0) {
        unsigned long now = avm_now_ms();
        if (now - avm_last_ms < (unsigned long)vm_wait_ms) return;
        avm_last_ms = now;
    }
    vm_frame_done = 0;
    long guard = 0;
    while (vm_qh != vm_qt && !vm_frame_done && ++guard < 2000000L) {
        /* Gather a run of par_safe, DISTINCT-actor messages from the head and
         * dispatch them across cores; preserve FIFO by stopping the run at the
         * first draw/WAIT actor or repeat target (those keep the 1-core path). */
        if (avm_par_enable && smp_cores_online() >= 2) {
            static vmmsg_t batch[PAR_BATCH_MAX];
            int batch_self[PAR_BATCH_MAX], bn = 0, qi = vm_qh;
            while (qi != vm_qt && bn < PAR_BATCH_MAX) {
                vmmsg_t *m = &vm_q[qi];
                int ok = m->self >= 0 && m->self < VM_MAXOBJ && vm_obj[m->self].used
                         && vm_class_parsafe[vm_obj[m->self].cls];
                for (int k = 0; ok && k < bn; k++) if (batch_self[k] == m->self) ok = 0;
                if (!ok) break;
                batch[bn] = *m; batch_self[bn] = m->self; bn++;
                qi = (qi + 1) % VM_Q;
            }
            if (bn >= 2) {
                vm_qh = qi;                                   /* consume the batch */
                int nc = smp_cores_online();
                if (nc > bn) nc = bn;
                for (int c = 0; c < PAR_NCORES; c++) {
                    par_qn[c] = 0;
                    par_spawn_next[c] = c * VM_MAXOBJ / PAR_NCORES;
                    par_spawn_hi[c]   = (c + 1) * VM_MAXOBJ / PAR_NCORES;
                }
                par_batch = batch; par_active = 1;
                avm_par_nbatch++; avm_par_lastbn = bn;     /* diag */
                smp_parallel_sum(par_dispatch_range, bn, nc);
                par_active = 0;
                for (int c = 0; c < PAR_NCORES; c++)          /* merge produced sends */
                    for (int j = 0; j < par_qn[c]; j++)
                        vm_enqueue(par_q[c][j].sender, par_q[c][j].self,
                                   par_q[c][j].method, par_q[c][j].na, par_q[c][j].a);
                int mx = 0;                                   /* recompute object high-water */
                for (int i = 0; i < VM_MAXOBJ; i++) if (vm_obj[i].used) mx = i + 1;
                vm_nobj = mx;
                continue;
            }
        }
        vmmsg_t m = vm_q[vm_qh]; vm_qh = (vm_qh + 1) % VM_Q;
        avm_dispatch(m.self, m.sender, m.method, m.a, m.na, 0);
    }
}

/* ---- playback controls (toolbar buttons + arrow keys) ---- */
void avm_ctl(int cmd)   /* 0 play, 1 pause, 2 stop, 3 prev, 4 next */
{
    if (mesh_mode) {                              /* AVM2 mesh turntable */
        switch (cmd) {
            case 0: mesh_spin = 1; mesh_last = 0; break;                       /* Play  */
            case 1: mesh_spin = 0; break;                                      /* Pause */
            case 2: mesh_spin = 0; mesh_yaw_mrad = mesh_yaw0;                  /* Stop  */
                    vm_mesh3d(mesh_yaw_mrad, mesh_pitch_mrad); break;
            case 3: mesh_spin = 0; mesh_yaw_mrad -= 160;                       /* <     */
                    vm_mesh3d(mesh_yaw_mrad, mesh_pitch_mrad); break;
            case 4: mesh_spin = 0; mesh_yaw_mrad += 160;                       /* >     */
                    vm_mesh3d(mesh_yaw_mrad, mesh_pitch_mrad); break;
            default: break;
        }
        return;
    }
    if (!ctl_cached) return;
    switch (cmd) {
        case 0: ctl_play = 1; break;                                  /* Start  */
        case 1: ctl_play = 0; break;                                  /* Pause  */
        case 2: ctl_play = 0; ctl_idx = 0; ctl_show(0); break;        /* Stop   */
        case 3: ctl_play = 0; ctl_idx = (ctl_idx - 1 + ctl_nframes) % ctl_nframes; ctl_show(ctl_idx); break; /* < */
        case 4: ctl_play = 0; ctl_idx = (ctl_idx + 1) % ctl_nframes;  ctl_show(ctl_idx); break;             /* > */
        default: break;
    }
}

/* ===== public entry: load the staged module and run it =================== */
int avm_loadrun(int len)
{
    if (len <= 0 || len > AVM_STAGE_MAX) return -1;
    /* reset runtime */
    avm_ld_active = 0;                             /* upload finished: hide the bar */
    vm_qh = vm_qt = 0; vm_nobj = 0;
    for (int i=0;i<VM_MAXOBJ;i++) vm_obj[i].used = 0;
    v_line_n = 0; v_tri_n = 0; g_buf_ready = 0; mesh_mode = 0;
    mesh_spin = 0; mesh_wire = 0; mesh_flat = 0;
    mesh_aa = 0; mesh_grid = 0; mesh_persp = 0;
    ctl_cached = 0; ctl_nframes = 0; ctl_prevf = -1;    /* fresh turntable: re-cache */
    ctl_play = 1; ctl_idx = 0;
    if (avm_load(avm_stage, len) != 0) return -1;

    /* (Re-)open the VM graphics window if it isn't currently shown.  The user may
     * have closed it with the titlebar [X] after the previous actor; without this
     * the next actor would run with NO window and look like it "didn't start". */
    if (!vmgfx_added || !wm_is_shown(&vmgfx_win)) {
        vmgfx_win.x = 120; vmgfx_win.y = 50;
        vmgfx_win.width = BW + 4;
        vmgfx_win.height = BH + WM_TITLEBAR_H + AVM_TB + AVM_CB + 6;
        const char *t = "VM graphics (AVM)";
        int k; for (k=0;k<WM_TITLE_MAX && t[k];k++) vmgfx_win.title[k]=t[k]; vmgfx_win.title[k]=0;
        vmgfx_win.chrome_color = 0xFFAACCEEU; vmgfx_win.title_bg = 0xFF102030U;
        vmgfx_win.title_fg = 0xFFFFFFFFU; vmgfx_win.content_bg = 0xFF2C3238U;
        vmgfx_win.draw_content = vmgfx_draw;
        vmgfx_win.on_click     = vmgfx_click;
        wm_add(&vmgfx_win);
        vmgfx_added = 1;
    }

    int id = vm_spawn(0);                         /* class 0 = synthetic __boot */
    if (id < 0) return -1;
    vm_enqueue(-1, id, "tick", 0, 0);
    avm_active = 1;                                /* vmgfx_draw will tick it */
    return id;
}
