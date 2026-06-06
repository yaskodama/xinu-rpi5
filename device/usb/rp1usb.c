// device/usb/rp1usb.c — Raspberry Pi 5 USB host (RP1 Synopsys DWC3 / xHCI).
//
// The Pi 5's USB-A ports hang off two DWC3 controllers INSIDE the RP1 I/O chip,
// reached over the same PCIe link we already trained for the Cadence GEM.  From
// rp1-common.dtsi: usb@40200000 + usb@40300000 ("snps,dwc3", dr_mode="host").
// RP1 offset X maps to CPU 0x1F00000000 + X (same window as the GEM at
// 0x1F00100000), so the controllers sit at 0x1F00200000 and 0x1F00300000.
//
// A DWC3 in host mode presents a standard xHCI register interface at offset 0
// (capability regs), with the DWC3-specific global registers up at 0xC000+.
// Because the Pi 5 BOOTS from a USB mass-storage stick, the firmware has already
// brought these controllers up — so this first phase just PROBES them (xHCI
// version + port/slot counts + DWC3 core id) to confirm we can reach a live,
// firmware-initialised host controller before we drive enumeration + HID.

#include "uart.h"

#ifdef RP1_ETH_BASE   /* Pi 5 only (RP1 over PCIe) */

#define RP1_USB0   0x1F00200000UL
#define RP1_USB1   0x1F00300000UL

#define R8(base,off)   (*(volatile unsigned char  *)((base) + (off)))
#define R16(base,off)  (*(volatile unsigned short *)((base) + (off)))
#define R32(base,off)  (*(volatile unsigned int   *)((base) + (off)))

/* xHCI capability registers (offset 0 from the controller base). */
#define XHCI_CAPLENGTH    0x00   /* 8-bit: bytes to the operational regs */
#define XHCI_HCIVERSION   0x02   /* 16-bit: BCD, expect 0x0100 / 0x0110   */
#define XHCI_HCSPARAMS1   0x04   /* [31:24]=MaxPorts [7:0]=MaxSlots       */
#define XHCI_HCCPARAMS1   0x10
/* DWC3 global registers. */
#define DWC3_GSNPSID      0xC120 /* core id + release: 0x5533xxxx etc.    */
#define DWC3_GCTL         0xC110

static unsigned int g_usb_caplen[2], g_usb_ver[2], g_usb_hcs1[2], g_usb_snpsid[2];
static unsigned int g_portsc[2][8];     /* PORTSC per controller per port */
static int          g_usb_probed;

/* xHCI port-status register, PORTSC, for 1-based port `p` (1..MaxPorts):
 * operational base = base + CAPLENGTH; port registers at +0x400 + (p-1)*0x10. */
static unsigned int portsc(unsigned long base, int p)
{
    unsigned int caplen = R8(base, XHCI_CAPLENGTH);
    return R32(base, caplen + 0x400 + (p - 1) * 0x10);
}

static void puts_hex(const char *tag, unsigned int v)
{
    uart_puts(tag);
    static const char hx[] = "0123456789abcdef";
    char b[11]; b[0]='0'; b[1]='x';
    for (int i = 0; i < 8; i++) b[2+i] = hx[(v >> ((7-i)*4)) & 0xF];
    b[10] = 0; uart_puts(b); uart_puts(" ");
}

static void probe_one(int idx, unsigned long base)
{
    unsigned int caplen = R8 (base, XHCI_CAPLENGTH);
    unsigned int ver    = R16(base, XHCI_HCIVERSION);
    unsigned int hcs1   = R32(base, XHCI_HCSPARAMS1);
    unsigned int snpsid = R32(base, DWC3_GSNPSID);
    g_usb_caplen[idx] = caplen; g_usb_ver[idx] = ver;
    g_usb_hcs1[idx] = hcs1;     g_usb_snpsid[idx] = snpsid;

    uart_puts("rp1usb: ctrl"); uart_putc((char)('0'+idx)); uart_puts(" @0x");
    { static const char hx[]="0123456789abcdef"; char b[11]; b[0]='0';b[1]='x';
      for(int i=0;i<8;i++) b[2+i]=hx[((unsigned)(base>>32==0?base:base)>> ((7-i)*4))&0xF]; b[10]=0; uart_puts(b);}
    uart_puts(" ");
    puts_hex("caplen=", caplen);
    puts_hex("hciver=", ver);
    puts_hex("hcsp1=", hcs1);
    puts_hex("snpsid=", snpsid);
    /* HCSPARAMS1: ports = bits 31:24, slots = bits 7:0 */
    int nports = (int)((hcs1 >> 24) & 0xff);
    uart_puts("ports="); uart_putc((char)('0' + nports/10%10)); uart_putc((char)('0' + nports%10));
    uart_puts("\n");

    /* Read each port's PORTSC; bit0 = CCS (device connected), bits13:10=speed. */
    for (int p = 1; p <= nports && p <= 8; p++) {
        unsigned int sc = portsc(base, p);
        g_portsc[idx][p-1] = sc;
        uart_puts("  port"); uart_putc((char)('0'+p));
        puts_hex(" PORTSC=", sc);
        uart_puts((sc & 1) ? "CONNECTED\n" : "empty\n");
    }
}

