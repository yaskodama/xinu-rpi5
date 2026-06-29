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

/* The RP1/DWC3 USB MMIO is 32-bit-access-only: a byte (R8) read of CAPLENGTH or
 * a half-word (R16) read of HCIVERSION returns garbage, and a garbage CAPLENGTH
 * then makes the PORTSC offsets unaligned -> Device-memory alignment fault.  So
 * always read the 32-bit word at offset 0 and slice it. */
static unsigned int xcaplen(unsigned long base){ return (*(volatile unsigned int *)(base)) & 0xffu; }
static unsigned int xhciver(unsigned long base){ return ((*(volatile unsigned int *)(base)) >> 16) & 0xffffu; }

/* xHCI capability registers (offset 0 from the controller base). */
#define XHCI_CAPLENGTH    0x00   /* 8-bit: bytes to the operational regs */
#define XHCI_HCIVERSION   0x02   /* 16-bit: BCD, expect 0x0100 / 0x0110   */
#define XHCI_HCSPARAMS1   0x04   /* [31:24]=MaxPorts [7:0]=MaxSlots       */
#define XHCI_HCCPARAMS1   0x10
/* DWC3 global registers. */
#define DWC3_GSNPSID      0xC120 /* core id + release: 0x5533xxxx etc.    */
#define DWC3_GCTL         0xC110
#define DWC3_GUSB2PHYCFG0 0xC200 /* bit6 SUSPHY, bit8 ENBLSLPM            */
#define DWC3_GUSB3PIPECTL0 0xC2C0 /* bit17 SUSPHY (USB3)                  */

static unsigned int g_usb_caplen[2], g_usb_ver[2], g_usb_hcs1[2], g_usb_snpsid[2];
static unsigned int g_portsc[2][8];     /* PORTSC per controller per port */
static int          g_usb_probed;

/* xHCI port-status register, PORTSC, for 1-based port `p` (1..MaxPorts):
 * operational base = base + CAPLENGTH; port registers at +0x400 + (p-1)*0x10. */
