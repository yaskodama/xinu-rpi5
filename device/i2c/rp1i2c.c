// device/i2c/rp1i2c.c — Raspberry Pi 5 RP1 I2C1 (DesignWare DW_apb_i2c), master, polled.
//
// The Pi 5 header GPIOs live inside the RP1 (over PCIe).  i2c1 is the pair on
// GPIO2 (SDA) / GPIO3 (SCL) — the same bus Linux exposes as /dev/i2c-1, and the
// one the Yahboom DOFBOT driver board (I2C slave 0x15) sits on.
//
//   rp1.dtsi:  i2c1 = 0x74000 in the RP1 window (0x1F00000000 + 0x74000)
//   pinctrl-rp1.c: GPIO2/3 FUNCSEL 3 = i2c1
//   clocks: clk_sys (200 MHz), always on — no gate to open.
//
// Register layout is the standard DesignWare one (Linux i2c-designware-core.h).
// Everything is polled with a cntpct deadline; nothing here can hang the box.
// The block answers 0xDEADDEAD (RP1's poison) if it is not clocked, so
// rp1i2c_init() checks IC_COMP_TYPE first and refuses to touch anything else.
#include "uart.h"
#ifdef RP1_I2C1_BASE

#define R(off)  (*(volatile unsigned int *)(RP1_I2C1_BASE + (off)))
#define IC_CON            0x00
#define IC_TAR            0x04
#define IC_DATA_CMD       0x10
#define IC_SS_SCL_HCNT    0x14
#define IC_SS_SCL_LCNT    0x18
#define IC_INTR_MASK      0x30
#define IC_RAW_INTR_STAT  0x34
#define IC_RX_TL          0x38
#define IC_TX_TL          0x3c
#define IC_CLR_INTR       0x40
#define IC_CLR_TX_ABRT    0x54
#define IC_ENABLE         0x6c
#define IC_STATUS         0x70
#define IC_TXFLR          0x74
#define IC_RXFLR          0x78
#define IC_SDA_HOLD       0x7c
#define IC_TX_ABRT_SOURCE 0x80
#define IC_ENABLE_STATUS  0x9c
#define IC_COMP_PARAM_1   0xf4
#define IC_COMP_VERSION   0xf8
#define IC_COMP_TYPE      0xfc
#define DW_COMP_TYPE_VALUE 0x44570140u

#define CON_MASTER        (1u << 0)
#define CON_SPEED_STD     (1u << 1)
#define CON_RESTART_EN    (1u << 5)
#define CON_SLAVE_DISABLE (1u << 6)
#define CMD_READ          (1u << 8)
#define CMD_STOP          (1u << 9)
#define CMD_RESTART       (1u << 10)
#define ST_TFNF           (1u << 1)     /* TX FIFO not full  */
#define ST_TFE            (1u << 2)     /* TX FIFO empty     */
#define ST_RFNE           (1u << 3)     /* RX FIFO not empty */
#define ST_MST_ACTIVITY   (1u << 5)
#define INTR_TX_ABRT      (1u << 6)

/* RP1 GPIO bank 0: CTRL (FUNCSEL) and PADS, same layout rp1eth.c uses. */
#define RP1_GPIO_IO    0x1F000D0000UL
#define RP1_GPIO_PADS  0x1F000F0000UL
#define G_CTRL(pn)  (*(volatile unsigned int *)(RP1_GPIO_IO   + (unsigned long)(pn)*8 + 4))
#define G_PAD(pn)   (*(volatile unsigned int *)(RP1_GPIO_PADS + 4 + (unsigned long)(pn)*4))
#define PAD_OD      (1u << 7)
#define PAD_IE      (1u << 6)
#define PAD_PUE     (1u << 3)
#define PAD_PDE     (1u << 2)
#define PAD_SCHMITT (1u << 1)
#define FUNCSEL_I2C1 3u

static int  i2c_ok = 0;                 /* init succeeded */
static unsigned int i2c_cur_tar = 0;    /* IC_TAR currently programmed */
static unsigned int last_abrt = 0;      /* IC_TX_ABRT_SOURCE of the last failure */
static unsigned long n_xfer = 0, n_fail = 0, n_timeout = 0;

static unsigned long now_ticks(void) { unsigned long v; __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(v)); return v; }
static unsigned long ms_ticks(unsigned ms) { unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f)); return (f / 1000u) * ms + 1; }