void rp1usb_probe(void)
{
    uart_puts("rp1usb: probing RP1 DWC3/xHCI host controllers\n");
    probe_one(0, RP1_USB0);
    probe_one(1, RP1_USB1);
    g_usb_probed = 1;
}

/* ====================================================================
 * Phase 3a — xHCI controller bring-up on controller 0 (the mouse is on
 * c0p2, Low speed).  Reset the HC, build our own DCBAA + command ring +
 * event ring (+ scratchpad), then set Run.  All DMA structures are static,
 * 64-byte aligned, in low RAM; the controller reaches them over PCIe at
 * physical + 0x1000000000 (same inbound window as the GEM).
 * ==================================================================== */

#define XDMA_OFFSET 0x1000000000UL
#define XDA(p)      (((unsigned long)(p) & 0xffffffffffUL) + XDMA_OFFSET)

/* operational register offsets (from oper base = base + CAPLENGTH) */
#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_CRCR     0x18
#define OP_DCBAAP   0x30
#define OP_CONFIG   0x38
/* capability regs */
#define CAP_HCSPARAMS2 0x08
#define CAP_DBOFF      0x14
#define CAP_RTSOFF     0x18
/* runtime interrupter-0 (from rt base + 0x20) */
#define IR0_IMAN    0x20
#define IR0_ERSTSZ  0x28
#define IR0_ERSTBA  0x30
#define IR0_ERDP    0x38

#define USBCMD_RS   (1u<<0)
#define USBCMD_HCRST (1u<<1)
#define USBCMD_INTE (1u<<2)
#define USBSTS_HCH  (1u<<0)
#define USBSTS_CNR  (1u<<11)

struct trb { unsigned int p0, p1, status, control; };

#define CMD_RING_N   64
#define EVT_RING_N   64
#define NSCRATCH_MAX 64

static unsigned long  g_xhci_base;
static unsigned long  g_oper, g_rt, g_db;
static unsigned long long g_dcbaa[256]      __attribute__((aligned(64)));
static struct trb     g_cmd_ring[CMD_RING_N] __attribute__((aligned(64)));
static struct trb     g_evt_ring[EVT_RING_N] __attribute__((aligned(64)));
static unsigned long long g_erst[4]         __attribute__((aligned(64)));
static unsigned long long g_scratch_arr[NSCRATCH_MAX] __attribute__((aligned(64)));
static unsigned char  g_scratch_buf[NSCRATCH_MAX][4096] __attribute__((aligned(4096)));
static int            g_cmd_idx, g_cmd_cycle = 1;
static int            g_evt_idx, g_evt_cycle = 1;
static unsigned int   g_xhci_usbsts_after;
static int            g_xhci_running;

static void xdelay(unsigned long us)
{
    unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0":"=r"(f));
    unsigned long t0; __asm__ volatile ("mrs %0, cntpct_el0":"=r"(t0));
    unsigned long d = (f/1000000UL)*us;
    for (;;){ unsigned long t; __asm__ volatile("mrs %0, cntpct_el0":"=r"(t)); if (t-t0>=d) break; }
}