static unsigned int portsc(unsigned long base, int p)
{
    unsigned int caplen = xcaplen(base);
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
    unsigned int caplen = xcaplen(base);
    unsigned int ver    = xhciver(base);
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

/* Per-controller ring memory + saved register/cursor state.  We run BOTH RP1
 * DWC3/xHCI controllers at once (mouse on one, keyboard on the other), so each
 * needs its own DCBAA / command ring / event ring / scratchpad — they can't
 * share.  xhci_switch() points the loose working-set globals at one controller's
 * memory and restores its cursors; binding and the pump operate on whichever is
 * current. */
struct ctrlmem {
    unsigned long long dcbaa[256]                 __attribute__((aligned(64)));
    struct trb         cmd_ring[CMD_RING_N]       __attribute__((aligned(64)));
    struct trb         evt_ring[EVT_RING_N]       __attribute__((aligned(64)));
    unsigned long long erst[4]                    __attribute__((aligned(64)));
    unsigned long long scratch_arr[NSCRATCH_MAX]  __attribute__((aligned(64)));
    unsigned char      scratch_buf[NSCRATCH_MAX][4096] __attribute__((aligned(4096)));
    unsigned long base, oper, rt, db;
    int   ctx_stride, cmd_idx, cmd_cycle, evt_idx, evt_cycle, inited;
} __attribute__((aligned(4096)));
static struct ctrlmem g_cm[2];
static int            g_active_ctrl = 0;

static unsigned long  g_xhci_base;
static unsigned long  g_sel_base = RP1_USB0;   /* which DWC3/xHCI controller to init */
static unsigned long  g_oper, g_rt, g_db;
/* loose working-set buffer pointers -> the active controller's ctrlmem */
static unsigned long long *g_dcbaa       = g_cm[0].dcbaa;
static struct trb         *g_cmd_ring    = g_cm[0].cmd_ring;
static struct trb         *g_evt_ring    = g_cm[0].evt_ring;
static unsigned long long *g_erst        = g_cm[0].erst;
static unsigned long long *g_scratch_arr = g_cm[0].scratch_arr;
static unsigned char     (*g_scratch_buf)[4096] = g_cm[0].scratch_buf;
static int            g_cmd_idx, g_cmd_cycle = 1;
static int            g_evt_idx, g_evt_cycle = 1;
static unsigned int   g_xhci_usbsts_after;
static int            g_xhci_running;
static int            g_ctx_stride = 32;   /* 64 if HCCPARAMS1.CSZ=1 */

/* Make controller `i` current: save the active controller's cursors, then load
 * controller i's registers/cursors and point the buffer globals at its memory. */
static void xhci_switch(int i)
{
    if (i == g_active_ctrl &&
        g_evt_ring == g_cm[i].evt_ring) return;     /* already current */
    struct ctrlmem *a = &g_cm[g_active_ctrl];
    a->oper=g_oper; a->rt=g_rt; a->db=g_db; a->base=g_xhci_base; a->ctx_stride=g_ctx_stride;
    a->cmd_idx=g_cmd_idx; a->cmd_cycle=g_cmd_cycle; a->evt_idx=g_evt_idx; a->evt_cycle=g_evt_cycle;
    struct ctrlmem *n = &g_cm[i];
    g_active_ctrl=i;
    g_oper=n->oper; g_rt=n->rt; g_db=n->db; g_xhci_base=n->base; g_ctx_stride=n->ctx_stride;
    g_cmd_idx=n->cmd_idx; g_cmd_cycle=n->cmd_cycle; g_evt_idx=n->evt_idx; g_evt_cycle=n->evt_cycle;
    g_dcbaa=n->dcbaa; g_cmd_ring=n->cmd_ring; g_evt_ring=n->evt_ring; g_erst=n->erst;
    g_scratch_arr=n->scratch_arr; g_scratch_buf=n->scratch_buf;
}
void rp1usb_select_ctrl(int which){ which=(which==1)?1:0; xhci_switch(which); g_sel_base=which?RP1_USB1:RP1_USB0; }

static void xdelay(unsigned long us)
{
    unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0":"=r"(f));
    unsigned long t0; __asm__ volatile ("mrs %0, cntpct_el0":"=r"(t0));
    unsigned long d = (f/1000000UL)*us;
    for (;;){ unsigned long t; __asm__ volatile("mrs %0, cntpct_el0":"=r"(t)); if (t-t0>=d) break; }
}

int rp1usb_xhci_init(void)
{
    unsigned long base = g_sel_base ? g_sel_base : RP1_USB0;
    g_xhci_base = base;
    unsigned int caplen = xcaplen(base);
    g_oper = base + caplen;
    g_rt   = base + (R32(base, CAP_RTSOFF) & ~0x1Fu);
    g_db   = base + (R32(base, CAP_DBOFF)  & ~0x3u);
    g_ctx_stride = (R32(base, XHCI_HCCPARAMS1) & (1u<<2)) ? 64 : 32;   /* CSZ */

    /* 1. Stop, then reset the controller. */
    R32(g_oper, OP_USBCMD) &= ~USBCMD_RS;
    for (int i=0;i<100 && !(R32(g_oper,OP_USBSTS)&USBSTS_HCH);i++) xdelay(1000);
    R32(g_oper, OP_USBCMD) |= USBCMD_HCRST;
    for (int i=0;i<200 && (R32(g_oper,OP_USBCMD)&USBCMD_HCRST);i++) xdelay(1000);
    for (int i=0;i<200 && (R32(g_oper,OP_USBSTS)&USBSTS_CNR);i++)   xdelay(1000);

    /* 1b. DWC3 quirk: keep the USB2 PHY awake.  If GUSB2PHYCFG.SUSPHY (bit6) or
     * ENBLSLPM (bit8) is left set, the PHY auto-suspends while a periodic
     * (interrupt) endpoint sits NAKing between reports — so control transfers
     * work but the mouse/keyboard interrupt endpoint never delivers data.  Clear
     * them so periodic polling keeps running.  (Classic DWC3 bring-up fix.) */
    R32(base, DWC3_GUSB2PHYCFG0)  &= ~((1u<<6)|(1u<<8));
    R32(base, DWC3_GUSB3PIPECTL0) &= ~(1u<<17);
    __asm__ volatile ("dsb sy":::"memory");

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

    /* Persist this controller's registers/cursors so xhci_switch() can restore
     * them when we hop back here to drain its event ring. */
    { struct ctrlmem *c=&g_cm[g_active_ctrl];
      c->base=g_xhci_base; c->oper=g_oper; c->rt=g_rt; c->db=g_db; c->ctx_stride=g_ctx_stride;
      c->cmd_idx=g_cmd_idx; c->cmd_cycle=g_cmd_cycle; c->evt_idx=g_evt_idx; c->evt_cycle=g_evt_cycle;
      c->inited = g_xhci_running ? 1 : 0; }

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
    xdelay(30000);
    /* Re-read PORTSC until the port reports connected (CCS) AND a valid speed id
     * (bits 13:10 != 0).  The speed field isn't reliable the instant reset clears,
     * and a wrong speed makes Address Device / the interrupt EP silently fail. */
    g_enum_portsc = R32(psc, 0);
    for (int i=0;i<20;i++){
        unsigned int v = R32(psc, 0);
        if ((v & 1u) && ((v>>10)&0xf)) { g_enum_portsc = v; break; }
        g_enum_portsc = v;
        xdelay(3000);
    }

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

/* ---------- Phase 3c: Address Device ---------- */
static unsigned char g_input_ctx[33*64] __attribute__((aligned(64)));  /* ctrl+slot+up to 31 EPs */
static unsigned char g_dev_ctx[33*64]  __attribute__((aligned(64)));  /* output context (device 0) */
static unsigned char g_kdevctx[33*64]  __attribute__((aligned(64)));  /* output context (device 1 = kbd) */
static unsigned char *g_addr_ctx = g_dev_ctx;   /* which output ctx address/configure writes (per-slot) */
static struct trb    g_ep0_ring[16]    __attribute__((aligned(64)));
static int           g_ep0_idx, g_ep0_cycle = 1;

/* Second HID device (keyboard): its own interrupt-IN ring + report buffer, polled
 * alongside the mouse by rp1usb_mouse_pump().  EP0/command/event rings are shared
 * (binding is sequential), only the persistent output ctx + interrupt ring differ. */
static struct trb    g_ep2_ring[16]    __attribute__((aligned(64)));
static int           g_ep2_idx, g_ep2_cycle = 1;
static unsigned char g_kbd_buf[8]      __attribute__((aligned(64)));
static unsigned char g_kbd_prev[8];
static int           g_kbd_slot, g_kbd_dci, g_kbd_active;
static unsigned long g_kbd_reports;
static int           g_mouse_ctrl, g_kbd_ctrl;   /* which controller each is bound on */
static unsigned int  g_addr_cc;
static int           g_dev_slot, g_dev_speed;

static unsigned int *ctx_at(unsigned char *p, int idx){ return (unsigned int *)(p + idx*g_ctx_stride); }

int rp1usb_address_device(int slot, int port, int speed, int bsr)
{
    g_dev_slot = slot; g_dev_speed = speed;
    /* EP0 transfer ring + link TRB. */
    for (int i=0;i<16;i++){ g_ep0_ring[i].p0=0;g_ep0_ring[i].p1=0;g_ep0_ring[i].status=0;g_ep0_ring[i].control=0; }
    g_ep0_ring[15].p0 = (unsigned int)(XDA(g_ep0_ring)&0xffffffff);
    g_ep0_ring[15].p1 = (unsigned int)(XDA(g_ep0_ring)>>32);
    g_ep0_ring[15].control = (6u<<10)|(1u<<1)|1u;
    g_ep0_idx=0; g_ep0_cycle=1;

    unsigned char *dctx = g_addr_ctx ? g_addr_ctx : g_dev_ctx;   /* per-slot output ctx */
    for (unsigned i=0;i<sizeof g_input_ctx;i++) g_input_ctx[i]=0;
    for (unsigned i=0;i<33*64;i++)              dctx[i]=0;

    unsigned int *icc = ctx_at(g_input_ctx, 0);
    icc[1] = (1u<<0)|(1u<<1);                       /* Add slot + EP0 */
    unsigned int *sc = ctx_at(g_input_ctx, 1);
    sc[0] = (1u<<27) | ((unsigned)speed << 20);     /* ctx entries=1, speed */
    sc[1] = ((unsigned)port & 0xff) << 16;          /* root hub port */
    int mps = (speed==4)?512:(speed==3)?64:8;       /* SS/HS / FS/LS */
    unsigned int *ep0 = ctx_at(g_input_ctx, 2);
    ep0[1] = (4u<<3) | (3u<<1) | ((unsigned)mps<<16);  /* Control EP, CErr=3, MPS */
    unsigned long trd = XDA(g_ep0_ring) | 1u;       /* DCS=1 */
    ep0[2] = (unsigned int)(trd & 0xffffffff);
    ep0[3] = (unsigned int)(trd >> 32);
    ep0[4] = 8;

    g_dcbaa[slot] = XDA(dctx);
    __asm__ volatile ("dsb sy":::"memory");

    unsigned long ic = XDA(g_input_ctx);
    cmd_submit((unsigned)(ic&0xffffffff), (unsigned)(ic>>32), 0,
               (11u<<10) | (bsr?(1u<<9):0u) | ((unsigned)slot<<24));
    struct trb ev = event_wait(33);
    g_addr_cc = (ev.status>>24)&0xff;
    uart_puts("rp1usb: address-device cc=");
    uart_putc((char)('0'+g_addr_cc/10%10)); uart_putc((char)('0'+g_addr_cc%10)); uart_puts("\n");
    return (g_addr_cc==1)?0:-1;
}
unsigned int rp1usb_addr_cc(void){ return g_addr_cc; }
int          rp1usb_ctx_stride(void){ return g_ctx_stride; }

/* ---------- Phase 3d: EP0 control transfer (GET_DESCRIPTOR) ---------- */
static unsigned char g_xfer_buf[256] __attribute__((aligned(64)));
static unsigned int  g_desc_cc, g_desc_len;

static void ep0_push(unsigned int p0, unsigned int p1, unsigned int status, unsigned int control)
{
    struct trb *t = &g_ep0_ring[g_ep0_idx];
    t->p0=p0; t->p1=p1; t->status=status;
    t->control = (control & ~1u) | (g_ep0_cycle & 1u);
    __asm__ volatile ("dsb sy":::"memory");
    g_ep0_idx++;
    if (g_ep0_idx == 15) {                  /* link TRB slot: wrap */
        g_ep0_ring[15].control = (g_ep0_ring[15].control & ~1u) | (g_ep0_cycle & 1u);
        __asm__ volatile ("dsb sy":::"memory");
        g_ep0_idx = 0; g_ep0_cycle ^= 1;
    }
}

/* Standard device-to-host GET_DESCRIPTOR on EP0 of `slot`; data into g_xfer_buf. */
int rp1usb_get_descriptor(int slot, int dtype, int dindex, int len)
{
    if (len > (int)sizeof g_xfer_buf) len = sizeof g_xfer_buf;
    for (int i=0;i<len;i++) g_xfer_buf[i]=0;
    unsigned int wValue = ((unsigned)dtype<<8) | (unsigned)dindex;
    /* Setup stage (IDT immediate 8-byte setup packet, TRT=IN). */
    ep0_push(0x80u | (6u<<8) | (wValue<<16), ((unsigned)len<<16), 8, (2u<<10)|(1u<<6)|(3u<<16));
    /* Data stage IN. */
    unsigned long ba = XDA(g_xfer_buf);
    ep0_push((unsigned)(ba&0xffffffff), (unsigned)(ba>>32), (unsigned)len, (3u<<10)|(1u<<16));
    /* Status stage OUT + IOC. */
    ep0_push(0,0,0, (4u<<10)|(1u<<5));
    /* Ring the slot's EP0 doorbell (DCI 1). */
    R32(g_db, slot*4) = 1;
    __asm__ volatile ("dsb sy":::"memory");
    struct trb ev = event_wait(32);          /* Transfer Event */
    g_desc_cc  = (ev.status>>24)&0xff;
    g_desc_len = (unsigned)len - (ev.status & 0xffffff);   /* len - residual */
    uart_puts("rp1usb: get-descriptor cc=");
    uart_putc((char)('0'+g_desc_cc/10%10)); uart_putc((char)('0'+g_desc_cc%10)); uart_puts("\n");
    return (g_desc_cc==1)?(int)g_desc_len:-1;
}
unsigned int rp1usb_desc_cc(void)    { return g_desc_cc; }
unsigned int rp1usb_desc_len(void)   { return g_desc_len; }
unsigned int rp1usb_desc_byte(int i) { return (i>=0&&i<256)?g_xfer_buf[i]:0; }

/* HID class GET_REPORT(Input) on EP0 -> latest mouse report into g_mouse_buf.
 * Bypasses the (currently silent) interrupt endpoint entirely; control transfers
 * are proven working, so this reliably reads the device's current report. */
static unsigned int g_grep_cc, g_grep_len;
int rp1usb_get_report(int slot, int len)
{
    extern unsigned char *rp1usb_mouse_buf_ptr(void);
    unsigned char *mb = rp1usb_mouse_buf_ptr();
    if (len > 8) len = 8;
    for (int i=0;i<len;i++) mb[i]=0;
    /* Setup: bmReqType=0xA1, bReq=0x01(GET_REPORT), wValue=0x0100(Input,id0), wIndex=0 */
    ep0_push(0xA1u | (0x01u<<8) | (0x0100u<<16), ((unsigned)len<<16), 8, (2u<<10)|(1u<<6)|(3u<<16));
    unsigned long ba = XDA(mb);
    ep0_push((unsigned)(ba&0xffffffff), (unsigned)(ba>>32), (unsigned)len, (3u<<10)|(1u<<16));
    ep0_push(0,0,0, (4u<<10)|(1u<<5));
    R32(g_db, slot*4) = 1;
    __asm__ volatile ("dsb sy":::"memory");
    struct trb ev = event_wait(32);
    g_grep_cc  = (ev.status>>24)&0xff;
    g_grep_len = (unsigned)len - (ev.status & 0xffffff);
    return (g_grep_cc==1||g_grep_cc==13)?(int)g_grep_len:-(int)g_grep_cc;
}
unsigned int rp1usb_grep_cc(void)  { return g_grep_cc; }
unsigned int rp1usb_grep_len(void) { return g_grep_len; }

/* ---------- Phase 3e: SET_CONFIGURATION + Configure Endpoint + HID poll ---------- */
static struct trb     g_ep1_ring[16] __attribute__((aligned(64)));
static int            g_ep1_idx, g_ep1_cycle = 1;
static unsigned char  g_mouse_buf[8] __attribute__((aligned(64)));
static unsigned int   g_setcfg_cc, g_cfgep_cc, g_setproto_cc;
static int            g_mouse_slot, g_mouse_active, g_mouse_dci = 3;  /* defined below; fwd for hid_setup */
static unsigned long  g_mouse_reports;
static int            g_last_btn, g_last_dx, g_last_dy;
static void           ep1_queue_trb(void);
static void           hid_arm(struct trb *ring, int *idx, int *cyc, unsigned char *buf);

/* EP0 control transfer with NO data stage (SET_CONFIGURATION, SET_PROTOCOL). */
static int control_nodata(int slot, unsigned int bmReqType, unsigned int bReq,
                          unsigned int wValue, unsigned int wIndex)
{
    ep0_push(bmReqType | (bReq<<8) | (wValue<<16), wIndex, 8, (2u<<10)|(1u<<6)|(0u<<16));
    ep0_push(0,0,0, (4u<<10)|(1u<<16)|(1u<<5));    /* Status IN + IOC */
    R32(g_db, slot*4) = 1;                          /* EP0 = DCI 1 */
    __asm__ volatile ("dsb sy":::"memory");
    struct trb ev = event_wait(32);
    return ((ev.status>>24)&0xff) == 1 ? 0 : -1;
}

/* SET_CONFIGURATION(1) + Configure-Endpoint to add the interrupt-IN EP1 (DCI 3)
 * + SET_PROTOCOL(boot).  `mps` = the HID endpoint's wMaxPacketSize. */
int rp1usb_hid_setup(int slot, int port, int speed, int mps)
{
    g_setcfg_cc = (control_nodata(slot, 0x00, 9, 1, 0) == 0) ? 1 : 0;  /* SET_CONFIG 1 */

    /* EP1 IN transfer ring + link TRB. */
    for (int i=0;i<16;i++){ g_ep1_ring[i].p0=0;g_ep1_ring[i].p1=0;g_ep1_ring[i].status=0;g_ep1_ring[i].control=0; }
    g_ep1_ring[15].p0=(unsigned)(XDA(g_ep1_ring)&0xffffffff);
    g_ep1_ring[15].p1=(unsigned)(XDA(g_ep1_ring)>>32);
    g_ep1_ring[15].control=(6u<<10)|(1u<<1)|1u;
    g_ep1_idx=0; g_ep1_cycle=1;

    /* Input context: Add slot (A0) + EP1-IN/DCI3 (A3). */
    for (unsigned i=0;i<sizeof g_input_ctx;i++) g_input_ctx[i]=0;
    unsigned int *icc=ctx_at(g_input_ctx,0); icc[1]=(1u<<0)|(1u<<3);
    unsigned int *sc=ctx_at(g_input_ctx,1);
    sc[0]=(3u<<27)|((unsigned)speed<<20);          /* context entries = 3 */
    sc[1]=((unsigned)port&0xff)<<16;
    unsigned int *ep=ctx_at(g_input_ctx,4);        /* DCI 3 -> input index 4 */
    ep[0]=(7u<<16);                                 /* Interval ~ (2^7 * 125us = 16ms) */
    ep[1]=(7u<<3)|(3u<<1)|((unsigned)mps<<16);      /* Interrupt-IN, CErr=3, MPS */
    unsigned long trd=XDA(g_ep1_ring)|1u;
    ep[2]=(unsigned)(trd&0xffffffff); ep[3]=(unsigned)(trd>>32);
    /* ep[4]: [15:0]=Average TRB Length, [31:16]=Max ESIT Payload Lo.
     * Max ESIT Payload MUST be non-zero or the periodic scheduler allocates 0
     * bandwidth and never issues a transfer (interrupt EP -> no events).
     * For an interrupt EP with MaxBurst=0 it is just MaxPacketSize. */
    ep[4]=((unsigned)mps<<16)|((unsigned)mps);
    g_dcbaa[slot]=XDA(g_dev_ctx);
    __asm__ volatile ("dsb sy":::"memory");
    unsigned long ic=XDA(g_input_ctx);
    cmd_submit((unsigned)(ic&0xffffffff),(unsigned)(ic>>32),0,(12u<<10)|((unsigned)slot<<24)); /* Configure Endpoint */
    struct trb ev=event_wait(33);
    g_cfgep_cc=(ev.status>>24)&0xff;

    (void)control_nodata(slot, 0x21, 0x0A, 0, 0);                            /* SET_IDLE(0) = report on change */
    g_setproto_cc = (control_nodata(slot, 0x21, 0x0B, 0, 0) == 0) ? 1 : 0;  /* SET_PROTOCOL boot */

    /* Arm the continuous pump: queue a couple of interrupt-IN transfers and let
     * rp1usb_mouse_pump() (in genet_rx_tick) deliver reports to the cursor. */
    if (g_cfgep_cc == 1) {
        g_mouse_slot = slot; g_mouse_dci = 3;
        ep1_queue_trb(); ep1_queue_trb();
        R32(g_db, slot*4) = 3;
        __asm__ volatile ("dsb sy":::"memory");
        g_mouse_active = 1;
    }
    return (g_cfgep_cc==1)?0:-1;
}

/* ---------- Pi3-style auto-bind: walk the config descriptor, find the real
 * mouse interrupt-IN endpoint, configure IT (not a hardcoded EP), start pump.
 * Mirrors xinu-raz usbMouseBindDevice: pick the HID interface whose interrupt-IN
 * endpoint is most mouse-like (boot mouse proto 2 > generic report proto 0 >
 * keyboard proto 1) and use the descriptor's real epaddr / wMaxPacketSize /
 * bInterval / interface number. ---------- */
static int g_auto_epaddr, g_auto_mps, g_auto_interval, g_auto_iface, g_auto_dci, g_auto_found, g_auto_proto;
static int g_autostart_active;   /* 1 while boot auto-scan runs (defined here for autosetup) */
int rp1usb_hid_autosetup(int slot, int port, int speed){ extern int rp1usb_hid_autosetup_if(int,int,int,int); return rp1usb_hid_autosetup_if(slot,port,speed,-1); }
/* want_iface >=0 forces binding that interface's interrupt-IN EP (e.g. 0=keyboard,
 * 1=touchpad); -1 = auto-pick the most mouse-like. */
int rp1usb_hid_autosetup_if(int slot, int port, int speed, int want_iface)
{
    int got = rp1usb_get_descriptor(slot, 2, 0, 9);            /* config header first */
    if (got < 4 || g_xfer_buf[0] != 9 || g_xfer_buf[1] != 2)   /* must be a real CONFIG desc */
        return -1;
    int wtot = g_xfer_buf[2] | (g_xfer_buf[3]<<8);
    if (wtot > (int)sizeof g_xfer_buf) wtot = sizeof g_xfer_buf;
    if (rp1usb_get_descriptor(slot, 2, 0, wtot) < wtot) { /* keep going with what we have */ }

    int pos=0, cur_class=-1, cur_proto=-1, cur_iface=-1, best=-1;
    g_auto_found=0;
    while (pos+2 <= wtot) {
        int blen=g_xfer_buf[pos], btype=g_xfer_buf[pos+1];
        if (blen<2) break;
        if (btype==4) {                                        /* interface */
            cur_iface=g_xfer_buf[pos+2]; cur_class=g_xfer_buf[pos+5]; cur_proto=g_xfer_buf[pos+7];
        } else if (btype==5 && cur_class==3) {                 /* endpoint of a HID interface */
            int epaddr=g_xfer_buf[pos+2], attr=g_xfer_buf[pos+3];
            int mps=g_xfer_buf[pos+4]|(g_xfer_buf[pos+5]<<8), interval=g_xfer_buf[pos+6];
            if ((attr&3)==3 && (epaddr&0x80) &&                 /* interrupt IN ... */
                (want_iface<0 || cur_iface==want_iface)) {     /* ...on the wanted iface */
                /* boot mouse > boot keyboard > generic (proto 0 is often a
                 * multimedia/consumer interface on a keyboard — don't mistake it
                 * for a mouse and skip the real keyboard interface). */
                int score=(cur_proto==2)?3:(cur_proto==1)?2:1;
                if (score>best) {
                    best=score;
                    g_auto_epaddr=epaddr; g_auto_mps=mps; g_auto_interval=interval;
                    g_auto_iface=cur_iface; g_auto_proto=cur_proto;
                    g_auto_dci=(epaddr&0xf)*2+1; g_auto_found=1;
                }
            }
        }
        pos+=blen;
    }
    if (!g_auto_found) return -2;

    /* During boot auto-scan, never let a second device of a type we've already
     * bound overwrite it — a flaky mass-storage port (the boot stick) can mis-
     * enumerate and produce a phantom "mouse"/"keyboard" that would clobber the
     * real one.  Manual /usb paths (autostart flag clear) always allow re-bind. */
    if (g_autostart_active &&
        (((g_auto_proto==1) && g_kbd_active) || ((g_auto_proto!=1) && g_mouse_active)))
        return -4;

    g_setcfg_cc=(control_nodata(slot,0x00,9,1,0)==0)?1:0;      /* SET_CONFIGURATION(1) */

    /* Route to the mouse device (g_ep1_ring/g_mouse_*) or, for a boot keyboard
     * (HID protocol 1), the keyboard device (g_ep2_ring/g_kbd_*). */
    int is_kbd = (g_auto_proto == 1);
    struct trb *ring = is_kbd ? g_ep2_ring : g_ep1_ring;
    int *pidx = is_kbd ? &g_ep2_idx : &g_ep1_idx;
    int *pcyc = is_kbd ? &g_ep2_cycle : &g_ep1_cycle;
    unsigned char *buf = is_kbd ? g_kbd_buf : g_mouse_buf;
    unsigned char *dctx = g_addr_ctx ? g_addr_ctx : g_dev_ctx;

    int dci=g_auto_dci, in_idx=dci+1, mps=g_auto_mps;
    for (int i=0;i<16;i++){ ring[i].p0=0;ring[i].p1=0;ring[i].status=0;ring[i].control=0; }
    ring[15].p0=(unsigned)(XDA(ring)&0xffffffff);
    ring[15].p1=(unsigned)(XDA(ring)>>32);
    ring[15].control=(6u<<10)|(1u<<1)|1u;
    *pidx=0; *pcyc=1;

    for (unsigned i=0;i<sizeof g_input_ctx;i++) g_input_ctx[i]=0;
    unsigned int *icc=ctx_at(g_input_ctx,0); icc[1]=(1u<<0)|(1u<<dci);   /* add slot + this EP */
    unsigned int *sc=ctx_at(g_input_ctx,1);
    sc[0]=((unsigned)dci<<27)|((unsigned)speed<<20);          /* context entries = dci */
    sc[1]=((unsigned)port&0xff)<<16;
    unsigned int *ep=ctx_at(g_input_ctx,in_idx);
    unsigned iv=3; { int b=g_auto_interval; while (b>1 && iv<10){ b>>=1; iv++; } }  /* ~log2(bInterval)+3 */
    ep[0]=(iv<<16);
    ep[1]=(7u<<3)|(3u<<1)|((unsigned)mps<<16);                 /* Interrupt-IN, CErr=3, MPS */
    unsigned long trd=XDA(ring)|1u;
    ep[2]=(unsigned)(trd&0xffffffff); ep[3]=(unsigned)(trd>>32);
    ep[4]=((unsigned)mps<<16)|((unsigned)mps);                 /* Max ESIT Payload + Avg TRB Len */
    g_dcbaa[slot]=XDA(dctx);
    __asm__ volatile("dsb sy":::"memory");
    unsigned long ic=XDA(g_input_ctx);
    cmd_submit((unsigned)(ic&0xffffffff),(unsigned)(ic>>32),0,(12u<<10)|((unsigned)slot<<24)); /* Configure Endpoint */
    struct trb ev=event_wait(33);
    g_cfgep_cc=(ev.status>>24)&0xff;

    (void)control_nodata(slot,0x21,0x0A,0,g_auto_iface);        /* SET_IDLE(0) on the HID iface */
    g_setproto_cc=(control_nodata(slot,0x21,0x0B,(unsigned)(is_kbd?0:0),g_auto_iface)==0)?1:0;  /* SET_PROTOCOL boot */

    if (g_cfgep_cc==1){
        hid_arm(ring,pidx,pcyc,buf); hid_arm(ring,pidx,pcyc,buf);
        R32(g_db,slot*4)=(unsigned)dci;
        __asm__ volatile("dsb sy":::"memory");
        if (is_kbd){ g_kbd_slot=slot; g_kbd_dci=dci; g_kbd_ctrl=g_active_ctrl; g_kbd_active=1; }
        else       { g_mouse_slot=slot; g_mouse_dci=dci; g_mouse_ctrl=g_active_ctrl; g_mouse_active=1; }
    }
    return (g_cfgep_cc==1)?0:-1;
}
int rp1usb_auto_epaddr(void){ return g_auto_epaddr; }
int rp1usb_auto_mps(void)   { return g_auto_mps; }
int rp1usb_auto_iface(void) { return g_auto_iface; }
int rp1usb_auto_dci(void)   { return g_auto_dci; }
int rp1usb_auto_proto(void) { return g_auto_proto; }
int rp1usb_auto_interval(void){ return g_auto_interval; }

/* One clean call: reset+enable-slot on `port`, auto-detect link speed from
 * PORTSC, Address Device, then descriptor auto-bind + start the pump.  Avoids
 * the manual slot/speed juggling that corrupted earlier c0p2 attempts. */
static int g_full_slot, g_full_speed;
int rp1usb_mouse_fullsetup(int port)
{
    extern int rp1usb_address_device(int,int,int,int);
    int slot = rp1usb_enum_slot(port);
    if (slot < 0) return -10;
    int speed = (int)((g_enum_portsc >> 10) & 0xf);     /* xHCI PORTSC speed id */
    g_full_slot = slot; g_full_speed = speed;
    if (rp1usb_address_device(slot, port, speed, 0) != 0) return -20;
    return rp1usb_hid_autosetup(slot, port, speed);
}
int rp1usb_full_slot(void) { return g_full_slot; }
int rp1usb_full_speed(void){ return g_full_speed; }

/* One-shot boot auto-bind: bring up BOTH RP1 controllers and scan every port for
 * HID devices, binding the mouse and keyboard wherever they are (they can be on
 * different controllers).  Called once from genet_rx_tick a few seconds into boot.
 * Returns the number of HID devices bound. */
static unsigned char *g_bind_ctxs[2];   /* output ctx per bound device */
static int            g_nbound;
static void autostart_scan_current_ctrl(void)
{
    extern int rp1usb_address_device(int,int,int,int);
    /* One output ctx per CONTROLLER (mouse on usb1 -> g_dev_ctx, kbd on usb0 ->
     * g_kdevctx) so a single-controller re-bind always reuses the same ctx and
     * can't collide with the other controller's already-bound device. */
    unsigned char *ctx = (g_active_ctrl == 1) ? g_dev_ctx : g_kdevctx;
    for (int p = 1; p <= 3; p++) {
        int slot = rp1usb_enum_slot(p);
        if (slot < 0) continue;
        int speed = (int)((g_enum_portsc >> 10) & 0xf);
        g_addr_ctx = ctx;
        if (rp1usb_address_device(slot, p, speed, 0) != 0) continue;
        /* autosetup walks the descriptor and routes mouse vs keyboard by proto. */
        if (rp1usb_hid_autosetup_if(slot, p, speed, -1) == 0) { g_nbound++; break; }
    }
}

/* Re-enumerate + re-bind only ONE controller's HID device, leaving the other
 * controller (and a device already delivering there) completely untouched. */
int rp1usb_rebind_ctrl(int ctrl)
{
    ctrl = (ctrl == 1) ? 1 : 0;
    g_autostart_active = 1;
    if (g_mouse_active && g_mouse_ctrl == ctrl) g_mouse_active = 0;
    if (g_kbd_active   && g_kbd_ctrl   == ctrl) g_kbd_active = 0;
    rp1usb_select_ctrl(ctrl);
    if (!g_cm[ctrl].inited) rp1usb_xhci_init();
    if (g_cm[ctrl].inited) autostart_scan_current_ctrl();
    g_addr_ctx = g_dev_ctx;
    g_autostart_active = 0;
    return 0;
}
int rp1usb_autostart(void)
{
    g_bind_ctxs[0] = g_dev_ctx; g_bind_ctxs[1] = g_kdevctx;
    g_nbound = 0;
    g_mouse_active = 0; g_kbd_active = 0;   /* clear so this run re-binds fresh */
    g_autostart_active = 1;
    /* IMPORTANT: HCRST-resetting a controller that already has a working device
     * tends to leave the OTHER controller's device not delivering.  So init each
     * controller AT MOST ONCE; on every (re-)run just re-enumerate + re-bind the
     * ports (port reset + Address + Configure-Endpoint), which is what reliably
     * gets a device delivering again. */
    rp1usb_select_ctrl(1);
    if (!g_cm[1].inited) rp1usb_xhci_init();
    if (g_cm[1].inited) autostart_scan_current_ctrl();
    rp1usb_select_ctrl(0);
    if (!g_cm[0].inited) rp1usb_xhci_init();
    if (g_cm[0].inited) autostart_scan_current_ctrl();
    g_addr_ctx = g_dev_ctx;                              /* restore default for manual paths */
    g_autostart_active = 0;
    return g_nbound;
}

/* periodic-schedule clock: if MFINDEX advances, the controller IS running the
 * (micro)frame timer that drives interrupt endpoints. */
unsigned int rp1usb_mfindex(void){ return R32(g_rt, 0) & 0x3fff; }
/* live EP state of whatever DCI the mouse is bound to (vs the old hardcoded 3). */
unsigned int rp1usb_boundep_state(void){ return ctx_at(g_dev_ctx, g_mouse_dci)[0] & 0x7; }
unsigned int rp1usb_boundep_deqlo(void){ return ctx_at(g_dev_ctx, g_mouse_dci)[2]; }
int          rp1usb_bound_dci(void)    { return g_mouse_dci; }

/* Queue one interrupt-IN transfer and wait for the boot-mouse report.  Blocks
 * (deadline-bounded) until the mouse sends data, so move the mouse to get one.
 * Returns the report length, or <=0 on error/timeout. */
int rp1usb_poll_mouse(int slot)
{
    /* Do NOT zero g_mouse_buf: the controller DMAs each completed report here,
     * so the buffer always holds the latest report even if event_wait returns
     * an earlier TRB's completion event (we share one buffer across TRBs). */
    struct trb *t=&g_ep1_ring[g_ep1_idx];
    unsigned long ba=XDA(g_mouse_buf);
    t->p0=(unsigned)(ba&0xffffffff); t->p1=(unsigned)(ba>>32);
    t->status=8;
    t->control=(1u<<10)|(1u<<5)|(g_ep1_cycle&1u);  /* Normal TRB + IOC */
    __asm__ volatile ("dsb sy":::"memory");
    g_ep1_idx++;
    if (g_ep1_idx==15){ g_ep1_ring[15].control=(g_ep1_ring[15].control&~1u)|(g_ep1_cycle&1u); __asm__ volatile("dsb sy":::"memory"); g_ep1_idx=0; g_ep1_cycle^=1; }
    R32(g_db, slot*4)=3;                            /* EP1 IN = DCI 3 */
    __asm__ volatile ("dsb sy":::"memory");
    struct trb ev=event_wait(32);
    unsigned cc=(ev.status>>24)&0xff;
    unsigned residual=ev.status&0xffffff;
    return (cc==1||cc==13)?(int)(8-residual):-(int)cc;   /* 13 = short packet (fine) */
}
unsigned int rp1usb_setcfg_cc(void) { return g_setcfg_cc; }
unsigned int rp1usb_cfgep_cc(void)  { return g_cfgep_cc; }
unsigned int rp1usb_setproto_cc(void){ return g_setproto_cc; }
unsigned int rp1usb_mouse_byte(int i){ return (i>=0&&i<8)?g_mouse_buf[i]:0; }
unsigned char *rp1usb_mouse_buf_ptr(void){ return g_mouse_buf; }

/* ---------- continuous (non-blocking) mouse pump -> xhci_mouse_event ---------- */

/* Generic: arm one interrupt-IN transfer into `buf` on `ring`. */
static void hid_arm(struct trb *ring, int *idx, int *cyc, unsigned char *buf)
{
    struct trb *t=&ring[*idx];
    unsigned long ba=XDA(buf);
    t->p0=(unsigned)(ba&0xffffffff); t->p1=(unsigned)(ba>>32);
    t->status=8;
    t->control=(1u<<10)|(1u<<5)|((*cyc)&1u);
    __asm__ volatile ("dsb sy":::"memory");
    (*idx)++;
    if (*idx==15){ ring[15].control=(ring[15].control&~1u)|((*cyc)&1u); __asm__ volatile("dsb sy":::"memory"); *idx=0; *cyc^=1; }
}

/* HID boot-keyboard report -> ASCII, emitting only newly-pressed keys. */
static const char g_km[64] = {
    [0x04]='a',[0x05]='b',[0x06]='c',[0x07]='d',[0x08]='e',[0x09]='f',[0x0a]='g',[0x0b]='h',
    [0x0c]='i',[0x0d]='j',[0x0e]='k',[0x0f]='l',[0x10]='m',[0x11]='n',[0x12]='o',[0x13]='p',
    [0x14]='q',[0x15]='r',[0x16]='s',[0x17]='t',[0x18]='u',[0x19]='v',[0x1a]='w',[0x1b]='x',
    [0x1c]='y',[0x1d]='z',[0x1e]='1',[0x1f]='2',[0x20]='3',[0x21]='4',[0x22]='5',[0x23]='6',
    [0x24]='7',[0x25]='8',[0x26]='9',[0x27]='0',[0x28]='\n',[0x2a]='\b',[0x2b]='\t',[0x2c]=' ',
    [0x2d]='-',[0x2e]='=',[0x2f]='[',[0x30]=']',[0x31]='\\',[0x33]=';',[0x34]='\'',[0x35]='`',
    [0x36]=',',[0x37]='.',[0x38]='/',
};
static const char g_kms[64] = {
    [0x04]='A',[0x05]='B',[0x06]='C',[0x07]='D',[0x08]='E',[0x09]='F',[0x0a]='G',[0x0b]='H',
    [0x0c]='I',[0x0d]='J',[0x0e]='K',[0x0f]='L',[0x10]='M',[0x11]='N',[0x12]='O',[0x13]='P',
    [0x14]='Q',[0x15]='R',[0x16]='S',[0x17]='T',[0x18]='U',[0x19]='V',[0x1a]='W',[0x1b]='X',
    [0x1c]='Y',[0x1d]='Z',[0x1e]='!',[0x1f]='@',[0x20]='#',[0x21]='$',[0x22]='%',[0x23]='^',
    [0x24]='&',[0x25]='*',[0x26]='(',[0x27]=')',[0x28]='\n',[0x2a]='\b',[0x2b]='\t',[0x2c]=' ',
    [0x2d]='_',[0x2e]='+',[0x2f]='{',[0x30]='}',[0x31]='|',[0x33]=':',[0x34]='"',[0x35]='~',
    [0x36]='<',[0x37]='>',[0x38]='?',
};
static char kbd_ascii(unsigned u, int shift)
{
    if (u >= 0x40) return 0;
    return shift ? g_kms[u] : g_km[u];
}

/* Auto-repeat state: the boot keyboard (SET_IDLE 0) sends nothing while a key is
 * held, so we synthesise repeats from a timer in kbd_repeat_tick(). */
static int           g_kbd_held;     /* usage being held (0 = none) */
static int           g_kbd_held_sh;
static unsigned long g_kbd_rep_at;   /* next repeat time (timer ticks) */
static int           g_kbd_rep_n;    /* repeats emitted (cap, in case of a stuck key) */

/* Apply the Ctrl modifier: Ctrl + A..Z (either case) -> control code 1..26
 * (so Ctrl-C == 0x03, which the BASIC window uses to break a running program).
 * Non-letters are passed through unchanged. */
static char kbd_apply_ctrl(char c)
{
    char up = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    if (up >= 'A' && up <= 'Z') return (char)(up - 'A' + 1);
    return c;
}

static void kbd_decode(unsigned char *r)
{
    extern void xhci_keyboard_event(char);
    extern unsigned long timer_ticks(void);
    int shift = (r[0] & 0x22) != 0;                /* L/R Shift in modifier byte */
    int ctrl  = (r[0] & 0x11) != 0;                /* L/R Ctrl  in modifier byte */
    for (int i=2;i<8;i++) {
        unsigned u=r[i];
        if (u==0 || u>=0x40) continue;
        int held=0;                                /* already down in the previous report? */
        for (int j=2;j<8;j++) if (g_kbd_prev[j]==u){ held=1; break; }
        if (held) continue;
        char c = kbd_ascii(u, shift);
        if (c) { if (ctrl) c = kbd_apply_ctrl(c); xhci_keyboard_event(c); }
    }
    /* Track the last typable key still down, for auto-repeat. */
    int cur = 0;
    for (int i=2;i<8;i++) if (r[i] && kbd_ascii(r[i], shift)) cur = r[i];
    if (cur && cur != g_kbd_held) {                /* new key held -> start the delay */
        g_kbd_held = cur; g_kbd_held_sh = shift;
        g_kbd_rep_at = timer_ticks() + 45;         /* ~450 ms initial delay */
        g_kbd_rep_n = 0;
    } else if (!cur) {
        g_kbd_held = 0;                            /* released */
    }
    for (int i=0;i<8;i++) g_kbd_prev[i]=r[i];
}

/* Called every pump tick: while a key is held past the initial delay, re-emit it
 * at the repeat rate (capped so a halted keyboard can't repeat forever). */
static void kbd_repeat_tick(void)
{
    extern void xhci_keyboard_event(char);
    extern unsigned long timer_ticks(void);
    if (!g_kbd_held || g_kbd_rep_n > 300) return;
    unsigned long now = timer_ticks();
    if (now >= g_kbd_rep_at) {
        char c = kbd_ascii(g_kbd_held, g_kbd_held_sh);
        if (c) xhci_keyboard_event(c);
        g_kbd_rep_at = now + 4;                    /* ~40 ms repeat interval (25/s) */
        g_kbd_rep_n++;
    }
}

static void ep1_queue_trb(void)            /* arm one interrupt-IN transfer */
{
    struct trb *t=&g_ep1_ring[g_ep1_idx];
    unsigned long ba=XDA(g_mouse_buf);
    t->p0=(unsigned)(ba&0xffffffff); t->p1=(unsigned)(ba>>32);
    t->status=8;
    t->control=(1u<<10)|(1u<<5)|(g_ep1_cycle&1u);
    __asm__ volatile ("dsb sy":::"memory");
    g_ep1_idx++;
    if (g_ep1_idx==15){ g_ep1_ring[15].control=(g_ep1_ring[15].control&~1u)|(g_ep1_cycle&1u); __asm__ volatile("dsb sy":::"memory"); g_ep1_idx=0; g_ep1_cycle^=1; }
}

extern void xhci_mouse_event(unsigned nButtons, int dx, int dy);

/* Called every genet_rx_tick (100 Hz).  Non-blocking: drains any pending xHCI
 * events; for each EP1 transfer event, hands the boot-mouse report to
 * xhci_mouse_event (moves the HDMI cursor) and re-arms a transfer. */
static int g_poll_mode;     /* 1 = poll mouse via EP0 GET_REPORT instead of EP1 IRQ */
void rp1usb_set_poll_mode(int on){ g_poll_mode = on?1:0; }
int  rp1usb_poll_mode_get(void) { return g_poll_mode; }

void rp1usb_mouse_pump(void)
{
    if (!g_mouse_active && !g_kbd_active) return;
    if (g_kbd_active) kbd_repeat_tick();        /* auto-repeat held keys */
    if (g_mouse_active && g_poll_mode) {       /* control-pipe polling path */
        extern int rp1usb_get_report(int,int);
        xhci_switch(g_mouse_ctrl);
        int n = rp1usb_get_report(g_mouse_slot, 4);
        if (n >= 3) {
            unsigned btn = g_mouse_buf[0];
            int dx = (int)(signed char)g_mouse_buf[1];
            int dy = (int)(signed char)g_mouse_buf[2];
            g_mouse_reports++;
            g_last_btn=btn; g_last_dx=dx; g_last_dy=dy;
            if (dx || dy || btn) xhci_mouse_event(btn, dx, dy);
        }
        return;
    }
    /* Drain each controller that hosts a bound HID device (mouse and keyboard
     * may live on different RP1 controllers, each with its own event ring). */
    for (int ci=0; ci<2; ci++) {
        if (!g_cm[ci].inited) continue;
        if (!((g_mouse_active && g_mouse_ctrl==ci) || (g_kbd_active && g_kbd_ctrl==ci))) continue;
        xhci_switch(ci);
        for (int guard=0; guard<32; guard++) {
            struct trb *e=&g_evt_ring[g_evt_idx];
            __asm__ volatile ("dsb sy":::"memory");
            if ((e->control & 1u) != (unsigned)g_evt_cycle) break;   /* no more events */
            struct trb ev=*e;
            g_evt_idx++;
            if (g_evt_idx==EVT_RING_N){ g_evt_idx=0; g_evt_cycle^=1; }
            unsigned long erdp=XDA(&g_evt_ring[g_evt_idx]);
            R32(g_rt, IR0_ERDP)   = (unsigned)((erdp&0xffffffff)|(1u<<3));
            R32(g_rt, IR0_ERDP+4) = (unsigned)(erdp>>32);
            if (((ev.control>>10)&0x3f) != 32) continue;             /* Transfer Event only */
            int eslot = (int)((ev.control>>24)&0xff);
            int edci  = (int)((ev.control>>16)&0x1f);
            if (g_mouse_active && g_mouse_ctrl==ci && eslot==g_mouse_slot && edci==g_mouse_dci) {
                unsigned btn = g_mouse_buf[0];
                int dx = (int)(signed char)g_mouse_buf[1];
                int dy = (int)(signed char)g_mouse_buf[2];
                g_mouse_reports++;
                g_last_btn=btn; g_last_dx=dx; g_last_dy=dy;
                if (dx || dy || btn) xhci_mouse_event(btn, dx, dy);
                ep1_queue_trb();
                R32(g_db, g_mouse_slot*4) = (unsigned)g_mouse_dci;   /* re-ring mouse EP doorbell */
                __asm__ volatile ("dsb sy":::"memory");
            } else if (g_kbd_active && g_kbd_ctrl==ci && eslot==g_kbd_slot && edci==g_kbd_dci) {
                g_kbd_reports++;
                kbd_decode(g_kbd_buf);
                hid_arm(g_ep2_ring, &g_ep2_idx, &g_ep2_cycle, g_kbd_buf);
                R32(g_db, g_kbd_slot*4) = (unsigned)g_kbd_dci;       /* re-ring kbd EP doorbell */
                __asm__ volatile ("dsb sy":::"memory");
            }
        }
    }
}
unsigned long rp1usb_kbd_reports(void){ return g_kbd_reports; }
int           rp1usb_kbd_on(void)     { return g_kbd_active; }
int rp1usb_mouse_ctrl_get(void){ return g_mouse_ctrl; }
int rp1usb_kbd_ctrl_get(void)  { return g_kbd_ctrl; }
int rp1usb_mouse_slot_get(void){ return g_mouse_slot; }
int rp1usb_mouse_dci_get(void) { return g_mouse_dci; }
int rp1usb_kbd_slot_get(void)  { return g_kbd_slot; }
int rp1usb_kbd_dci_get(void)   { return g_kbd_dci; }
unsigned long rp1usb_mouse_reports(void){ return g_mouse_reports; }
int           rp1usb_mouse_on(void)     { return g_mouse_active; }
int           rp1usb_last_btn(void)     { return g_last_btn; }
int           rp1usb_last_dx(void)      { return g_last_dx; }
int           rp1usb_last_dy(void)      { return g_last_dy; }
/* live snapshot of the ring cursors so we can see whether events arrive */
int           rp1usb_evt_idx(void)      { return g_evt_idx; }
int           rp1usb_ep1_idx_get(void)  { return g_ep1_idx; }
/* OUTPUT device-context introspection: did the controller actually run EP1? */
unsigned int  rp1usb_slot_state(void)   { return (ctx_at(g_dev_ctx,0)[3]>>27)&0x1f; }
unsigned int  rp1usb_ep1_state(void)    { return ctx_at(g_dev_ctx,3)[0]&0x7; }
unsigned int  rp1usb_ep1_deq_lo(void)   { return ctx_at(g_dev_ctx,3)[2]; }     /* TR dequeue ptr lo */
unsigned int  rp1usb_ep1_ctx0(void)     { return ctx_at(g_dev_ctx,3)[0]; }

/* Read-only register offsets (for diagnosing the xHCI init alignment fault). */
unsigned int rp1usb_rtsoff(void){ return R32(RP1_USB0, CAP_RTSOFF); }
unsigned int rp1usb_dboff(void) { return R32(RP1_USB0, CAP_DBOFF);  }
unsigned int rp1usb_hccp1(void) { return R32(RP1_USB0, XHCI_HCCPARAMS1); }

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

/* ====================================================================
 * Phase 4 — USB Mass Storage (Bulk-Only Transport + SCSI) on the boot
 * stick (c0p2, USB2 HS).  Reuses the xHCI primitives above: SET_CONFIG via
 * control_nodata(), Configure-Endpoint via cmd_submit(), and Normal-TRB bulk
 * transfers via bulk_xfer().  Drives the standard BOT handshake
 * (CBW -> data -> CSW) and the SCSI commands needed for block r/w.  All HTTP-
 * driven (no boot path) so a bug here can't fault the boot.
 * ==================================================================== */

static struct trb     g_bo_ring[16] __attribute__((aligned(64)));   /* bulk OUT */
static struct trb     g_bi_ring[16] __attribute__((aligned(64)));   /* bulk IN  */
static int            g_bo_idx, g_bo_cyc = 1, g_bi_idx, g_bi_cyc = 1;
static unsigned char  g_msd_devctx[33*64] __attribute__((aligned(64)));  /* MSD output ctx */
static unsigned char  g_cbw[64] __attribute__((aligned(64)));
static unsigned char  g_csw[64] __attribute__((aligned(64)));
static unsigned char  g_msd_data[4096] __attribute__((aligned(64)));
static int            g_msd_slot, g_msd_iface, g_msd_out_ep, g_msd_in_ep, g_msd_out_dci, g_msd_in_dci;
static unsigned int   g_msd_setup_cc, g_msd_cfgep_cc;
static unsigned int   g_msd_blocks, g_msd_blocksize;
static unsigned int   g_msd_tag = 1, g_msd_csw_status, g_msd_residue;
static int            g_msd_last_r;
static int            g_p_cbw, g_p_data, g_p_csw;          /* per-phase byte counts */
static unsigned       g_p_cbw_cc, g_p_data_cc, g_p_csw_cc; /* per-phase completion codes */

static void msd_le32(unsigned char *p, unsigned int v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

static void bulk_ring_init(struct trb *ring, int *idx, int *cyc)
{
    for (int i=0;i<16;i++){ ring[i].p0=ring[i].p1=ring[i].status=ring[i].control=0; }
    ring[15].p0 = (unsigned)(XDA(ring)&0xffffffff);
    ring[15].p1 = (unsigned)(XDA(ring)>>32);
    ring[15].control = (6u<<10)|(1u<<1)|1u;        /* Link TRB, TC=1, cycle */
    *idx = 0; *cyc = 1;
}

/* Queue one Normal TRB on a bulk ring, ring the slot's EP doorbell, wait for the
 * transfer event.  Returns bytes transferred (len - residual) or a negative
 * error: -1000 = no event (timeout), else -(completion code). */
static unsigned g_bulk_cc;        /* completion code of the last bulk_xfer (0xff=timeout) */
static int bulk_xfer(struct trb *ring, int *idx, int *cyc, int slot, int dci,
                     void *buf, int len, int is_in)
{
    struct trb *t = &ring[*idx];
    unsigned long ba = XDA(buf);
    t->p0 = (unsigned)(ba&0xffffffff); t->p1 = (unsigned)(ba>>32);
    t->status = (unsigned)len & 0x1ffff;                       /* TRB transfer length */
    t->control = (1u<<10)|(1u<<5)|(is_in?(1u<<2):0u)|((*cyc)&1u); /* Normal, IOC, ISP(in) */
    __asm__ volatile ("dsb sy":::"memory");
    (*idx)++;
    if (*idx==15){ ring[15].control=(ring[15].control&~1u)|((*cyc)&1u);
                   __asm__ volatile("dsb sy":::"memory"); *idx=0; *cyc^=1; }
    R32(g_db, slot*4) = (unsigned)dci;
    __asm__ volatile ("dsb sy":::"memory");
    struct trb ev = event_wait(32);
    if (ev.control == 0) { g_bulk_cc = 0xff; return -1000; }    /* no event = timeout */
    unsigned cc = (ev.status>>24)&0xff;
    unsigned residual = ev.status & 0xffffff;
    g_bulk_cc = cc;
    if (cc==1 || cc==13) return len - (int)residual;           /* 13 = short packet, ok */
    return -(int)cc;
}

/* SET_CONFIGURATION(1) + locate the class-8 (mass-storage) interface's bulk IN/OUT
 * endpoints + Configure-Endpoint them.  Output ctx must already be g_msd_devctx
 * (address it via rp1usb_msd_fullsetup). */
int rp1usb_msd_setup(int slot, int port, int speed)
{
    g_msd_slot = slot;
    g_msd_setup_cc = (control_nodata(slot, 0x00, 9, 1, 0) == 0) ? 1 : 0;  /* SET_CONFIG 1 */

    int got = rp1usb_get_descriptor(slot, 2, 0, 9);            /* config header */
    if (got < 4 || g_xfer_buf[0]!=9 || g_xfer_buf[1]!=2) return -1;
    int wtot = g_xfer_buf[2] | (g_xfer_buf[3]<<8);
    if (wtot > (int)sizeof g_xfer_buf) wtot = sizeof g_xfer_buf;
    rp1usb_get_descriptor(slot, 2, 0, wtot);                   /* full config */

    int pos=0, cur_class=-1, cur_iface=-1, found=0;
    int in_ep=0, out_ep=0, in_mps=512, out_mps=512;
    while (pos+2 <= wtot) {
        int blen=g_xfer_buf[pos], btype=g_xfer_buf[pos+1];
        if (blen<2) break;
        if (btype==4) {                                        /* interface */
            cur_iface=g_xfer_buf[pos+2]; cur_class=g_xfer_buf[pos+5];
        } else if (btype==5 && cur_class==8) {                 /* bulk EP of MSD iface */
            int epaddr=g_xfer_buf[pos+2], attr=g_xfer_buf[pos+3];
            int mps=g_xfer_buf[pos+4]|(g_xfer_buf[pos+5]<<8);
            if ((attr&3)==2) {                                 /* bulk */
                if (epaddr&0x80){ in_ep=epaddr; in_mps=mps; } else { out_ep=epaddr; out_mps=mps; }
                g_msd_iface=cur_iface; found=1;
            }
        }
        pos += blen;
    }
    if (!found || !in_ep || !out_ep) return -2;

    int out_dci=(out_ep&0xf)*2, in_dci=(in_ep&0xf)*2+1;
    int maxdci = out_dci>in_dci ? out_dci : in_dci;
    g_msd_out_ep=out_ep; g_msd_in_ep=in_ep; g_msd_out_dci=out_dci; g_msd_in_dci=in_dci;

    bulk_ring_init(g_bo_ring,&g_bo_idx,&g_bo_cyc);
    bulk_ring_init(g_bi_ring,&g_bi_idx,&g_bi_cyc);

    for (unsigned i=0;i<sizeof g_input_ctx;i++) g_input_ctx[i]=0;
    unsigned int *icc=ctx_at(g_input_ctx,0);
    icc[1]=(1u<<0)|(1u<<out_dci)|(1u<<in_dci);                 /* add slot + both bulk EPs */
    unsigned int *sc=ctx_at(g_input_ctx,1);
    sc[0]=((unsigned)maxdci<<27)|((unsigned)speed<<20);
    sc[1]=((unsigned)port&0xff)<<16;
    { unsigned int *ep=ctx_at(g_input_ctx,out_dci+1);          /* bulk OUT */
      ep[1]=(2u<<3)|(3u<<1)|((unsigned)out_mps<<16);
      unsigned long trd=XDA(g_bo_ring)|1u;
      ep[2]=(unsigned)(trd&0xffffffff); ep[3]=(unsigned)(trd>>32);
      ep[4]=(unsigned)out_mps; }
    { unsigned int *ep=ctx_at(g_input_ctx,in_dci+1);           /* bulk IN */
      ep[1]=(6u<<3)|(3u<<1)|((unsigned)in_mps<<16);
      unsigned long trd=XDA(g_bi_ring)|1u;
      ep[2]=(unsigned)(trd&0xffffffff); ep[3]=(unsigned)(trd>>32);
      ep[4]=(unsigned)in_mps; }
    g_dcbaa[slot]=XDA(g_msd_devctx);
    __asm__ volatile("dsb sy":::"memory");
    unsigned long ic=XDA(g_input_ctx);
    cmd_submit((unsigned)(ic&0xffffffff),(unsigned)(ic>>32),0,(12u<<10)|((unsigned)slot<<24));
    struct trb ev=event_wait(33);
    g_msd_cfgep_cc=(ev.status>>24)&0xff;
    return (g_msd_cfgep_cc==1)?0:-3;
}

/* One BOT transaction: CBW -> optional data -> CSW.  dir: 0 none, 1 IN, 2 OUT.
 * Returns 0 on CSW status==0 (good), 1 on CSW fail, negative on transport error. */
static int msd_scsi(const unsigned char *cdb, int cdblen, int dir, void *data, int datalen)
{
    unsigned int tag = g_msd_tag++;
    for (int i=0;i<31;i++) g_cbw[i]=0;
    msd_le32(g_cbw+0, 0x43425355u);             /* dCBWSignature "USBC" */
    msd_le32(g_cbw+4, tag);
    msd_le32(g_cbw+8, (unsigned)datalen);
    g_cbw[12] = (dir==1) ? 0x80 : 0x00;         /* bmCBWFlags: IN=0x80 */
    g_cbw[13] = 0;                              /* bCBWLUN = 0 */
    g_cbw[14] = (unsigned char)cdblen;
    for (int i=0;i<cdblen && i<16;i++) g_cbw[15+i]=cdb[i];

    g_p_cbw = bulk_xfer(g_bo_ring,&g_bo_idx,&g_bo_cyc,g_msd_slot,g_msd_out_dci,g_cbw,31,0);
    g_p_cbw_cc = g_bulk_cc;
    if (g_p_cbw < 0) return -1;
    g_p_data = 0; g_p_data_cc = 0;
    if (dir==1 && datalen>0) {
        g_p_data = bulk_xfer(g_bi_ring,&g_bi_idx,&g_bi_cyc,g_msd_slot,g_msd_in_dci,data,datalen,1);
        g_p_data_cc = g_bulk_cc;
        if (g_p_data < 0) return -2;
    } else if (dir==2 && datalen>0) {
        g_p_data = bulk_xfer(g_bo_ring,&g_bo_idx,&g_bo_cyc,g_msd_slot,g_msd_out_dci,data,datalen,0);
        g_p_data_cc = g_bulk_cc;
        if (g_p_data < 0) return -3;
    }
    for (int i=0;i<13;i++) g_csw[i]=0;
    g_p_csw = bulk_xfer(g_bi_ring,&g_bi_idx,&g_bi_cyc,g_msd_slot,g_msd_in_dci,g_csw,13,1);
    g_p_csw_cc = g_bulk_cc;
    if (g_p_csw < 0) return -4;
    g_msd_csw_status = g_csw[12];
    g_msd_residue = g_csw[8]|(g_csw[9]<<8)|(g_csw[10]<<16)|(g_csw[11]<<24);
    return (g_msd_csw_status==0) ? 0 : 1;
}

int rp1usb_msd_inquiry(void)
{
    unsigned char cdb[6]={0x12,0,0,0,36,0};
    return (g_msd_last_r = msd_scsi(cdb,6,1,g_msd_data,36));
}
int rp1usb_msd_test_ready(void)
{
    unsigned char cdb[6]={0,0,0,0,0,0};
    return (g_msd_last_r = msd_scsi(cdb,6,0,0,0));
}
int rp1usb_msd_request_sense(void)
{
    unsigned char cdb[6]={0x03,0,0,0,18,0};
    return (g_msd_last_r = msd_scsi(cdb,6,1,g_msd_data,18));
}
int rp1usb_msd_capacity(void)
{
    unsigned char cdb[10]={0x25,0,0,0,0,0,0,0,0,0};
    /* A freshly-configured device often answers the first TEST UNIT READY with a
     * "unit attention"; clear it with REQUEST SENSE then retry a few times. */
    for (int i=0;i<4;i++){ if (rp1usb_msd_test_ready()==0) break; rp1usb_msd_request_sense(); }
    int r = msd_scsi(cdb,10,1,g_msd_data,8);
    if (r==0) {
        g_msd_blocks    = ((unsigned)g_msd_data[0]<<24)|((unsigned)g_msd_data[1]<<16)|
                          ((unsigned)g_msd_data[2]<<8)|g_msd_data[3];
        g_msd_blocks   += 1;                    /* READ CAPACITY returns last LBA */
        g_msd_blocksize = ((unsigned)g_msd_data[4]<<24)|((unsigned)g_msd_data[5]<<16)|
                          ((unsigned)g_msd_data[6]<<8)|g_msd_data[7];
    }
    return (g_msd_last_r = r);
}
int rp1usb_msd_read_block(unsigned int lba)
{
    unsigned int bs = g_msd_blocksize ? g_msd_blocksize : 512;
    unsigned char cdb[10]={0x28,0,(unsigned char)(lba>>24),(unsigned char)(lba>>16),
                           (unsigned char)(lba>>8),(unsigned char)lba,0,0,1,0};
    return (g_msd_last_r = msd_scsi(cdb,10,1,g_msd_data,(int)bs));
}
int rp1usb_msd_write_block(unsigned int lba)
{
    unsigned int bs = g_msd_blocksize ? g_msd_blocksize : 512;
    unsigned char cdb[10]={0x2A,0,(unsigned char)(lba>>24),(unsigned char)(lba>>16),
                           (unsigned char)(lba>>8),(unsigned char)lba,0,0,1,0};
    return (g_msd_last_r = msd_scsi(cdb,10,2,g_msd_data,(int)bs));
}
/* Fill the data buffer with a recognisable test pattern (for write verification). */
void rp1usb_msd_fill_pattern(unsigned int seed)
{
    for (unsigned i=0;i<sizeof g_msd_data;i++) g_msd_data[i]=(unsigned char)(seed + i);
}

/* Full bring-up of the mass-storage device on controller 0, port `port`
 * (the boot stick is c0p2): init controller 0 if needed, enable a slot, address
 * the device into g_msd_devctx, then Configure the bulk endpoints. */
int rp1usb_msd_fullsetup(int port)
{
    extern int rp1usb_address_device(int,int,int,int);
    rp1usb_select_ctrl(0);
    if (!g_cm[0].inited) rp1usb_xhci_init();
    if (!g_cm[0].inited) return -100;
    int slot = -1;
    for (int attempt=0; attempt<3 && slot<0; attempt++) slot = rp1usb_enum_slot(port);
    if (slot < 0) return -10;
    int speed = (int)((g_enum_portsc >> 10) & 0xf);
    /* The boot mass-storage stick is USB2 High-Speed, but this RP1/DWC3
     * controller's PORTSC speed field reports 2 (its non-standard PSI map — the
     * same quirk noted for the mouse slot context: actual HS=3 while PORTSC reads
     * 2).  A wrong slot/EP speed makes the periodic+async scheduler mis-handle the
     * bulk endpoints so transfers never complete (CBW times out).  Force HS. */
    if (speed < 3) speed = 3;
    g_addr_ctx = g_msd_devctx;
    int ar = -1;
    for (int attempt=0; attempt<2 && ar!=0; attempt++) ar = rp1usb_address_device(slot, port, speed, 0);
    g_addr_ctx = g_dev_ctx;
    if (ar != 0) return -20;
    return rp1usb_msd_setup(slot, port, speed);
}

unsigned int rp1usb_msd_setup_cc(void) { return g_msd_setup_cc; }
unsigned int rp1usb_msd_cfgep_cc(void) { return g_msd_cfgep_cc; }
unsigned int rp1usb_msd_csw_status(void){ return g_msd_csw_status; }
unsigned int rp1usb_msd_residue(void)  { return g_msd_residue; }
unsigned int rp1usb_msd_blocks(void)   { return g_msd_blocks; }
unsigned int rp1usb_msd_blocksize(void){ return g_msd_blocksize; }
int          rp1usb_msd_slot(void)     { return g_msd_slot; }
int          rp1usb_msd_in_ep(void)    { return g_msd_in_ep; }
int          rp1usb_msd_out_ep(void)   { return g_msd_out_ep; }
int          rp1usb_msd_in_dci(void)   { return g_msd_in_dci; }
int          rp1usb_msd_out_dci(void)  { return g_msd_out_dci; }
int          rp1usb_msd_last_r(void)   { return g_msd_last_r; }
int          rp1usb_msd_p_cbw(void)    { return g_p_cbw; }
int          rp1usb_msd_p_data(void)   { return g_p_data; }
int          rp1usb_msd_p_csw(void)    { return g_p_csw; }
unsigned int rp1usb_msd_p_cbw_cc(void) { return g_p_cbw_cc; }
unsigned int rp1usb_msd_p_data_cc(void){ return g_p_data_cc; }
unsigned int rp1usb_msd_p_csw_cc(void) { return g_p_csw_cc; }
/* device-context introspection (the controller writes EP/slot state here). */
unsigned int rp1usb_msd_slotstate(void) { return (ctx_at(g_msd_devctx,0)[3]>>27)&0x1f; }
unsigned int rp1usb_msd_out_epstate(void){ return ctx_at(g_msd_devctx,g_msd_out_dci)[0]&0x7; }
unsigned int rp1usb_msd_in_epstate(void) { return ctx_at(g_msd_devctx,g_msd_in_dci)[0]&0x7; }
unsigned int rp1usb_msd_out_deqlo(void)  { return ctx_at(g_msd_devctx,g_msd_out_dci)[2]; }
unsigned int rp1usb_msd_in_deqlo(void)   { return ctx_at(g_msd_devctx,g_msd_in_dci)[2]; }
unsigned int rp1usb_msd_out_ep1(void)    { return ctx_at(g_msd_devctx,g_msd_out_dci)[1]; }
unsigned int rp1usb_msd_usbsts(void)     { return R32(g_oper, OP_USBSTS); }
unsigned int rp1usb_msd_portsc2(void)    { return R32(g_oper, 0x400 + 0x10); }  /* c0 port2 */
unsigned int rp1usb_msd_data_byte(int i){ return (i>=0&&i<(int)sizeof g_msd_data)?g_msd_data[i]:0; }
void         rp1usb_msd_set_byte(int i, unsigned int v){ if(i>=0&&i<(int)sizeof g_msd_data) g_msd_data[i]=(unsigned char)v; }

#endif /* RP1_ETH_BASE */