/* Wait until (R(IC_STATUS) & mask) == want, at most `ms`.  0 ok, -1 timeout. */
static int wait_status(unsigned int mask, unsigned int want, unsigned ms)
{
    unsigned long end = now_ticks() + ms_ticks(ms);
    while ((R(IC_STATUS) & mask) != want) {
        if (now_ticks() > end) { n_timeout++; return -1; }
    }
    return 0;
}

static void put_hex32(unsigned int v)
{
    for (int i = 7; i >= 0; i--) { unsigned int n = (v >> (i * 4)) & 0xF; uart_putc((char)(n < 10 ? '0' + n : 'a' + n - 10)); }
}

static void dw_disable(void)
{
    R(IC_ENABLE) = 0;
    unsigned long end = now_ticks() + ms_ticks(10);
    while ((R(IC_ENABLE_STATUS) & 1u) && now_ticks() < end) { }
}

static void set_target(unsigned int addr)
{
    if (i2c_cur_tar == addr) return;
    dw_disable();
    R(IC_TAR) = addr & 0x3ff;
    R(IC_ENABLE) = 1;
    i2c_cur_tar = addr;
}

/* After a transfer: wait for TX FIFO drain + bus idle, then check TX_ABRT.
 * Returns 0 on success, -1 on abort (last_abrt holds the source), -2 on timeout. */
static int finish(unsigned ms)
{
    if (wait_status(ST_TFE, ST_TFE, ms) < 0) goto tmo;
    if (wait_status(ST_MST_ACTIVITY, 0, ms) < 0) goto tmo;
    if (R(IC_RAW_INTR_STAT) & INTR_TX_ABRT) {
        last_abrt = R(IC_TX_ABRT_SOURCE);
        (void)R(IC_CLR_TX_ABRT);
        n_fail++;
        return -1;
    }
    return 0;
tmo:
    /* Recover: drop whatever is stuck in the FIFOs by cycling ENABLE. */
    last_abrt = R(IC_TX_ABRT_SOURCE);
    dw_disable(); R(IC_ENABLE) = 1;
    n_fail++;
    return -2;
}

/* Bring the block up in 100 kHz standard mode.  1 ok, 0 not present. */
int rp1i2c_init(void)
{
    unsigned int t = R(IC_COMP_TYPE);
    uart_puts("rp1i2c: IC_COMP_TYPE 0x"); put_hex32(t);
    uart_puts(" version 0x"); put_hex32(R(IC_COMP_VERSION)); uart_puts("\n");
    if (t != DW_COMP_TYPE_VALUE) { uart_puts("rp1i2c: not a DesignWare I2C (unclocked?) — disabled\n"); return 0; }

    /* GPIO2 (SDA) / GPIO3 (SCL): pull-up, input enable, open-drain driver on, FUNCSEL i2c1. */
    for (int pn = 2; pn <= 3; pn++) {
        unsigned int pad = G_PAD(pn);
        pad &= ~(PAD_OD | PAD_PDE);
        pad |=  (PAD_IE | PAD_PUE | PAD_SCHMITT);
        G_PAD(pn) = pad;
        G_CTRL(pn) = FUNCSEL_I2C1;
    }

    dw_disable();
    R(IC_CON) = CON_MASTER | CON_SPEED_STD | CON_RESTART_EN | CON_SLAVE_DISABLE;
    /* clk_sys 200 MHz: tHIGH 4.0us+tf -> ~850, tLOW 4.7us+tf -> ~1000 (DW formulas, Linux i2c_dw_scl_*cnt). */
    R(IC_SS_SCL_HCNT) = 850;
    R(IC_SS_SCL_LCNT) = 1000;
    R(IC_SDA_HOLD)    = 60;             /* 300 ns hold — safe for the STM8 on the arm board */
    R(IC_RX_TL) = 0; R(IC_TX_TL) = 0;
    R(IC_INTR_MASK) = 0;                /* polled */
    R(IC_TAR) = 0x15; i2c_cur_tar = 0x15;
    R(IC_ENABLE) = 1;
    i2c_ok = 1;
    uart_puts("rp1i2c: i2c1 up (GPIO2/3, 100 kHz)\n");
    return 1;
}

int rp1i2c_present(void) { return i2c_ok; }