int rp1usb_xhci_init(void)
{
    unsigned long base = RP1_USB0;
    g_xhci_base = base;
    unsigned int caplen = R8(base, XHCI_CAPLENGTH);
    g_oper = base + caplen;
    g_rt   = base + (R32(base, CAP_RTSOFF) & ~0x1Fu);
    g_db   = base + (R32(base, CAP_DBOFF)  & ~0x3u);

    /* 1. Stop, then reset the controller. */
    R32(g_oper, OP_USBCMD) &= ~USBCMD_RS;
    for (int i=0;i<100 && !(R32(g_oper,OP_USBSTS)&USBSTS_HCH);i++) xdelay(1000);
    R32(g_oper, OP_USBCMD) |= USBCMD_HCRST;
    for (int i=0;i<200 && (R32(g_oper,OP_USBCMD)&USBCMD_HCRST);i++) xdelay(1000);
    for (int i=0;i<200 && (R32(g_oper,OP_USBSTS)&USBSTS_CNR);i++)   xdelay(1000);

    /* 2. DCBAA + scratchpad. */
    for (int i=0;i<256;i++) g_dcbaa[i]=0;
    unsigned int hcs2 = R32(base, CAP_HCSPARAMS2);
    int nscratch = (int)(((hcs2>>27)&0x1f) | (((hcs2>>21)&0x1f)<<5));
    if (nscratch > NSCRATCH_MAX) nscratch = NSCRATCH_MAX;
    for (int i=0;i<nscratch;i++) g_scratch_arr[i] = XDA(&g_scratch_buf[i][0]);
    if (nscratch > 0) g_dcbaa[0] = XDA(&g_scratch_arr[0]);
    R32(g_oper, OP_DCBAAP)     = (unsigned int)(XDA(g_dcbaa) & 0xffffffff);
    R32(g_oper, OP_DCBAAP+4)   = (unsigned int)(XDA(g_dcbaa) >> 32);

    /* 3. Command ring. */
    for (int i=0;i<CMD_RING_N;i++){ g_cmd_ring[i].p0=0;g_cmd_ring[i].p1=0;g_cmd_ring[i].status=0;g_cmd_ring[i].control=0; }
    /* link TRB at end -> back to start, toggle cycle */
    g_cmd_ring[CMD_RING_N-1].p0 = (unsigned int)(XDA(g_cmd_ring)&0xffffffff);
    g_cmd_ring[CMD_RING_N-1].p1 = (unsigned int)(XDA(g_cmd_ring)>>32);
    g_cmd_ring[CMD_RING_N-1].control = (6u<<10) | (1u<<1) | 1u;  /* Link TRB, TC=1, cycle */
    g_cmd_idx=0; g_cmd_cycle=1;
    R32(g_oper, OP_CRCR)   = (unsigned int)((XDA(g_cmd_ring)&0xffffffff) | 1u);  /* RCS=1 */
    R32(g_oper, OP_CRCR+4) = (unsigned int)(XDA(g_cmd_ring)>>32);

    /* 4. Event ring + ERST (interrupter 0). */
    for (int i=0;i<EVT_RING_N;i++){ g_evt_ring[i].p0=0;g_evt_ring[i].p1=0;g_evt_ring[i].status=0;g_evt_ring[i].control=0; }
    g_evt_idx=0; g_evt_cycle=1;
    g_erst[0] = XDA(g_evt_ring);
    g_erst[1] = (unsigned long long)EVT_RING_N;   /* segment size in low 16 bits */
    R32(g_rt, IR0_ERSTSZ) = 1;
    R32(g_rt, IR0_ERDP)   = (unsigned int)(XDA(g_evt_ring)&0xffffffff);
    R32(g_rt, IR0_ERDP+4) = (unsigned int)(XDA(g_evt_ring)>>32);
    R32(g_rt, IR0_ERSTBA) = (unsigned int)(XDA(g_erst)&0xffffffff);
    R32(g_rt, IR0_ERSTBA+4)=(unsigned int)(XDA(g_erst)>>32);
    R32(g_rt, IR0_IMAN)   = 0x2;                  /* IE=0, clear IP */

    /* 5. CONFIG: announce max device slots = MaxSlots from HCSPARAMS1. */
    R32(g_oper, OP_CONFIG) = (R32(base, XHCI_HCSPARAMS1) & 0xff);

    /* 6. Run. */
    __asm__ volatile ("dsb sy" ::: "memory");
    R32(g_oper, OP_USBCMD) |= USBCMD_RS;
    for (int i=0;i<200 && (R32(g_oper,OP_USBSTS)&USBSTS_HCH);i++) xdelay(1000);

    g_xhci_usbsts_after = R32(g_oper, OP_USBSTS);
    g_xhci_running = !(g_xhci_usbsts_after & USBSTS_HCH);

    uart_puts("rp1usb: xHCI init; USBSTS=0x");
    { static const char hx[]="0123456789abcdef"; char b[9]; for(int i=0;i<8;i++)b[i]=hx[(g_xhci_usbsts_after>>((7-i)*4))&0xF]; b[8]=0; uart_puts(b);}
    uart_puts(g_xhci_running ? " RUNNING\n" : " halted\n");
    return g_xhci_running ? 0 : -1;
}

unsigned int rp1usb_usbsts(void)   { return g_xhci_usbsts_after; }
int          rp1usb_running(void)  { return g_xhci_running; }

/* ---------- Phase 3b: command ring submit + event wait + port reset + slot ----- */

/* Push a command TRB (p0/p1/status given; control's low type/flags given,
 * cycle added) and ring the command doorbell.  Handles the link-TRB wrap. */
