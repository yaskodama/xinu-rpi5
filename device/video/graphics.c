// device/video/graphics.c — a wm window that renders a 3D wireframe wine glass
// (a surface of revolution) and spins it about all three axes.  Driven entirely
// with integer fixed-point maths (trig scaled by 4096) so it needs no FPU/libm.
//
// The `wine` shell command calls graphics_wine_start(), which kicks off a 30-step
// rotation; graphics_draw() (the window's draw_content) advances and renders it.

#include "wm.h"
#include "video.h"

window_t graphics_win;

/* ---- fixed-point trig (Bhaskara I sine approximation), result scaled by 4096 -- */
static int isin(int deg)
{
    deg %= 360; if (deg < 0) deg += 360;
    int sign = 1;
    if (deg > 180) { deg -= 180; sign = -1; }
    long num = 4L * deg * (180 - deg);
    long den = 40500L - (long)deg * (180 - deg);
    return (int)(sign * (num * 4096) / den);
}
static int icos(int deg) { return isin(deg + 90); }

static int isqrt(int v)
{
    if (v <= 0) return 0;
    int x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

/* "コダマ" as straight strokes in a 230x90 cell (x right, y down); each stroke is
 * extruded into a 3D box (a block) by draw_kodama().  Three chars side by side. */
struct kstroke { short x0, y0, x1, y1; };
static const struct kstroke kodama_strokes[] = {
    /* コ */
    {  5, 10,  60, 10}, { 58, 10,  58, 75}, {  5, 75,  60, 75},
    /* ダ (タ + dakuten), shifted +80 */
    { 88, 18, 125, 12}, {130,  8,  95, 80}, { 90, 40, 128, 40},
    {138,  8, 144, 14}, {138, 18, 144, 24},
    /* マ, shifted +160 */
    {168, 15, 218, 15}, {218, 15, 198, 48}, {200, 40, 178, 82},
};
#define NKS ((int)(sizeof(kodama_strokes)/sizeof(kodama_strokes[0])))

/* ---- wine-glass profile: (radius, height) in model units, bottom -> rim ------- */
#define NP 11
#define NS 14            /* segments around the axis of revolution */
static const int prof_r[NP] = {  0, 60, 60,  8,  8, 16, 46, 56, 50, 50, 52 };
static const int prof_y[NP] = {-105,-105,-96,-90,-12,  0, 28, 58, 84, 98,104 };

/* ---- animation state --------------------------------------------------------- */
static int g_active;             /* 1 while the 30-step spin is running */
static int g_step;               /* 0..30 */
static unsigned g_last_frame;
static int g_ax, g_ay, g_az;     /* current rotation angles (degrees) */

static int g_mode;               /* 0 = wine glass, 1 = four rotating lines */

void graphics_wine_start(void)
{
    g_mode = 0;
    g_active = 1; g_step = 0; g_last_frame = 0;
    g_ax = 0; g_ay = 0; g_az = 0;
}

void graphics_4lines_start(void)
{
    g_mode = 1;
    g_active = 1; g_step = 0; g_last_frame = 0;
    g_ax = 0; g_ay = 0; g_az = 0;
}

void graphics_kodama_start(void)
{
    g_mode = 2;
    g_active = 1; g_step = 0; g_last_frame = 0;
    g_ax = 0; g_ay = 0; g_az = 0;
}

/* rotate (x,y,z) by g_ax,g_ay,g_az; fixed-point >>12. */
static void rotate(int x, int y, int z, int *ox, int *oy)
{
    int s, c, x1, y1, z1, x2, z2;
    /* X axis */
    s = isin(g_ax); c = icos(g_ax);
    y1 = (y*c - z*s) >> 12;
    z1 = (y*s + z*c) >> 12;
    x1 = x;
    /* Y axis */
    s = isin(g_ay); c = icos(g_ay);
    x2 = (x1*c + z1*s) >> 12;
    z2 = (-x1*s + z1*c) >> 12;
    /* Z axis */
    s = isin(g_az); c = icos(g_az);
    *ox = (x2*c - y1*s) >> 12;
    *oy = (x2*s + y1*c) >> 12;
    (void)z2;
}

/* Four line segments, each centred on a corner of a square, all spinning. */
static void draw_4lines(window_t *self)
{
    int cw = self->width - 2;
    int ch = self->height - WM_TITLEBAR_H - 3;
    int cx = self->x + 1 + cw / 2;
    int cy = self->y + WM_TITLEBAR_H + 2 + ch / 2;
    int M = (cw < ch ? cw : ch);
    int Q = M / 4;                  /* half the square's side */
    int R = M / 8;                  /* half the segment length */
    int c = icos(g_ax), s = isin(g_ax);
    int vx[4] = { -Q,  Q,  Q, -Q };
    int vy[4] = { -Q, -Q,  Q,  Q };

    /* (no square drawn — just the four rotating segments) */
    unsigned int col[4] = { 0xFFFF6060u, 0xFF60FF60u, 0xFF6080FFu, 0xFFFFFF60u };
    for (int i = 0; i < 4; i++) {   /* segment centred on each corner, rotated */
        int dx = (R*c) >> 12, dy = (R*s) >> 12;
        draw_line(cx+vx[i]-dx, cy+vy[i]-dy, cx+vx[i]+dx, cy+vy[i]+dy, col[i]);
    }
}

/* "コダマ" — each stroke extruded into a wireframe 3D box, all rotating. */
static void draw_kodama(window_t *self)
{
    int cw = self->width - 2;
    int ch = self->height - WM_TITLEBAR_H - 3;
    int cx = self->x + 1 + cw / 2;
    int cy = self->y + WM_TITLEBAR_H + 2 + ch / 2;
    #define KMAXR 122
    int half = (cw < ch ? cw : ch) / 2 - 6;
    int S = half * 100 / KMAXR;
    if (S < 3) return;

    const int T = 8, HD = 8, CXc = 112, CYc = 45;   /* thickness, half-depth, cell centre */
    unsigned int col = 0xFFFFD060u;                 /* warm gold */
    for (int i = 0; i < NKS; i++) {
        int x0 = kodama_strokes[i].x0, y0 = kodama_strokes[i].y0;
        int x1 = kodama_strokes[i].x1, y1 = kodama_strokes[i].y1;
        int dx = x1 - x0, dy = y1 - y0;
        int len = isqrt(dx*dx + dy*dy); if (len < 1) len = 1;
        int px = -dy * (T/2) / len, py = dx * (T/2) / len;     /* perpendicular */
        int rx[4] = { x0+px, x1+px, x1-px, x0-px };            /* rectangle corners */
        int ry[4] = { y0+py, y1+py, y1-py, y0-py };
        int fx[4], fy[4], bx[4], by[4];
        for (int k = 0; k < 4; k++) {
            int mx = rx[k] - CXc, my = -(ry[k] - CYc);         /* to centred, y-up */
            int ox, oy;
            rotate(mx, my,  HD, &ox, &oy); fx[k] = cx + (ox*S)/100; fy[k] = cy - (oy*S)/100;
            rotate(mx, my, -HD, &ox, &oy); bx[k] = cx + (ox*S)/100; by[k] = cy - (oy*S)/100;
        }
        for (int k = 0; k < 4; k++) {                          /* 12 box edges */
            int j = (k + 1) & 3;
            draw_line(fx[k],fy[k], fx[j],fy[j], col);
            draw_line(bx[k],by[k], bx[j],by[j], col);
            draw_line(fx[k],fy[k], bx[k],by[k], col);
        }
    }
}

void graphics_draw(window_t *self, unsigned int frame)
{
    /* advance the spin every frame (no wait) */
    (void)frame;
    if (g_active) {
        if (g_mode == 1) {                 /* 4lines: 30 deg/frame, 30 full turns */
            g_ax = (g_ax + 30) % 360;      /* 12 frames per turn (smoother) */
            if (++g_step >= 360) g_active = 0;   /* 360 frames = 30 turns */
        } else {                           /* wine / kodama: 30 steps about x/y/z */
            g_ax = (g_ax + 12) % 360;
            g_ay = (g_ay + 8)  % 360;
            g_az = (g_az + 5)  % 360;
            if (++g_step >= 30) g_active = 0;
        }
    }

    if (g_mode == 1) { draw_4lines(self); return; }
    if (g_mode == 2) { draw_kodama(self); return; }

    int cw = self->width - 2;
    int ch = self->height - WM_TITLEBAR_H - 3;
    int cx = self->x + 1 + cw / 2;
    int cy = self->y + WM_TITLEBAR_H + 2 + ch / 2;
    /* Scale to the model's bounding-sphere radius (the farthest profile point
     * from the origin) so NO rotation can push the glass past the window: at any
     * angle a point's projected distance is <= MAXR*S/100 = half. */
    #define MAXR 122
    int half = (cw < ch ? cw : ch) / 2 - 6;    /* leave a small margin */
    int S = half * 100 / MAXR;                 /* MAXR model units -> `half` px */
    if (S < 5) return;

    static int px[NP][NS], py[NP][NS];
    for (int i = 0; i < NP; i++) {
        for (int j = 0; j < NS; j++) {
            int ang = j * (360 / NS);
            int mx = (prof_r[i] * icos(ang)) >> 12;   /* revolve: r*cos */
            int mz = (prof_r[i] * isin(ang)) >> 12;   /*          r*sin */
            int my = prof_y[i];
            int ox, oy;
            rotate(mx, my, mz, &ox, &oy);
            px[i][j] = cx + (ox * S) / 100;
            py[i][j] = cy - (oy * S) / 100;           /* y up -> screen down */
        }
    }

    unsigned int col = 0xFF66CCFFu;                   /* light blue wireframe */
    for (int i = 0; i < NP; i++)                       /* latitude rings */
        for (int j = 0; j < NS; j++) {
            int k = (j + 1) % NS;
            draw_line(px[i][j], py[i][j], px[i][k], py[i][k], col);
        }
    for (int i = 0; i < NP - 1; i++)                   /* longitude lines */
        for (int j = 0; j < NS; j++)
            draw_line(px[i][j], py[i][j], px[i+1][j], py[i+1][j], col);
}