/* Write `n` bytes (typically reg + payload) to slave `addr`.  0 ok, <0 fail. */
int rp1i2c_write(unsigned int addr, const unsigned char *buf, int n)
{
    if (!i2c_ok || n <= 0) return -3;
    n_xfer++;
    set_target(addr);
    (void)R(IC_CLR_INTR);
    for (int i = 0; i < n; i++) {
        if (wait_status(ST_TFNF, ST_TFNF, 100) < 0) return finish(1), -2;
        R(IC_DATA_CMD) = (unsigned int)buf[i] | (i == n - 1 ? CMD_STOP : 0);
    }
    return finish(100);   /* 腕の基板はサーボと通信中だと 20 ms を超えて待たせる（実測） */
}

/* Write `wn` bytes (usually one register number), repeated-START, then read `rn` bytes. */
int rp1i2c_write_read(unsigned int addr, const unsigned char *wbuf, int wn, unsigned char *rbuf, int rn)
{
    if (!i2c_ok || rn <= 0 || rn > 16) return -3;
    n_xfer++;
    set_target(addr);
    (void)R(IC_CLR_INTR);
    for (int i = 0; i < wn; i++) {
        if (wait_status(ST_TFNF, ST_TFNF, 100) < 0) return finish(1), -2;
        R(IC_DATA_CMD) = (unsigned int)wbuf[i];
    }
    for (int i = 0; i < rn; i++) {
        if (wait_status(ST_TFNF, ST_TFNF, 100) < 0) return finish(1), -2;
        R(IC_DATA_CMD) = CMD_READ | (i == 0 && wn > 0 ? CMD_RESTART : 0) | (i == rn - 1 ? CMD_STOP : 0);
    }
    unsigned long end = now_ticks() + ms_ticks(30);
    int got = 0;
    while (got < rn) {
        if (R(IC_RAW_INTR_STAT) & INTR_TX_ABRT) break;
        if (R(IC_STATUS) & ST_RFNE) { rbuf[got++] = (unsigned char)(R(IC_DATA_CMD) & 0xff); continue; }
        if (now_ticks() > end) { n_timeout++; break; }
    }
    int r = finish(20);
    if (r < 0) return r;
    return got == rn ? 0 : -2;
}

/* Probe: zero-length-ish ping = write one byte 0x00 to the address and see if it ACKs. */
int rp1i2c_probe(unsigned int addr)
{
    unsigned char z = 0;
    return rp1i2c_write(addr, &z, 1) == 0;
}

void rp1i2c_stats(char *out, int cap)
{
    int o = 0;
    #define P(s) do { for (const char *p_ = (s); *p_ && o < cap - 1; p_++) out[o++] = *p_; } while (0)
    #define PN(v) do { char nb[24]; int k = 0; unsigned long v_ = (unsigned long)(v); if (!v_) nb[k++] = '0'; while (v_) { nb[k++] = (char)('0' + v_ % 10); v_ /= 10; } while (k) { if (o < cap - 1) out[o++] = nb[--k]; else k = 0; } } while (0)
    P("i2c1 "); P(i2c_ok ? "up" : "absent"); P(" xfer="); PN(n_xfer); P(" fail="); PN(n_fail); P(" timeout="); PN(n_timeout);
    P(" abrt_src=0x"); { const char *hx = "0123456789abcdef"; for (int i = 7; i >= 0; i--) if (o < cap - 1) out[o++] = hx[(last_abrt >> (i * 4)) & 15]; }
    P(" status=0x"); { unsigned int s = i2c_ok ? R(IC_STATUS) : 0; const char *hx = "0123456789abcdef"; for (int i = 7; i >= 0; i--) if (o < cap - 1) out[o++] = hx[(s >> (i * 4)) & 15]; }
    P("\n");
    out[o] = 0;
    #undef P
    #undef PN
}

#else  /* no RP1 (Pi 4 / QEMU builds): the arm layer still links, but reports "no bus" */
int rp1i2c_init(void) { return 0; }
int rp1i2c_present(void) { return 0; }
int rp1i2c_write(unsigned int addr, const unsigned char *buf, int n) { (void)addr; (void)buf; (void)n; return -3; }
int rp1i2c_write_read(unsigned int addr, const unsigned char *wbuf, int wn, unsigned char *rbuf, int rn) { (void)addr; (void)wbuf; (void)wn; (void)rbuf; (void)rn; return -3; }
int rp1i2c_probe(unsigned int addr) { (void)addr; return 0; }
void rp1i2c_stats(char *out, int cap) { const char *s = "i2c1 absent (no RP1 on this board)\n"; int i = 0; while (s[i] && i < cap - 1) { out[i] = s[i]; i++; } out[i] = 0; }
#endif