static void cmd_submit(unsigned int p0, unsigned int p1, unsigned int status, unsigned int control)
{
    struct trb *t = &g_cmd_ring[g_cmd_idx];
    t->p0 = p0; t->p1 = p1; t->status = status;
    t->control = (control & ~1u) | (g_cmd_cycle & 1u);
    __asm__ volatile ("dsb sy" ::: "memory");
    g_cmd_idx++;
    if (g_cmd_idx == CMD_RING_N - 1) {            /* hit the link TRB: wrap */
        g_cmd_ring[CMD_RING_N-1].control = (g_cmd_ring[CMD_RING_N-1].control & ~1u) | (g_cmd_cycle & 1u);
        __asm__ volatile ("dsb sy" ::: "memory");
        g_cmd_idx = 0; g_cmd_cycle ^= 1;
    }
    R32(g_db, 0) = 0;                              /* doorbell 0 = command ring */
    __asm__ volatile ("dsb sy" ::: "memory");
}

/* Poll the event ring for one event whose cycle matches; returns its TRB by
 * value (control==0 if none within the timeout).  Advances ERDP. */
static struct trb event_wait(unsigned int want_type)
{
    struct trb ev; ev.p0=ev.p1=ev.status=ev.control=0;
    for (int spin = 0; spin < 500; spin++) {
        struct trb *e = &g_evt_ring[g_evt_idx];
        __asm__ volatile ("dsb sy" ::: "memory");
        if ((e->control & 1u) == (unsigned)g_evt_cycle) {
            ev = *e;
            g_evt_idx++;
            if (g_evt_idx == EVT_RING_N) { g_evt_idx = 0; g_evt_cycle ^= 1; }
            /* advance ERDP to the next slot (+ EHB clear) */
            unsigned long erdp = XDA(&g_evt_ring[g_evt_idx]);
            R32(g_rt, IR0_ERDP)   = (unsigned int)((erdp & 0xffffffff) | (1u<<3));
            R32(g_rt, IR0_ERDP+4) = (unsigned int)(erdp >> 32);
            unsigned int type = (ev.control >> 10) & 0x3f;
            if (want_type == 0 || type == want_type) return ev;
            spin = 0;                              /* consumed an unrelated event; keep going */
            continue;
        }
        xdelay(1000);
    }
    return ev;
}

static unsigned int g_enum_portsc, g_enum_slotid, g_enum_cc;

/* Reset port `p` (1-based) on controller 0 and Enable Slot. */
int rp1usb_enum_slot(int p)
{
    unsigned long psc = g_oper + 0x400 + (p-1)*0x10;   /* PORTSC, from OPER base */
    /* Port reset: write PR (bit4) + PP (bit9); change bits are RW1C. */
    R32(psc, 0) = (1u<<9) | (1u<<4);
    for (int i=0;i<100;i++){ xdelay(1000); if (!(R32(psc,0)&(1u<<4))) break; }  /* PR clears when done */
    xdelay(20000);
    g_enum_portsc = R32(psc, 0);

    /* Enable Slot (TRB type 9). */
    cmd_submit(0, 0, 0, (9u<<10));
    struct trb ev = event_wait(33);                /* Command Completion Event */
    g_enum_cc     = (ev.status >> 24) & 0xff;       /* completion code (1=success) */
    g_enum_slotid = (ev.control >> 24) & 0xff;      /* assigned slot id */

    uart_puts("rp1usb: enable-slot cc="); uart_putc((char)('0'+g_enum_cc/10%10)); uart_putc((char)('0'+g_enum_cc%10));
    uart_puts(" slot="); uart_putc((char)('0'+g_enum_slotid/10%10)); uart_putc((char)('0'+g_enum_slotid%10));
    uart_puts("\n");
    return (g_enum_cc == 1) ? (int)g_enum_slotid : -1;
}

unsigned int rp1usb_enum_portsc(void){ return g_enum_portsc; }
unsigned int rp1usb_enum_cc(void)    { return g_enum_cc; }
unsigned int rp1usb_enum_slotid(void){ return g_enum_slotid; }

/* On-screen accessors (HDMI, since serial is unreliable). */
unsigned int rp1usb_caplen(int i){ return (i>=0&&i<2)?g_usb_caplen[i]:0; }
unsigned int rp1usb_ver(int i)   { return (i>=0&&i<2)?g_usb_ver[i]:0; }
unsigned int rp1usb_hcs1(int i)  { return (i>=0&&i<2)?g_usb_hcs1[i]:0; }
unsigned int rp1usb_snpsid(int i){ return (i>=0&&i<2)?g_usb_snpsid[i]:0; }
int          rp1usb_ports(int i) { return (i>=0&&i<2)?(int)((g_usb_hcs1[i]>>24)&0xff):0; }
unsigned int rp1usb_portsc(int i, int p){ return (i>=0&&i<2&&p>=0&&p<8)?g_portsc[i][p]:0; }
/* Count connected ports across both controllers (CCS bit). */
int          rp1usb_connected(void)
{
    int c = 0;
    for (int i = 0; i < 2; i++) for (int p = 0; p < 8; p++) if (g_portsc[i][p] & 1) c++;
    return c;
}

#endif /* RP1_ETH_BASE */
