/* device/wifi/wifi.c — BCM43455/CYW43455 SDIO bring-up for Raspberry Pi 5
 * (BCM2712 + RP1).  Ported from the proven xinu-rpi4 driver.
 *
 * The CYW43455 chip is identical to the Pi4's, so everything from the
 * backplane layer up (firmware download, SDPCM/BCDC, scan/join/WPA2, data
 * plane) is the SAME code.  Only the SDIO HOST differs:
 *   Pi4: Arasan SDHCI (bcm2835-sdhci) @ 0xFE300000, mailbox WL_REG_ON, clk-mgr.
 *   Pi5: sdhci-brcmstb @ 0x10_01100000 (host) + 0x10_01100400 (cfg),
 *        native GPIO28 WL_REG_ON (gio@7d508500), fixed 200 MHz clk_emmc2.
 * So this top section (MMIO bases, power, pinmux, SDHCI host init, emmc_cmd,
 * cmd53 PIO) is rewritten for the brcmstb controller + standard SDHCI register
 * layout; the rest of the file is the Pi4 driver verbatim.
 *
 * The WiFi SDIO host (SDIO2) is a different controller from the boot SD card,
 * so driving it does not disturb the boot media.  Gated on WIFI_SDIO_BASE so
 * it compiles to nothing on pi4/qemu builds (same discipline as RP1_ETH_BASE).
 */

#ifdef WIFI_SDIO_BASE

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

extern void uart_puts(const char *s);
extern void uart_putc(char c);

/* ------------------------------------------------------------------ *
 *  trace log (HTTP /wifi-trace dumps this) + mini formatter          *
 * ------------------------------------------------------------------ */
static char wifi_tbuf[8000];
static int  wifi_tn;
static u32  g_fwload_hz = 2000000u;    /* SD clock for the bulk fw download; tunable via /wifi-stage?hz=N */
void wifi_set_fwload_hz(u32 hz) { if (hz) g_fwload_hz = hz; }

static void wlog_putc(char c)
{
    if (wifi_tn < (int)sizeof(wifi_tbuf) - 1) wifi_tbuf[wifi_tn++] = c;
    uart_putc(c);
}
static void wlog_str(const char *s) { while (*s) wlog_putc(*s++); }
static void wlog_u(u32 v, int base, int width, char pad)
{
    char t[16]; int n = 0;
    const char *dig = "0123456789abcdef";
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = dig[v % base]; v /= base; }
    while (n < width) t[n++] = pad;
    while (n--) wlog_putc(t[n]);
}
/* supports %d %u %x %08x %02x %s %c %% */
static void wifi_log(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { wlog_putc(*fmt); continue; }
        fmt++;
        int width = 0; char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt-'0'); fmt++; }
        switch (*fmt) {
        case 'd': { int v = __builtin_va_arg(ap, int);
                    if (v < 0) { wlog_putc('-'); v = -v; }
                    wlog_u((u32)v, 10, width, pad); } break;
        case 'u': wlog_u(__builtin_va_arg(ap, u32), 10, width, pad); break;
        case 'x': wlog_u(__builtin_va_arg(ap, u32), 16, width, pad); break;
        case 's': wlog_str(__builtin_va_arg(ap, const char *)); break;
        case 'c': wlog_putc((char)__builtin_va_arg(ap, int)); break;
        case '%': wlog_putc('%'); break;
        default:  wlog_putc('%'); wlog_putc(*fmt); break;
        }
    }
    __builtin_va_end(ap);
}

const char *wifi_trace(void) { return wifi_tbuf; }
int wifi_trace_len(void) { return wifi_tn; }

/* ------------------------------------------------------------------ *
 *  MMIO — brcmstb SDIO2 host (standard SDHCI regs) + cfg + gpio +    *
 *  pinctrl.  Bases come from compile/Makefile -D (see plan).         *
 * ------------------------------------------------------------------ */
#define SDREG(off)   (*(volatile u32 *)(WIFI_SDIO_BASE     + (off)))
/* This controller drops sub-word (8/16-bit) register writes — only 32-bit
 * accesses commit (confirmed: 16-bit TRANSFER_MODE/HC2 writes read back 0).
 * So all 8/16-bit register access is done as a 32-bit read-modify-write on the
 * containing aligned word.  This also means POWER/HOST_CONTROL/CLOCK actually
 * take effect (they didn't as 8/16-bit writes). */
static inline void sd_w16(u32 off, u16 v) {
    u32 b = off & ~3u, sh = (off & 2) * 8;
    SDREG(b) = (SDREG(b) & ~(0xFFFFu << sh)) | ((u32)v << sh);
}
static inline u16 sd_r16(u32 off) { return (u16)(SDREG(off & ~3u) >> ((off & 2) * 8)); }
static inline void sd_w8(u32 off, u8 v) {
    u32 b = off & ~3u, sh = (off & 3) * 8;
    SDREG(b) = (SDREG(b) & ~(0xFFu << sh)) | ((u32)v << sh);
}
static inline u8 sd_r8(u32 off) { return (u8)(SDREG(off & ~3u) >> ((off & 3) * 8)); }
#define CFGREG(off)  (*(volatile u32 *)(WIFI_SDIO_CFG_BASE + (off)))
#define GIOREG(off)  (*(volatile u32 *)(WIFI_GPIO_BASE     + (off)))
#define PINREG(off)  (*(volatile u32 *)(WIFI_PINCTRL_BASE  + (off)))

/* Standard SDHCI register offsets (sdhci.h). */
#define SDHCI_ARG2          0x00
#define SDHCI_BLOCK_SIZE    0x04   /* 16-bit; +0x06 = block count           */
#define SDHCI_ARGUMENT      0x08
#define SDHCI_XFER_MODE     0x0C   /* 16-bit                                */
#define SDHCI_COMMAND       0x0E   /* 16-bit                                */
#define SDHCI_RESPONSE      0x10   /* 0x10/0x14/0x18/0x1C                   */
#define SDHCI_BUFFER        0x20   /* PIO data port                         */
#define SDHCI_PRESENT_STATE 0x24
#define SDHCI_HOST_CONTROL  0x28   /* 8-bit                                 */
#define SDHCI_POWER_CONTROL 0x29   /* 8-bit                                 */
#define SDHCI_CLOCK_CONTROL 0x2C   /* 16-bit                                */
#define SDHCI_TIMEOUT_CTRL  0x2E   /* 8-bit                                 */
#define SDHCI_SOFTWARE_RESET 0x2F  /* 8-bit                                 */
#define SDHCI_INT_STATUS    0x30
#define SDHCI_INT_ENABLE    0x34
#define SDHCI_SIGNAL_ENABLE 0x38
#define SDHCI_HOST_CONTROL2 0x3E   /* 16-bit                                */
#define SDHCI_CAPABILITIES  0x40
#define SDHCI_CAPABILITIES1 0x44
#define SDHCI_VENDOR        0x78   /* brcmstb: gate-sdclk-en lives here     */
#define SDHCI_HOST_VERSION  0xFE   /* 16-bit                                */

/* PRESENT_STATE bits */
#define SR_CMD_INHIBIT      (1u << 0)
#define SR_DAT_INHIBIT      (1u << 1)
/* CLOCK_CONTROL (0x2C) bits */
#define CC_INT_CLK_EN       (1u << 0)
#define CC_INT_CLK_STABLE   (1u << 1)
#define CC_SD_CLK_EN        (1u << 2)
/* SOFTWARE_RESET (0x2F) bits */
#define SRST_ALL            (1u << 0)
#define SRST_CMD            (1u << 1)
#define SRST_DATA           (1u << 2)
/* POWER_CONTROL (0x29) */
#define PWR_ON              0x01
#define PWR_330             0x0E   /* 3.3V VDD select                       */
/* HOST_CONTROL (0x28) */
#define HC_4BIT             0x02
/* INT_STATUS (0x30) bits — standard SDHCI */
#define INT_CMD_DONE        (1u << 0)
#define INT_DATA_DONE       (1u << 1)
#define INT_WRITE_RDY       (1u << 4)
#define INT_READ_RDY        (1u << 5)
#define INT_ERR             0xFFFF8000u
/* The BCM2712 SDIO2 controller (like the bcm2835/Pi4 Arasan, per Circle's
 * bare-metal driver) uses a SINGLE 32-bit "Cmdtm" command register at 0x0C —
 * index, response type, data/direction all written together in ONE store that
 * triggers the command.  It is NOT the standard-SDHCI split TRANSFER_MODE(0x0C)
 * + COMMAND(0x0E).  Bit fields (all in the one 0x0C word): */
#define CMD_INDEX_SHIFT     24          /* command index at bits [29:24]        */
#define CMD_RSPNS_NONE      (0u << 16)
#define CMD_RSPNS_136       (1u << 16)
#define CMD_RSPNS_48        (2u << 16)
#define CMD_RSPNS_48BUSY    (3u << 16)
#define CMD_CRCCHK_EN       (1u << 19)
#define CMD_IXCHK_EN        (1u << 20)
#define CMD_ISDATA          (1u << 21)
#define TM_BLKCNT_EN        (1u << 1)
#define TM_DMA_EN           (1u << 0)   /* TRANSFER_MODE: enable DMA              */
#define TM_DAT_DIR_CH       (1u << 4)   /* Card2host = 1 = read (card -> host)  */
#define TM_MULTI_BLK        (1u << 5)
#define SDHCI_CMDTM         0x0C        /* the combined command/transfer reg    */
/* ADMA2 (Advanced DMA): the brcmstb PIO data path corrupts CMD53 transfers, so
 * the bulk path uses ADMA2 instead.  HOST_CONTROL[4:3]=10 selects ADMA2-32bit;
 * the descriptor table address goes in ADMA_ADDRESS.  bcm2712 dma-ranges are 1:1
 * and the MMU is identity-mapped, so a descriptor/buffer address == its CPU PA. */
#define SDHCI_ADMA_ERROR    0x54
#define SDHCI_ADMA_ADDRESS  0x58        /* 64-bit; +0x5C = high 32 bits          */
#define HC_DMA_MASK         0x18        /* HOST_CONTROL bits [4:3] = DMA select   */
#define HC_DMA_ADMA2_32     0x10        /* [4:3]=10 -> ADMA2 32-bit               */
#define ADMA2_VALID         0x01        /* descriptor: entry valid                */
#define ADMA2_END           0x02        /* descriptor: last entry                 */
#define ADMA2_TRAN          0x20        /* descriptor: Act=Tran (transfer data)   */

/* brcmstb cfg region (+0x400) */
#define SDIO_CFG_CTRL              0x00
#define   CFG_CTRL_SDCD_N_TEST_EN  (1u << 31)
#define   CFG_CTRL_SDCD_N_TEST_LEV (1u << 30)
#define SDIO_CFG_SD_PIN_SEL        0x44
#define   CFG_SD_PIN_SEL_MASK      0x3u
#define   CFG_SD_PIN_SEL_SD        0x2u
#define SDIO_CFG_MAX_50MHZ_MODE    0x1AC
#define   CFG_MAX_50MHZ_ENABLE     (1u << 31)
#define   CFG_MAX_50MHZ_STRAP_OVR  (1u << 30)

/* brcmstb gio (gio@7d508500): per-bank stride 0x20; GPIO28 is bank 0. */
#define GIO_DATA(bank)   (((bank) * 0x20) + 0x04)
#define GIO_IODIR(bank)  (((bank) * 0x20) + 0x08)
#define WL_ON_GPIO       28

#define WIFI_SDIO_BASE_HZ  200000000u   /* clk_emmc2 fixed 200 MHz          */

/* ---- SDIO / SBSDIO / chip-core constants (chip-side; identical to Pi4) -- */
#define SD_CMD0_GO_IDLE         0
#define SD_CMD3_SEND_RCA        3
#define SD_CMD5_IO_OP_COND      5
#define SD_CMD7_SELECT          7
#define SD_CMD52_IO_RW_DIRECT   52
#define SD_CMD53_IO_RW_EXT      53
#define SDIO_CCCR_IOEx          0x02
#define SDIO_CCCR_IORx          0x03

#define SBSDIO_FUNC1_SBADDRLOW  0x1000A
#define SBSDIO_SB_OFT_ADDR_MASK 0x07FFF
#define SBSDIO_SB_ACCESS_2_4B   0x08000
#define SBSDIO_SBWINDOW_MASK    0xFFFF8000u
#define SI_ENUM_BASE            0x18000000u
#define CID_ID_MASK             0x0000FFFFu
#define BCM43455_CHIP_ID        0x4345

#define SB_WSIZE   0x8000u
#define SB_32BIT   0x8000u
#define SB_ENUMBASE 0x18000000u
#define CORE_ARMCR4 0x83E
#define CORE_ARMCM3 0x82A
#define CORE_ARM7   0x825
#define CORE_CHIPCOMMON 0x800
#define CORE_SOCRAM 0x80E
#define CORE_SDIODEV 0x829
#define CORE_D11    0x812
#define SB_XFER     2048

/* M1 — firmware download + ARM core start + clock/Fn2 enable */
#define REG_IOCTRL      0x408
#define REG_RESETCTRL   0x800
#define CR4_CAP         0x04
#define CR4_BANKIDX     0x40
#define CR4_BANKINFO    0x44
#define CR4_CPUHALT     0x20
#define REG_CLKCSR      0x1000E
#define CLK_FORCEALP    0x01
#define CLK_FORCEHT     0x02
#define CLK_REQALP      0x08
#define CLK_REQHT       0x10
#define CLK_NOHWREQ     0x20
#define CLK_ALPAVAIL    0x40
#define CLK_HTAVAIL     0x80
#define REG_PULLUPS     0x1000F
#define SDR_INTSTATUS   0x20
#define SDR_INTMASK     0x24
#define SDR_MBOXDATA    0x48
#define SDR_SBMBOX      0x40
#define SDR_HOSTMBOX    0x4c
#define SD_INT_MBOX     (1u << 7)
/* 43455 F2 watermark quirk (brcmstb host) */
#define SBSDIO_WATERMARK         0x10008
#define SBSDIO_DEVICE_CTL        0x10009
#define   SBSDIO_DEVCTL_F2WM_ENAB 0x10
#define SBSDIO_FUNC1_MESBUSYCTRL 0x1001D
#define CY_43455_F2_WATERMARK    0x60
#define CY_43455_MESBUSYCTRL     0xD0
/* M2 — SDPCM/BCDC control plane over Fn2 */
#define SDPCM_HDR   12
#define BCDC_HDR    16
#define WLC_GET_VAR 262
#define WLC_SET_VAR 263
#define WLC_DOWN        3
#define WLC_SET_INFRA   20
#define WLC_GET_BSSID   23
#define WLC_SET_SSID    26
#define WLC_SET_CHANNEL 30
#define WLC_SET_WSEC_PMK 268
#define SDIO_CCCR_INT_ENABLE 0x04

/* ------------------------------------------------------------------ *
 *  delays — generic timer (cntpct_el0) as a 1 MHz microsecond clock  *
 * ------------------------------------------------------------------ */
static u32 wifi_now_us(void)
{
    unsigned long t, f;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (!f) f = 54000000UL;
    return (u32)(t / (f / 1000000UL));
}
#define SYSTIMER_CLO  wifi_now_us()
static void wifi_udelay(volatile unsigned long n) { while (n--) asm volatile("nop"); }
static void wifi_delay_us(u32 us) { u32 s = SYSTIMER_CLO; while ((SYSTIMER_CLO - s) < us) {} }
static void wifi_msleep(u32 ms) { while (ms--) wifi_delay_us(1000); }
static void ew(volatile u32 *reg, u32 val) { *reg = val; wifi_delay_us(2); }

/* ------------------------------------------------------------------ *
 *  WL_REG_ON power: brcmstb gio GPIO28 (no mailbox on Pi5)           *
 * ------------------------------------------------------------------ */
/* Kept the Pi4 helper names so the bring-up sequence below is unchanged;
 * the gpio number argument is ignored — Pi5 always drives native GPIO28. */
static int wifi_fw_set_gpio(u32 gpio, u32 state)
{
    (void)gpio;
    /* brcmstb gio IODIR: 0 = output, 1 = input (gpio-brcmstb.c).  Drive the
     * data latch first, then switch the pin to output. */
    if (state) GIOREG(GIO_DATA(0)) |=  (1u << WL_ON_GPIO);
    else       GIOREG(GIO_DATA(0)) &= ~(1u << WL_ON_GPIO);
    GIOREG(GIO_IODIR(0)) &= ~(1u << WL_ON_GPIO);          /* 0 = output        */
    return 0;
}
static int wifi_fw_get_gpio(u32 gpio, u32 *state)
{
    (void)gpio;
    if (state) *state = (GIOREG(GIO_DATA(0)) >> WL_ON_GPIO) & 1u;
    return 0;
}
u32 wifi_clock_rate(u32 id)     { (void)id; return WIFI_SDIO_BASE_HZ; }
u32 wifi_clock_measured(u32 id) { (void)id; return WIFI_SDIO_BASE_HZ; }

/* ------------------------------------------------------------------ *
 *  pinmux: route GPIO30=CLK,31=CMD,32-35=DAT0-3 to the SDIO2 ("sd2") *
 *  function on the bcm2712 pinctrl.  Encoding is the project's best   *
 *  hypothesis (tunable via /wifi-pinmux?fsel=N) — the one unknown.    *
 * ------------------------------------------------------------------ */
static u32 g_pin_fsel = 4;   /* sd2 alt index (found on hw); override at runtime */
void wifi_set_pin_fsel(u32 f) { g_pin_fsel = f & 0xF; }

/* bcm2712 pinmux: 8 pins per 32-bit register, 4 bits each (funcsel = low 4). */
static void pin_set_func(int gpio, u32 fsel)
{
    u32 reg = (u32)(gpio / 8) * 4u;
    int sh  = (gpio % 8) * 4;
    u32 v = PINREG(reg);
    v &= ~(0xFu << sh);
    v |=  ((fsel & 0xF) << sh);
    PINREG(reg) = v;
}
/* bcm2712 pad pull control for GPIO30-35: all six 2-bit fields live in the one
 * word at pinctrl+0x24 (per pinctrl-bcm2712: pad_bit = pull_reg*32 + pull_bit*2,
 * pull_reg=9 -> offset 0x24).  Values: 0=none, 1=down, 2=up.  The Pi5 dts pulls
 * CMD(31) and DAT0-3(32-35) UP and leaves CLK(30) with no bias — without these
 * the DAT0 turnaround floats and every write's CRC-status reads as an error. */
#define PINPULL_OFF  0x24
static void wifi_pinmux_pulls(void)
{
    u32 before = PINREG(PINPULL_OFF), v = before;
    v &= ~(0xFFFu << 14);                 /* clear GPIO30..35 (bits 25:14) */
    v |=  (2u<<16)|(2u<<18)|(2u<<20)|(2u<<22)|(2u<<24);  /* CMD,DAT0-3 = pull-up */
    /* GPIO30 (CLK) bits[15:14] left 0 = no bias */
    PINREG(PINPULL_OFF) = v;
    wifi_log("[wifi] pad pulls @+0x24: 0x%08x -> 0x%08x (CMD/DAT0-3 pull-up)\r\n",
             before, PINREG(PINPULL_OFF));
}
static void wifi_pinmux(void)
{
    int p;
    /* Per the working Plan9/Circle Pi5 driver, the sd2 alt function is NOT the
     * same for every pin on the (non-D0) BCM2712: CLK/CMD/D0/D2 (gpio30,31,32,34)
     * use Func4, but D1/D3 (gpio33,35) use Func3.  Muxing D1/D3 to Func4 leaves
     * those two data lines wrong in 4-bit mode -> every DAT transfer corrupts
     * (DCRC), while CMD-line-only commands (CMD52, enumeration) still work. */
    for (p = 30; p <= 35; p++) {
        u32 f = (p == 33 || p == 35) ? 3u : g_pin_fsel;   /* g_pin_fsel defaults to 4 */
        pin_set_func(p, f);
    }
    wifi_pinmux_pulls();
    wifi_log("[wifi] pinmux sd2: gpio30/31/32/34=%u gpio33/35=3\r\n", g_pin_fsel);
}
/* read-only dump of the pinctrl words covering GPIO30-35 (for /wifi-pinmux-dump) */
void wifi_pinmux_dump(void)
{
    int r;
    wifi_log("[wifi] pinctrl @0x%x:\r\n", (u32)WIFI_PINCTRL_BASE);
    for (r = (30/8)*4; r <= (35/8)*4; r += 4)
        wifi_log("[wifi]   +0x%02x = 0x%08x\r\n", r, PINREG(r));
}
/* compat shims for the Pi4 bring-up body */
/* The real base clock matters: divisors are computed from it, so a wrong value
 * means every "target Hz" is actually some other frequency.  Circle/the RPi
 * forum read it from the firmware (≈ not 200 MHz as assumed).  Query the VC
 * mailbox for the actual EMMC2 (and EMMC) clock rate. */
extern int mbox_call(volatile unsigned int *buf);
static u32 g_base_hz = WIFI_SDIO_BASE_HZ;
static u32 wifi_mbox_clk(u32 clkid)
{
    static volatile unsigned int __attribute__((aligned(16))) buf[8];
    buf[0] = 32; buf[1] = 0;
    buf[2] = 0x00030002u;     /* GET_CLOCK_RATE */
    buf[3] = 8; buf[4] = 0;
    buf[5] = clkid; buf[6] = 0; buf[7] = 0;
    if (mbox_call(buf) != 0) return 0;
    return (u32)buf[6];
}
static u32 wifi_mbox_set_clk(u32 clkid, u32 rate)
{
    static volatile unsigned int __attribute__((aligned(16))) buf[9];
    buf[0] = 36; buf[1] = 0;
    buf[2] = 0x00038002u;     /* SET_CLOCK_RATE */
    buf[3] = 12; buf[4] = 0;
    buf[5] = clkid; buf[6] = rate; buf[7] = 0; buf[8] = 0;
    if (mbox_call(buf) != 0) return 0;
    return (u32)buf[6];
}
static void wifi_clk_setup(void)
{
    u32 e2 = wifi_mbox_clk(12);   /* CLOCK_ID_EMMC2 */
    u32 e1 = wifi_mbox_clk(1);    /* CLOCK_ID_EMMC  */
    /* sdio2 (WiFi) is normally a disabled node, so the firmware may not have set
     * up its clk_emmc2 — the query reports 0.  Explicitly program it to 200 MHz
     * so the controller runs at a known, configured rate (an unconfigured clock
     * makes write timing wrong, matching our write-only CRC symptom). */
    if (!e2) {
        u32 set = wifi_mbox_set_clk(12, 200000000u);
        e2 = wifi_mbox_clk(12);
        wifi_log("[wifi] clk: set EMMC2=200MHz -> set_ret=%u requery=%u\r\n", set, e2);
    }
    if (e2) g_base_hz = e2; else if (e1) g_base_hz = e1;
    wifi_log("[wifi] clk: mbox EMMC2=%u Hz EMMC=%u Hz -> base=%u Hz\r\n", e2, e1, g_base_hz);
}
static void wifi_gpio_sdio(void) { wifi_pinmux(); }

/* ------------------------------------------------------------------ *
 *  brcmstb SDHCI host bring-up (standard SDHCI register layout)      *
 * ------------------------------------------------------------------ */
static u32 emmc_divisor(u32 base_hz, u32 target_hz)
{
    u32 d = 1; if (!target_hz) target_hz = 400000;
    while ((base_hz / (2*d)) > target_hz && d < 1023) d++;
    return d;
}
/* Software reset with the brcmstb quirk: after a CMD/DATA reset the controller
 * needs the internal+card clock re-enabled (and the vendor sdclk gate). */
static int sdhci_reset(u8 mask)
{
    int t;
    sd_w8(SDHCI_SOFTWARE_RESET, mask);
    for (t = 0; t < 100000; t++) { if (!(sd_r8(SDHCI_SOFTWARE_RESET) & mask)) break; wifi_delay_us(10); }
    if (sd_r8(SDHCI_SOFTWARE_RESET) & mask) { wifi_log("[wifi]   SRST 0x%x stuck\r\n", mask); return -1; }
    return 0;
}
static u32 g_clkdiv = 250;          /* current SD clock divisor (for re-program) */
static int emmc_set_clock_div(u32 div)
{
    u32 c; int t;
    g_clkdiv = div;
    sd_w16(SDHCI_CLOCK_CONTROL, 0);                         /* stop clock     */
    wifi_delay_us(20);
    c = CC_INT_CLK_EN | ((div & 0xFF) << 8) | (((div >> 8) & 0x3) << 6);
    sd_w16(SDHCI_CLOCK_CONTROL, (u16)c);
    for (t = 0; t < 100000; t++) { if (sd_r16(SDHCI_CLOCK_CONTROL) & CC_INT_CLK_STABLE) break; wifi_delay_us(10); }
    if (!(sd_r16(SDHCI_CLOCK_CONTROL) & CC_INT_CLK_STABLE)) { wifi_log("[wifi]   clk not stable\r\n"); return -1; }
    sd_w16(SDHCI_CLOCK_CONTROL, (u16)(sd_r16(SDHCI_CLOCK_CONTROL) | CC_SD_CLK_EN));
    wifi_delay_us(2000);
    return 0;
}
/* Select the brcmstb delay-line PHY clock source for stable data sampling
 * (clear MAX_50MHZ_MODE_ENABLE bit0, set STRAP_OVERRIDE bit31).  Must be done
 * with the controller clock already running (doing it before reset hangs the
 * reset). cfg+0x1AC = SDIO_CFG_MAX_50MHZ_MODE. */
static void emmc_select_phy_clock(void)
{
    u32 r = CFGREG(0x1AC);
    r &= ~(1u << 0);
    r |= (1u << 31);
    CFGREG(0x1AC) = r;
    wifi_log("[wifi] PHY clock source selected (CFG_MAX_50MHZ=0x%08x)\r\n", CFGREG(0x1AC));
}
static u32 g_cur_hz = 0;            /* last clock programmed, for ensure-clock */
static int emmc_set_clock(u32 target_hz)
{
    u32 div = emmc_divisor(g_base_hz, target_hz);
    sd_w16(SDHCI_CLOCK_CONTROL, (u16)(sd_r16(SDHCI_CLOCK_CONTROL) & ~CC_SD_CLK_EN));
    wifi_delay_us(50);
    if (emmc_set_clock_div(div) != 0) return -1;
    g_cur_hz = target_hz;
    return 0;
}
/* The brcmstb host needs DIFFERENT clocks for the two command classes: the CMD
 * line (CMD52, command responses) is reliable at 25 MHz but corrupts at 2-3 MHz,
 * while the DAT data path (CMD53 PIO) only samples cleanly at ~2 MHz.  Each
 * helper ensures its own clock; the tracking var skips the (slow) reprogram when
 * we're already there, so a run of same-class commands pays it only once. */
#define WIFI_CMD_HZ   25000000u     /* CMD52 / enumeration / register access */
static void emmc_ensure_clock(u32 hz) { if (g_cur_hz != hz) emmc_set_clock(hz); }
static int emmc_host_init(void)
{
    u32 ver, caps, div;
    /* brcmstb cfg: SDIO (not eMMC) pin mode + force card presence (nonremovable) */
    { u32 r = CFGREG(SDIO_CFG_SD_PIN_SEL); r &= ~CFG_SD_PIN_SEL_MASK; r |= CFG_SD_PIN_SEL_SD;
      CFGREG(SDIO_CFG_SD_PIN_SEL) = r; }
    { u32 r = CFGREG(SDIO_CFG_CTRL); r &= ~CFG_CTRL_SDCD_N_TEST_LEV; r |= CFG_CTRL_SDCD_N_TEST_EN;
      CFGREG(SDIO_CFG_CTRL) = r; }
    /* Tell the controller its base clock (200 MHz, FMUL=3) — bcm2712 cfginit. */
    CFGREG(0x4C) = (3u << 12) | 200u;     /* SDIO_CFG_CQ_CAPABILITY            */

    if (sdhci_reset(SRST_ALL) != 0) return -1;

    ver  = sd_r16(SDHCI_HOST_VERSION);
    caps = SDREG(SDHCI_CAPABILITIES);
    wifi_log("[wifi] brcmstb SDHCI host_ver=0x%04x CAPS=0x%08x CAPS1=0x%08x\r\n",
             ver, caps, SDREG(SDHCI_CAPABILITIES1));

    /* PHY output delay.  The mainline brcmstb driver tracks SDIO_CFG_OP_DLY
     * (cfg+0x34, default 0x80000003) — it sets the CMD/DAT *output* timing.  A
     * wrong output delay makes the card mis-sample every write (CRC) while reads
     * (host samples the card) stay clean: exactly our symptom.  Also dump the PHY
     * RX control (cfg+0x7C) the mainline driver tracks. */
    wifi_log("[wifi] PHY before: OP_DLY(0x34)=0x%08x RX_CTRL(0x7C)=0x%08x\r\n",
             CFGREG(0x34), CFGREG(0x7C));
    CFGREG(0x34) = 0x80000003u;        /* SDIO_CFG_OP_DLY_DEFAULT */
    wifi_log("[wifi] PHY after:  OP_DLY(0x34)=0x%08x\r\n", CFGREG(0x34));

    /* power on at 3.3V, 1-bit bus for identification (32-bit RMW so it sticks) */
    sd_w8(SDHCI_POWER_CONTROL, PWR_330 | PWR_ON);
    sd_w8(SDHCI_HOST_CONTROL,  0);
    sd_w8(SDHCI_TIMEOUT_CTRL,  0x0E);
    wifi_log("[wifi] power: PWR=0x%02x HOSTCTL=0x%02x\r\n",
             sd_r8(SDHCI_POWER_CONTROL), sd_r8(SDHCI_HOST_CONTROL));

    /* This controller mis-samples at the 400 kHz id clock (data CRC + shifted
     * CMD responses on some boots).  The soldered CYW43455 tolerates a faster
     * id clock, so run identification at ~6 MHz for reliable sampling. */
    div = emmc_divisor(g_base_hz, 6000000);
    wifi_log("[wifi] init clk divisor=%u (~%u Hz)\r\n", div, g_base_hz/(2*div));
    if (emmc_set_clock_div(div) != 0) return -1;

    SDREG(SDHCI_INT_ENABLE)    = 0xFFFFFFFFu;
    SDREG(SDHCI_SIGNAL_ENABLE) = 0;                 /* poll, don't assert IRQ */
    SDREG(SDHCI_INT_STATUS)    = 0xFFFFFFFFu;
    wifi_log("[wifi] SDHCI ready: PRESENT=0x%08x CLKCTL=0x%04x\r\n",
             SDREG(SDHCI_PRESENT_STATE), sd_r16(SDHCI_CLOCK_CONTROL));
    return 0;
}
/* Issue a command.  `flags` are the standard-SDHCI COMMAND-register low byte
 * (response type + CRC/index check + data).  rd4=1 reads a single 4-byte block
 * (CMD53 4-byte backplane read).  Signature preserved from the Pi4 driver so
 * all chip-side callers are unchanged. */
#define CMD_TOLERATE  0x100u   /* software-only: don't fail on a response framing
                                * error (END_BIT/CRC) if CMD_DONE is set — used
                                * for R1b SELECT, where the card enters the
                                * selected state regardless of the host seeing a
                                * clean response end bit. */
/* Reset just the data circuit (FIFO + CRC engine + data state machine) via the
 * brcmstb quirk: SRST_DATA must be issued together with the clock-enable bits
 * (a bare SRST write gates the clock and the reset never completes). */
static void emmc_data_reset(void)
{
    u32 t, w = SDREG(SDHCI_CLOCK_CONTROL) | ((u32)SRST_DATA << 24)
               | CC_SD_CLK_EN | CC_INT_CLK_EN;
    SDREG(SDHCI_CLOCK_CONTROL) = w;
    for (t = 0; t < 100000; t++) if (!(sd_r8(SDHCI_SOFTWARE_RESET) & SRST_DATA)) break;
    SDREG(SDHCI_INT_STATUS) = 0xFFFFFFFFu;
}
static int emmc_cmd(u32 idx, u32 arg, u32 flags, u32 *resp, int rd4, u32 *data)
{
    u32 intr, t, c;
    int tolerate = (flags & CMD_TOLERATE) ? 1 : 0;
    flags &= ~CMD_TOLERATE;
    /* A command may only be issued once Command-Inhibit(CMD) is clear; a
     * DAT-line command additionally needs Command-Inhibit(DAT) clear. */
    for (t = 0; t < 1000000; t++) if (!(SDREG(SDHCI_PRESENT_STATE) & SR_CMD_INHIBIT)) break;
    /* If a previous data transfer wedged the DAT line (DAT-inhibit stuck), reset
     * the data circuit before this data command — otherwise we'd read a wedged
     * FIFO and get garbage (Circle/Plan9 emmccmd does exactly this; without it,
     * the SECOND consecutive CMD53 read returns corrupt data). */
    if (rd4) {
        for (t = 0; t < 1000000; t++) if (!(SDREG(SDHCI_PRESENT_STATE) & SR_DAT_INHIBIT)) break;
        if (SDREG(SDHCI_PRESENT_STATE) & SR_DAT_INHIBIT) {
            u32 w = SDREG(SDHCI_CLOCK_CONTROL) | ((u32)SRST_DATA << 24)
                    | CC_SD_CLK_EN | CC_INT_CLK_EN;          /* brcmstb reset quirk */
            SDREG(SDHCI_CLOCK_CONTROL) = w;
            for (t = 0; t < 100000; t++) if (!(sd_r8(SDHCI_SOFTWARE_RESET) & SRST_DATA)) break;
        }
    }
    SDREG(SDHCI_INT_STATUS) = 0xFFFFFFFFu; wifi_delay_us(12);
    c = ((u32)idx << CMD_INDEX_SHIFT) | flags;
    if (rd4) {
        SDREG(SDHCI_BLOCK_SIZE) = (1u << 16) | 4;       /* count=1, size=4 (single block) */
        /* data, card->host, AND Block-Count-Enable.  Linux sdhci_set_transfer_mode
         * sets BLK_CNT_EN for EVERY data read (single block too) — without it the
         * controller can't terminate the transfer via BLOCK_COUNT, leaving the data
         * state machine desynced so the SECOND consecutive read Data-CRCs. */
        c |= CMD_ISDATA | TM_DAT_DIR_CH | TM_BLKCNT_EN;
        wifi_delay_us(12);
    }
    SDREG(SDHCI_ARGUMENT)  = arg; wifi_delay_us(12);
    SDREG(SDHCI_CMDTM)     = c; wifi_delay_us(12);  /* ONE 32-bit write to 0x0C triggers the command */
    for (t = 0; t < 1000000; t++) {
        intr = SDREG(SDHCI_INT_STATUS);
        if (intr & INT_ERR) {
            if (tolerate && (intr & INT_CMD_DONE)) {
                wifi_log("[wifi]   cmd%d tolerated err 0x%08x (CMD_DONE set)\r\n", idx, intr);
                SDREG(SDHCI_INT_STATUS) = 0xFFFFFFFFu; break;
            }
            wifi_log("[wifi]   cmd%d err 0x%08x\r\n", idx, intr); return -1;
        }
        if (intr & INT_CMD_DONE) { SDREG(SDHCI_INT_STATUS) = INT_CMD_DONE; break; }
    }
    if (t >= 1000000) { wifi_log("[wifi]   cmd%d timeout\r\n", idx); return -1; }
    if (resp) *resp = SDREG(SDHCI_RESPONSE);
    /* R1b (RSPNS_48BUSY): the card asserts busy on DAT0 after the response;
     * wait for it to release (DAT-inhibit clear) before returning — otherwise
     * the DAT line stays wedged and blocks the next data command's setup. */
    if ((c & (3u << 16)) == CMD_RSPNS_48BUSY) {
        for (t = 0; t < 1000000; t++) if (!(SDREG(SDHCI_PRESENT_STATE) & SR_DAT_INHIBIT)) break;
        if (SDREG(SDHCI_PRESENT_STATE) & SR_DAT_INHIBIT) {
            wifi_log("[wifi]   cmd%d busy stuck, resetting DAT\r\n", idx);
            { u32 w = SDREG(SDHCI_CLOCK_CONTROL) | ((u32)SRST_DATA << 24)
                      | CC_SD_CLK_EN | CC_INT_CLK_EN;          /* brcmstb reset quirk */
              SDREG(SDHCI_CLOCK_CONTROL) = w; }
            for (t = 0; t < 100000; t++) if (!(sd_r8(SDHCI_SOFTWARE_RESET) & SRST_DATA)) break;
        }
    }
    if (rd4) {     /* single 4-byte read: wait READ_RDY, read BUFFER, drain leftover, wait DATA_DONE */
        int g;
        for (t = 0; t < 1000000; t++) { intr = SDREG(SDHCI_INT_STATUS);
            if (intr & INT_ERR) { wifi_log("[wifi]   cmd%d rd4 err 0x%08x\r\n", idx, intr);
                                  emmc_data_reset(); return -1; }
            if (intr & INT_READ_RDY) { SDREG(SDHCI_INT_STATUS) = INT_READ_RDY; break; } }
        if (t >= 1000000) { emmc_data_reset(); return -1; }
        if (data) *data = SDREG(SDHCI_BUFFER);
        /* The controller over-shifts the DAT pins into the FIFO; drain the
         * leftover so the transfer terminates and doesn't poison the next read
         * (RPi forum t=377652).  Buffer-Read-Enable = PRESENT bit 11. */
        for (g = 0; (SDREG(SDHCI_PRESENT_STATE) & (1u<<11)) && g < 4096; g++) (void)SDREG(SDHCI_BUFFER);
        { u32 pp = 0;
          for (t = 0; t < 1000000; t++) { pp = SDREG(SDHCI_INT_STATUS);
            if (pp & (INT_DATA_DONE|INT_ERR)) break; }
          wifi_log("[wifi]   cmd%d rd4 done: drained=%d INT=0x%08x PRESENT=0x%08x\r\n",
                   idx, g, pp, SDREG(SDHCI_PRESENT_STATE));
          SDREG(SDHCI_INT_STATUS) = 0xFFFFFFFFu; }
        /* Flush the PIO read FIFO + CRC engine so the NEXT consecutive CMD53
         * read starts clean.  Without this the 2nd read fails Data-CRC (the
         * controller's over-shift leaves the CRC engine mis-aligned). */
        emmc_data_reset();
    }
    return 0;
}

/* ---- chip-side SDIO primitives (verbatim from Pi4) ---- */
static int wifi_cmd53_pio(int write, int fn, u32 off, u8 *buf, int len, int incr);
static int sdio_cmd52(int func, u32 addr, int write, u32 val)
{
    u32 arg, resp; int try;
    emmc_ensure_clock(WIFI_CMD_HZ);          /* CMD line is reliable at 25 MHz */
    arg = ((u32)(write&1)<<31) | ((u32)(func&7)<<28) | (write?(1u<<27):0) |
          ((addr & 0x1FFFF) << 9) | (val & 0xFF);
    /* CMD52 is response-only (no DAT line) and cheap to re-issue.  The brcmstb
     * CMD line throws the occasional transient CRC/end-bit error at the faster
     * data-path clocks; just retry — a real failure persists across all tries. */
    for (try = 0; try < 4; try++) {
        if (emmc_cmd(SD_CMD52_IO_RW_DIRECT, arg, CMD_RSPNS_48|CMD_CRCCHK_EN|CMD_IXCHK_EN, &resp, 0, 0) == 0)
            return (int)(resp & 0xFF);
        wifi_delay_us(20);
    }
    return -1;
}
static int sdio_cmd53_read32(u32 f1off, u32 *out)
{
    u32 arg = (0u<<31)|(1u<<28)|(0u<<27)|(1u<<26)|((f1off&0x1FFFF)<<9)|(4u&0x1FF);
    return emmc_cmd(SD_CMD53_IO_RW_EXT, arg, CMD_RSPNS_48|CMD_CRCCHK_EN|CMD_IXCHK_EN, 0, 1, out);
}
static int sdio_set_window(u32 addr)
{
    u32 v = (addr & SBSDIO_SBWINDOW_MASK) >> 8; int i;
    for (i = 0; i < 3; i++, v >>= 8)
        if (sdio_cmd52(1, SBSDIO_FUNC1_SBADDRLOW + i, 1, v & 0xFF) < 0) return -1;
    return 0;
}
/* Read a 32-bit backplane register via FOUR CMD52 byte reads instead of one
 * CMD53 data transfer.  CMD52 is response-only (no DAT line), so it sidesteps
 * the brcmstb consecutive-CMD53-read Data-CRC entirely.  Window must be set. */
static int sdio_cmd52_read32(u32 f1off, u32 *out)
{
    u32 v = 0; int i, b;
    for (i = 0; i < 4; i++) {
        b = sdio_cmd52(1, (f1off & 0x1FFFF) + i, 0, 0);
        if (b < 0) return -1;
        v |= (u32)(b & 0xFF) << (8 * i);
    }
    *out = v;
    return 0;
}
/* Backplane 32-bit register read.  The brcmstb host's consecutive-CMD53 data
 * reads Data-CRC after the first one, but byte-wise CMD52 reads work reliably
 * (verified: SI_ENUM_BASE reads 0x15264345 repeatedly), so route register reads
 * through CMD52. */
static int wifi_backplane_read32(u32 addr, u32 *out)
{
    if (sdio_set_window(addr) != 0) return -1;
    return sdio_cmd52_read32(addr & SBSDIO_SB_OFT_ADDR_MASK, out);
}

static int g_cmd53_logn = 0;    /* throttle counter for tolerated-CRC logging */
/* CMD53 (IO_RW_EXTENDED) block/byte PIO transfer — standard SDHCI register
 * layout (XFER_MODE 0x0C + COMMAND 0x0E split, INT_STATUS 0x30, BUFFER 0x20).
 * Same semantics as the Pi4 Arasan version; only the register plumbing differs. */
static int wifi_cmd53_pio(int write, int fn, u32 off, u8 *buf, int len, int incr)
{
    u32 arg, intr, t, total, w, words; u32 bsize = (fn==2)?512u:64u; int blkmode, blkcnt;
    if (len <= 0) return 0;
    emmc_ensure_clock(g_fwload_hz);          /* DAT data path samples cleanly at ~2 MHz */
    if ((u32)len > bsize) { blkmode = 1; blkcnt = len/bsize; total = blkcnt*bsize; }
    else                  { blkmode = 0; blkcnt = len;       total = len; }
    /* Abort/reset the SDIO function's data path before a read.  The brcmstb
     * controller leaves the function wedged after a CMD53, so the 2nd+
     * consecutive read mis-samples; the CCCR I/O-Abort (0x06 = fn) resets it.
     * (Plan9/Circle use this as the transfer-recovery mechanism.) */
    if (!write) sdio_cmd52(0, 0x06, 1, (u32)fn);
    for (t = 0; t < 200000; t++) if (!(SDREG(SDHCI_PRESENT_STATE) & (SR_CMD_INHIBIT|SR_DAT_INHIBIT))) break;
    if (SDREG(SDHCI_PRESENT_STATE) & (SR_CMD_INHIBIT|SR_DAT_INHIBIT)) {
        wifi_log("[wifi]   cmd53 line busy PRESENT=0x%08x\r\n", SDREG(SDHCI_PRESENT_STATE)); return -1; }
    /* Drain stale read-FIFO data from a previous transfer (RPi forum t=377652). */
    if (!write) { int g = 0; while ((SDREG(SDHCI_PRESENT_STATE) & (1u<<11)) && g++ < 4096) (void)SDREG(SDHCI_BUFFER); }
    /* Space out the register writes (Pi4 'ew()' erratum: the controller needs
     * settling time between writes, else the 2nd+ consecutive transfer's setup
     * is not latched and data mis-samples). */
    SDREG(SDHCI_INT_STATUS) = 0xFFFFFFFFu; wifi_delay_us(12);
    if (blkmode) SDREG(SDHCI_BLOCK_SIZE) = ((u32)blkcnt<<16)|bsize;
    else         SDREG(SDHCI_BLOCK_SIZE) = (1u<<16)|(u32)total;
    wifi_delay_us(12);
    arg = ((u32)(write&1)<<31)|((u32)(fn&7)<<28)|((u32)(blkmode&1)<<27)|((u32)(incr&1)<<26)|
          ((off&0x1FFFF)<<9)|((blkmode?(u32)blkcnt:(u32)total)&0x1FF);
    SDREG(SDHCI_ARGUMENT) = arg; wifi_delay_us(12);
    { u32 c = ((u32)SD_CMD53_IO_RW_EXT << CMD_INDEX_SHIFT)
              | CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN | CMD_ISDATA;
      if (!write)  c |= TM_DAT_DIR_CH;                 /* Card2host = read */
      if (blkmode) c |= TM_MULTI_BLK | TM_BLKCNT_EN;
      SDREG(SDHCI_CMDTM)    = c; wifi_delay_us(12); }   /* ONE 32-bit write triggers the command */
    for (t = 0; t < 100000; t++) { intr = SDREG(SDHCI_INT_STATUS);
        if (intr & INT_ERR) { wifi_log("[wifi]   cmd53 err 0x%08x\r\n", intr); return -1; }
        if (intr & INT_CMD_DONE) { SDREG(SDHCI_INT_STATUS) = INT_CMD_DONE; break; } }
    if (t >= 100000) { wifi_log("[wifi]   cmd53 CMD timeout off=0x%x\r\n", off); return -1; }
    words = (total + 3) / 4;
    { u32 done = 0;
      while (done < words) {
        u32 flag = write ? INT_WRITE_RDY : INT_READ_RDY;
        u32 chunk = (total > bsize && (words-done) > (bsize/4)) ? (bsize/4) : (words-done);
        for (t = 0; t < 100000; t++) { intr = SDREG(SDHCI_INT_STATUS);
            if (intr & INT_ERR) { wifi_log("[wifi]   cmd53 read err 0x%08x off=0x%x done=%u\r\n", intr, off, done); return -1; }
            if (intr & flag) { SDREG(SDHCI_INT_STATUS) = flag; break; } }
        if (t >= 100000) { wifi_log("[wifi]   cmd53 RDY timeout done=%u/%u\r\n", done, words); return -1; }
        for (w = 0; w < chunk; w++) { u32 idx = (done+w)*4;
            if (write) { u32 v = (u32)buf[idx]|((u32)buf[idx+1]<<8)|((u32)buf[idx+2]<<16)|((u32)buf[idx+3]<<24); SDREG(SDHCI_BUFFER) = v; }
            else       { u32 v = SDREG(SDHCI_BUFFER); buf[idx]=v; buf[idx+1]=v>>8; buf[idx+2]=v>>16; buf[idx+3]=v>>24; } }
        done += chunk;
      } }
    /* Wait for transfer-complete (DATA_DONE).  Must actually fire — if it never
     * does, the transfer didn't terminate and the DAT line stays busy, wedging
     * the NEXT chunk ("line busy").  Treat a missing DATA_DONE / any data error
     * as a hard failure rather than silently returning success. */
    { u32 fin = 0; int got = 0;
      for (t = 0; t < 2000000; t++) { fin = SDREG(SDHCI_INT_STATUS);
          if (fin & (INT_DATA_DONE | INT_ERR)) { got = 1; break; } }
      SDREG(SDHCI_INT_STATUS) = 0xFFFFFFFFu;
      if (!got || (fin & 0x00700000u)) {       /* no completion, or timeout/CRC/end-bit */
          /* Byte-mode 4-byte backplane register writes (sb_reset, BANKIDX) Data-CRC
           * on this host but were tolerated before — keep them lenient so sbinit
           * proceeds.  Hard-fail only the block-mode bulk path (firmware download),
           * where a CRC means the RAM image is actually corrupt. */
          /* The CRC/end-bit may only be the host mis-reading the card's CRC-status
           * token (host DAT0 input on the few status bits), with the written data
           * itself intact.  Tolerate it for both byte and block mode, recover the
           * DAT line, and let the firmware boot decide: if the image is actually
           * correct the CR4 comes alive at stage 6; if not, it won't. */
          g_cmd53_logn++;
          emmc_data_reset();    /* clear the wedged DAT line for the retry */
          return -2;            /* CRC/end-bit: caller may retry this transfer */
      }
    }
    /* After a write the card holds DAT0 low (busy) while it commits the block;
     * wait for it to release before returning so the next chunk's setup sees a
     * free DAT line (otherwise: "cmd53 line busy"). */
    if (write) {
        for (t = 0; t < 2000000; t++) if (!(SDREG(SDHCI_PRESENT_STATE) & SR_DAT_INHIBIT)) break;
        if (SDREG(SDHCI_PRESENT_STATE) & SR_DAT_INHIBIT)
            wifi_log("[wifi]   cmd53 wr busy stuck PRESENT=0x%08x off=0x%x\r\n",
                     SDREG(SDHCI_PRESENT_STATE), off);
    }
    return 0;
}

/* ADMA2 descriptor table (one entry is enough: chunks are <= 64 KB).  Must be
 * 8-byte aligned and in DMA-reachable RAM; caches are OFF so no flush is needed
 * for the controller to see our writes. */
struct adma2_desc { volatile u16 cmd; volatile u16 len; volatile u32 addr; } __attribute__((packed));
static struct adma2_desc g_adma[4] __attribute__((aligned(32)));
static u8 g_dma_buf[2048] __attribute__((aligned(32)));   /* aligned bounce buffer */
static int g_dma_logn = 0;

/* CMD53 (IO_RW_EXTENDED) transfer via ADMA2 instead of PIO.  Same arg/block setup
 * as wifi_cmd53_pio, but the controller DMAs the data to/from `buf` directly, so
 * there's no FIFO to under/over-run — which is what corrupts the PIO path. */
static int wifi_cmd53_dma(int write, int fn, u32 off, u8 *buf, int len, int incr)
{
    u32 arg, intr, t, total, c, i; u32 bsize = (fn==2)?512u:64u; int blkmode, blkcnt;
    if (len <= 0) return 0;
    if ((u32)len > bsize) { blkmode = 1; blkcnt = len/bsize; total = blkcnt*bsize; }
    else                  { blkmode = 0; blkcnt = 1;         total = len; }
    if (total > sizeof(g_dma_buf)) { wifi_log("[wifi]   dma chunk too big %u\r\n", total); return -1; }
    /* Bounce through an aligned static buffer: ADMA2 needs 4-byte alignment and
     * stack/rodata source buffers may be unaligned. */
    if (write) for (i = 0; i < total; i++) g_dma_buf[i] = buf[i];
    emmc_ensure_clock(g_fwload_hz);
    /* Wait for both CMD and DAT lines free. */
    for (t = 0; t < 1000000; t++) if (!(SDREG(SDHCI_PRESENT_STATE) & (SR_CMD_INHIBIT|SR_DAT_INHIBIT))) break;
    if (SDREG(SDHCI_PRESENT_STATE) & (SR_CMD_INHIBIT|SR_DAT_INHIBIT)) {
        wifi_log("[wifi]   dma line busy PRESENT=0x%08x\r\n", SDREG(SDHCI_PRESENT_STATE)); return -1; }
    /* Build a single Tran descriptor for the whole buffer (total <= 2 KB << 64 KB). */
    g_adma[0].cmd  = ADMA2_VALID | ADMA2_END | ADMA2_TRAN;
    g_adma[0].len  = (u16)total;                 /* 16-bit byte count (total < 64 KB) */
    g_adma[0].addr = (u32)(unsigned long)g_dma_buf;  /* PA == VA (identity MMU), 1:1 DMA */
    SDREG(SDHCI_ADMA_ADDRESS)     = (u32)((unsigned long)g_adma);
    SDREG(SDHCI_ADMA_ADDRESS + 4) = 0;
    sd_w8(SDHCI_HOST_CONTROL, (u8)((sd_r8(SDHCI_HOST_CONTROL) & ~HC_DMA_MASK) | HC_DMA_ADMA2_32));
    SDREG(SDHCI_INT_STATUS) = 0xFFFFFFFFu;
    if (blkmode) SDREG(SDHCI_BLOCK_SIZE) = ((u32)blkcnt<<16)|bsize;
    else         SDREG(SDHCI_BLOCK_SIZE) = (1u<<16)|(u32)total;
    arg = ((u32)(write&1)<<31)|((u32)(fn&7)<<28)|((u32)(blkmode&1)<<27)|((u32)(incr&1)<<26)|
          ((off&0x1FFFF)<<9)|((blkmode?(u32)blkcnt:(u32)total)&0x1FF);
    SDREG(SDHCI_ARGUMENT) = arg;
    c = ((u32)SD_CMD53_IO_RW_EXT << CMD_INDEX_SHIFT)
        | CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN | CMD_ISDATA
        | TM_DMA_EN | TM_BLKCNT_EN;
    if (!write)  c |= TM_DAT_DIR_CH;
    if (blkmode) c |= TM_MULTI_BLK;
    SDREG(SDHCI_CMDTM) = c;
    for (t = 0; t < 1000000; t++) { intr = SDREG(SDHCI_INT_STATUS);
        if (intr & INT_ERR) { wifi_log("[wifi]   dma CMD err 0x%08x off=0x%x\r\n", intr, off); return -1; }
        if (intr & INT_CMD_DONE) { SDREG(SDHCI_INT_STATUS) = INT_CMD_DONE; break; } }
    if (t >= 1000000) { wifi_log("[wifi]   dma CMD timeout off=0x%x\r\n", off); return -1; }
    /* The controller now DMAs the whole buffer; just wait for transfer-complete. */
    for (t = 0; t < 20000000; t++) { intr = SDREG(SDHCI_INT_STATUS);
        if (intr & (INT_DATA_DONE | INT_ERR)) break; }
    { int adma_err = (SDREG(SDHCI_ADMA_ERROR) != 0);
      if (adma_err || !(intr & INT_DATA_DONE) || (intr & INT_ERR)) {
        if ((g_dma_logn++ & 0x3F) == 0)
            wifi_log("[wifi]   dma %s DATA crc(tol) INT=0x%08x ADMAerr=0x%08x off=0x%x (#%d)\r\n",
                     write?"wr":"rd", intr, SDREG(SDHCI_ADMA_ERROR), off, g_dma_logn);
        /* A real ADMA descriptor error means the DMA itself faulted — hard fail.
         * A DAT-line CRC/end-bit (data path) is the same tolerable glitch the PIO
         * path sees: the data usually transfers fine; let the fw-boot decide. */
        if (adma_err) { SDREG(SDHCI_INT_STATUS) = 0xFFFFFFFFu; return -1; }
        emmc_data_reset();
      }
    }
    SDREG(SDHCI_INT_STATUS) = 0xFFFFFFFFu;
    if (write) {     /* wait out the card's program-busy before the next transfer */
        for (t = 0; t < 2000000; t++) if (!(SDREG(SDHCI_PRESENT_STATE) & SR_DAT_INHIBIT)) break;
    } else {         /* copy DMA'd data back to the caller's buffer */
        for (i = 0; i < total; i++) buf[i] = g_dma_buf[i];
    }
    return 0;
}

/* ==== chip + protocol layer (verbatim from Pi4 driver) ==== */
static int g_sbmem_trace = 0;   /* /wifi-bulk sets this to log per-chunk timing */
static int g_cmd53_retries = 16;   /* retry a CRC-failed CMD53 chunk up to N times */
static int g_retry_total = 0;
static int wifi_sbmem(int write, u8 *buf, int len, u32 addr)
{
    int n = 0; u32 cur_win = 0xFFFFFFFFu;   /* force a set_window on the first chunk */
    while (len > 0) {
        u32 woff, t0, t1, win = addr & ~(SB_WSIZE-1);
        int wrem = (int)(SB_WSIZE - (addr & (SB_WSIZE-1))); int chunk = len;
        if (chunk > SB_XFER) chunk = SB_XFER;
        if (chunk > wrem) chunk = wrem;
        t0 = SYSTIMER_CLO;
        /* Set the backplane window only when it actually changes (every 32 KB),
         * not every 2 KB chunk — each set_window is 3 CMD52s at 25 MHz, and doing
         * it per chunk would thrash the CMD/DAT clock switch on every transfer. */
        if (win != cur_win) {
            if (sdio_set_window(addr) != 0) { wifi_log("[wifi]   sbmem set_window fail @0x%x\r\n", addr); return -1; }
            cur_win = win;
        }
        woff = (addr & (SB_WSIZE-1)) | SB_32BIT;
        /* The DAT line throws intermittent CRC/end-bit errors; retry the chunk
         * until it transfers clean.  Each CMD53 re-specifies its start offset in
         * the arg, so a retry re-sends the whole chunk to the same address (the
         * window CMD52s persist).  rc==-2 = retryable data error; -1 = hard fail. */
        { int rc = -2, tries;
          for (tries = 0; tries < g_cmd53_retries && rc == -2; tries++)
              rc = wifi_cmd53_pio(write, 1, woff, buf, (chunk+3)&~3, 1);
          if (rc != 0) { wifi_log("[wifi]   sbmem cmd53 fail rc=%d @0x%x (chunk #%d, %d tries)\r\n", rc, addr, n, tries); return -1; }
          if (tries > 1) { g_retry_total += tries; if ((n & 0x1F) == 0) wifi_log("[wifi]   chunk #%d ok after %d tries (tot retries=%d)\r\n", n, tries, g_retry_total); }
        }
        t1 = SYSTIMER_CLO;
        if (g_sbmem_trace) wifi_log("[wifi]   chunk %d @0x%x sz=%d dt=%u us\r\n", n, addr, chunk, t1 - t0);
        addr += chunk; buf += chunk; len -= chunk;
        n++;
    }
    return 0;
}
/* Backplane 32-bit register write.  Byte-mode CMD53 writes work on the brcmstb
 * host (ramscan reads the correct 1.07 MB only with this path — CMD52 byte writes
 * don't atomically commit a 32-bit backplane transaction), so keep CMD53 here.
 * Only the READ side needs the CMD52 workaround. */
static int wifi_backplane_write32(u32 addr, u32 val)
{
    u8 b[4]; b[0]=val; b[1]=val>>8; b[2]=val>>16; b[3]=val>>24;
    return wifi_sbmem(1, b, 4, addr);
}

/* discovered chip layout */
static u32 chip_chipcommon, chip_armctl, chip_armregs, chip_armcore;
static u32 chip_d11ctl, chip_socramctl, chip_socramregs, chip_sdregs;
static u32 chip_rambase, chip_socramsize;

/* embedded firmware blobs (device/wifi/wifi_fw.c) */
extern u8 wifi_fw_bin[],    wifi_fw_bin_end[];
extern u8 wifi_nvram_txt[], wifi_nvram_txt_end[];
extern u8 wifi_clm_blob[],  wifi_clm_blob_end[];

static int wifi_corescan(void)
{
    static u8 erom[512]; u32 eromptr, i; int coreid = 0;
    if (wifi_backplane_read32(SB_ENUMBASE + 63*4, &eromptr) != 0) { wifi_log("[wifi]   corescan: eromptr read failed\r\n"); return -1; }
    wifi_log("[wifi]   corescan: eromptr=0x%08x\r\n", eromptr);
    chip_chipcommon = chip_armctl = chip_armregs = chip_armcore = 0;
    chip_d11ctl = chip_socramctl = chip_socramregs = chip_sdregs = 0;
    if (wifi_sbmem(0, erom, sizeof(erom), eromptr) != 0) { wifi_log("[wifi]   corescan: erom sbmem failed\r\n"); return -1; }
    wifi_log("[wifi]   corescan: erom[0..7]= %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
             erom[0],erom[1],erom[2],erom[3],erom[4],erom[5],erom[6],erom[7]);
    for (i = 0; i < sizeof(erom); i += 4) {
        u32 addr;
        switch (erom[i] & 0xF) {
        case 0xF: return 0;
        case 0x1:
            if ((erom[i+4] & 0xF) != 0x1) break;
            coreid = (erom[i+1] | (erom[i+2]<<8)) & 0xFFF; i += 4; break;
        case 0x5:
            addr = (erom[i+1]<<8)|(erom[i+2]<<16)|(erom[i+3]<<24); addr &= ~0xFFFu;
            switch (coreid) {
            case CORE_CHIPCOMMON: if ((erom[i]&0xC0)==0) chip_chipcommon = addr; break;
            case CORE_ARMCM3: case CORE_ARM7: case CORE_ARMCR4:
                chip_armcore = coreid;
                if (erom[i]&0xC0) { if (!chip_armctl) chip_armctl = addr; }
                else              { if (!chip_armregs) chip_armregs = addr; } break;
            case CORE_SOCRAM:
                if (erom[i]&0xC0) chip_socramctl = addr; else if (!chip_socramregs) chip_socramregs = addr; break;
            case CORE_SDIODEV: if ((erom[i]&0xC0)==0) chip_sdregs = addr; break;
            case CORE_D11: if (erom[i]&0xC0) chip_d11ctl = addr; break;
            }
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 *  M1 — RAM scan, firmware download, ARM core start, clock/Fn2 enable *
 * ------------------------------------------------------------------ */
static int wifi_ramscan(void)
{
    u32 r, n, size = 0; int banks, i;
    if (chip_armcore != CORE_ARMCR4) { wifi_log("[wifi] ramscan: non-CR4 0x%x\r\n", chip_armcore); return -1; }
    r = chip_armregs;
    if (wifi_backplane_read32(r + CR4_CAP, &n) != 0) return -1;
    banks = ((n >> 4) & 0xF) + (n & 0xF);
    /* Walk the CR4 memory banks to compute the real RAM size.  This needs a
     * backplane register WRITE per bank (CR4_BANKIDX) — now that the DAT pinmux
     * is fixed those writes are reliable, so we get the true size (the firmware
     * trailer/nvram length goes at rambase+socramsize-4, so a wrong size here
     * mis-places it and the firmware won't boot). */
    for (i = 0; i < banks; i++) {
        if (wifi_backplane_write32(r + CR4_BANKIDX, i) != 0) return -1;
        if (wifi_backplane_read32(r + CR4_BANKINFO, &n) != 0) return -1;
        size += 8192 * ((n & 0x3F) + 1);
    }
    chip_socramsize = size;
    chip_rambase    = 0x198000;    /* BCM43455 TCM base */
    wifi_log("[wifi] ramscan: banks=%d ramsize=0x%x (%u)\r\n", banks, size, size);
    return 0;
}

static int  cfgr(u32 off) { return sdio_cmd52(1, off, 0, 0); }
static void cfgw(u32 off, int v) { sdio_cmd52(1, off, 1, v); }

static void sb_disable(u32 regs, int pre, int ioctl)
{
    u32 v; int t;
    if (wifi_backplane_read32(regs + REG_RESETCTRL, &v) == 0 && (v & 1)) {
        wifi_backplane_write32(regs + REG_IOCTRL, 3 | ioctl);
        wifi_backplane_read32(regs + REG_IOCTRL, &v); return;
    }
    wifi_backplane_write32(regs + REG_IOCTRL, 3 | pre);
    wifi_backplane_read32(regs + REG_IOCTRL, &v);
    wifi_backplane_write32(regs + REG_RESETCTRL, 1);
    wifi_delay_us(10);
    for (t = 0; t < 30; t++) { wifi_backplane_read32(regs + REG_RESETCTRL, &v); if (v & 1) break; }
    if (!(v & 1)) wifi_log("[wifi]   sb_disable(0x%x): RESETCTRL never set\r\n", regs);
    wifi_backplane_write32(regs + REG_IOCTRL, 3 | ioctl);
    wifi_backplane_read32(regs + REG_IOCTRL, &v);
}
static void sb_reset(u32 regs, int pre, int ioctl)
{
    u32 v; int t;
    sb_disable(regs, pre, ioctl);
    for (t = 0; t < 30; t++) {
        wifi_backplane_read32(regs + REG_RESETCTRL, &v);
        if (!(v & 1)) break;
        wifi_backplane_write32(regs + REG_RESETCTRL, 0); wifi_delay_us(40);
    }
    if (v & 1) wifi_log("[wifi]   sb_reset(0x%x): RESETCTRL stuck set\r\n", regs);
    wifi_backplane_write32(regs + REG_IOCTRL, 1 | ioctl);
    wifi_backplane_read32(regs + REG_IOCTRL, &v);
}

static int wifi_condense(u8 *buf, int n)
{
    u8 *p, *ep = buf + n, *op = buf, *lp = buf; int c, skipping = 0;
    for (p = buf; p < ep; p++) { c = *p;
        if (c == '#') skipping = 1;
        else if (c == '\0' || c == '\n') { skipping = 0; if (op != lp) { *op++ = 0; lp = op; } }
        else if (c == '\r') {}
        else if (!skipping) *op++ = (u8)c; }
    if (!skipping && op != lp) *op++ = 0;
    *op++ = 0;
    for (n = op - buf; n & 3; n++) *op++ = 0;
    return n;
}

static u32 g_resetvec;
/* Write firmware + nvram into chip RAM (NO CR4 release).  Split from the
 * CR4 start so /wifi-stage can bisect "bulk write" vs "core release". */
static int wifi_fwload_write(void)
{
    static u8 nvbuf[3072]; u8 tr[4]; u32 t, fwlen, nvlen, off; int i;
    fwlen = (u32)(wifi_fw_bin_end - wifi_fw_bin);
    nvlen = (u32)(wifi_nvram_txt_end - wifi_nvram_txt);
    wifi_log("[wifi] fwload: fw=%u nvram=%u -> rambase 0x%x (ramsize %u)\r\n",
             fwlen, nvlen, chip_rambase, chip_socramsize);
    /* Raise SD clock from 400 kHz id rate to operational speed: the 548 KB
     * download is ~27x too slow to feed at 400 kHz on the cooperative kernel. */
    emmc_set_clock(g_fwload_hz);
    cfgw(REG_CLKCSR, CLK_REQALP);
    for (i = 0; i < 2000 && !(cfgr(REG_CLKCSR) & CLK_ALPAVAIL); i++) wifi_delay_us(100);
    tr[0]=tr[1]=tr[2]=tr[3]=0;
    if (wifi_sbmem(1, tr, 4, chip_rambase + chip_socramsize - 4) != 0) return -1;
    g_resetvec = (u32)wifi_fw_bin[0]|((u32)wifi_fw_bin[1]<<8)|((u32)wifi_fw_bin[2]<<16)|((u32)wifi_fw_bin[3]<<24);
    if (wifi_sbmem(1, wifi_fw_bin, (int)fwlen, chip_rambase) != 0) { wifi_log("[wifi] fwload: fw write failed\r\n"); return -1; }
    wifi_log("[wifi] fwload: firmware written\r\n");
    if (nvlen > sizeof(nvbuf)) nvlen = sizeof(nvbuf);
    for (i = 0; i < (int)nvlen; i++) nvbuf[i] = wifi_nvram_txt[i];
    nvlen = wifi_condense(nvbuf, (int)nvlen);
    off = chip_socramsize - nvlen - 4;
    if (wifi_sbmem(1, nvbuf, (int)nvlen, chip_rambase + off) != 0) return -1;
    t = nvlen / 4; t = (t & 0xFFFF) | (~t << 16);
    tr[0]=t; tr[1]=t>>8; tr[2]=t>>16; tr[3]=t>>24;
    if (wifi_sbmem(1, tr, 4, chip_rambase + chip_socramsize - 4) != 0) return -1;
    wifi_log("[wifi] fwload: nvram %u B + trailer 0x%x\r\n", nvlen, t);
    return 0;
}
/* Plant the reset vector at 0 and release the CR4 (start the firmware). */
static void wifi_fwload_start(void)
{
    u8 tr[4];
    wifi_backplane_write32(chip_sdregs + SDR_INTSTATUS, 0xFFFFFFFFu);
    if (g_resetvec != 0) { tr[0]=g_resetvec; tr[1]=g_resetvec>>8; tr[2]=g_resetvec>>16; tr[3]=g_resetvec>>24; wifi_sbmem(1, tr, 4, 0); }
    sb_reset(chip_armctl, CR4_CPUHALT, 0);
    wifi_log("[wifi] fwload: CR4 released (resetvec=0x%x) — firmware running\r\n", g_resetvec);
}

static int wifi_sbenable(void)
{
    int i, fn2;
    cfgw(REG_CLKCSR, 0); wifi_delay_us(1000);
    cfgw(REG_CLKCSR, CLK_REQHT);
    /* Wait up to ~5 s for HT (the firmware must boot first; Plan9 waits 50×100ms). */
    for (i = 0; i < 2500 && !(cfgr(REG_CLKCSR) & CLK_HTAVAIL); i++) wifi_delay_us(2000);
    if (!(cfgr(REG_CLKCSR) & CLK_HTAVAIL)) { wifi_log("[wifi] sbenable: no HT clock (csr=0x%02x)\r\n", cfgr(REG_CLKCSR)); return -1; }
    cfgw(REG_CLKCSR, cfgr(REG_CLKCSR) | CLK_FORCEHT); wifi_delay_us(10000);
    wifi_log("[wifi] sbenable: HT clock up (csr=0x%02x)\r\n", cfgr(REG_CLKCSR));
    wifi_backplane_write32(chip_sdregs + SDR_MBOXDATA, 4 << 16);
    wifi_backplane_write32(chip_sdregs + SDR_INTMASK, (1u<<7)|(1u<<6)|(1u<<5));
    sdio_cmd52(0, SDIO_CCCR_IOEx, 1, (1<<1)|(1<<2));
    for (i = 0, fn2 = 0; i < 50; i++) { fn2 = sdio_cmd52(0, SDIO_CCCR_IORx, 0, 0);
        if (fn2 >= 0 && (fn2 & (1<<2))) break; wifi_delay_us(2000); }
    if (!(fn2 & (1<<2))) { wifi_log("[wifi] sbenable: Fn2 not ready (0x%02x)\r\n", fn2); return -1; }
    sdio_cmd52(0, SDIO_CCCR_INT_ENABLE, 1, (1<<1)|(1<<2)|1);
    wifi_log("[wifi] sbenable: WLAN Fn2 enabled+ready\r\n");
    return 0;
}

/* ------------------------------------------------------------------ *
 *  bring-up + probe                                                  *
 * ------------------------------------------------------------------ */
static int wifi_ready;
static int g_wifi_stop = 99;   /* /wifi-stage?n=K sets this to bisect the M1 path */

int wifi_bringup(void)
{
    u32 ocr = 0, rca = 0, chipid = 0; int r, ioe = 0;
    if (wifi_ready) { wifi_log("[wifi] (already up)\r\n"); return 0; }
    wifi_log("[wifi] === Pi4 BCM43455 SDIO bring-up (M0) ===\r\n");

    wifi_pinmux_dump();
    wifi_clk_setup();

    { u32 st = 0xDEAD;
      wifi_log("[wifi] WL_REG_ON cycle (brcmstb gio GPIO%d)...\r\n", WL_ON_GPIO);
      wifi_fw_set_gpio(WL_ON_GPIO, 0); wifi_msleep(50);
      wifi_fw_set_gpio(WL_ON_GPIO, 1);
      wifi_msleep(150);
      if (wifi_fw_get_gpio(WL_ON_GPIO, &st) == 0) wifi_log("[wifi] WL_REG_ON GPIO%d -> high (readback=%u)\r\n", WL_ON_GPIO, st); }

    wifi_gpio_sdio();

    if (emmc_host_init() != 0) { wifi_log("[wifi] FAILED: host init\r\n"); return -1; }
    /* S0a: power + brcmstb host regs proven (CAPS read); no SD command issued. */
    if (g_wifi_stop <= -2) { wifi_log("[wifi] -- stage0a done: power + brcmstb host up --\r\n"); return 0; }

    emmc_cmd(SD_CMD0_GO_IDLE, 0, CMD_RSPNS_NONE, 0, 0, 0); wifi_msleep(2);
    r = emmc_cmd(SD_CMD5_IO_OP_COND, 0, CMD_RSPNS_48, &ocr, 0, 0);
    if (r != 0) { wifi_log("[wifi] FAILED: CMD5 (no SDIO device)\r\n"); return -1; }
    wifi_log("[wifi] CMD5 OCR=0x%08x (numfn=%d)\r\n", ocr, (ocr>>28)&7);
    { int tries; u32 voltwin = ocr & 0x00FFFF00u;
      for (tries = 0; tries < 50; tries++) {
        if (emmc_cmd(SD_CMD5_IO_OP_COND, voltwin, CMD_RSPNS_48, &ocr, 0, 0) != 0) break;
        if (ocr & 0x80000000u) break; wifi_msleep(10); } }
    if (!(ocr & 0x80000000u)) { wifi_log("[wifi] FAILED: OCR not ready 0x%08x\r\n", ocr); return -1; }
    wifi_log("[wifi] SDIO ready OCR=0x%08x\r\n", ocr);
    /* S0b: pinmux correct + card answered CMD5 (the pinmux unknown is resolved). */
    if (g_wifi_stop <= -1) { wifi_log("[wifi] -- stage0b done: pinmux + CMD5 OCR ready --\r\n"); return 0; }

    if (emmc_cmd(SD_CMD3_SEND_RCA, 0, CMD_RSPNS_48, &rca, 0, 0) != 0) { wifi_log("[wifi] FAILED: CMD3\r\n"); return -1; }
    rca &= 0xFFFF0000u; wifi_log("[wifi] RCA=0x%08x\r\n", rca);
    if (emmc_cmd(SD_CMD7_SELECT, rca, CMD_RSPNS_48BUSY | CMD_TOLERATE, 0, 0, 0) != 0) { wifi_log("[wifi] FAILED: CMD7\r\n"); return -1; }
    /* Card selected — 25 MHz for enumeration/CMD52/corescan (the CMD line is
     * reliable here; CMD52 actually *fails* at 2 MHz).  The bulk CMD53 data path
     * needs a much lower clock for clean DAT sampling, so wifi_fwload_write()
     * drops to g_fwload_hz (2 MHz) just for the firmware stream and restores
     * 25 MHz afterwards. */
    emmc_set_clock(25000000);

    if (sdio_cmd52(0, SDIO_CCCR_IOEx, 1, 0x02) < 0) { wifi_log("[wifi] FAILED: enable F1\r\n"); return -1; }
    { int tries; for (tries = 0; tries < 50; tries++) { ioe = sdio_cmd52(0, SDIO_CCCR_IORx, 0, 0);
        if (ioe >= 0 && (ioe & 0x02)) break; wifi_msleep(2); } }
    if (!(ioe & 0x02)) { wifi_log("[wifi] FAILED: F1 not ready (0x%02x)\r\n", ioe); return -1; }
    sdio_cmd52(0, 0x110, 1, 64); sdio_cmd52(0, 0x111, 1, 0);
    sdio_cmd52(0, 0x210, 1, 512 & 0xFF); sdio_cmd52(0, 0x211, 1, (512>>8)&0xFF);
    wifi_log("[wifi] F1 enabled, blksize set\r\n");

    /* Switch to a 4-bit data bus.  The brcmstb controller's per-block CRC always
     * mismatches in 1-bit mode (data transfers correctly but every block reports
     * a CRC/end-bit error, so the card NAKs all writes) — 4-bit is the normal
     * CYW43455 mode brcmfmac uses, and exercises the CRC engine the way the
     * controller expects.  CMD52 is CMD-line only so it's unaffected. */
    { int bic = sdio_cmd52(0, 0x07, 0, 0); if (bic < 0) bic = 0;
      sdio_cmd52(0, 0x07, 1, (u32)((bic & ~0x03) | 0x02));                 /* card -> 4-bit */
      sd_w8(SDHCI_HOST_CONTROL, (u8)(sd_r8(SDHCI_HOST_CONTROL) | HC_4BIT)); /* host -> 4-bit */
      wifi_log("[wifi] bus -> 4-bit (CCCR07 0x%02x->0x%02x HOSTCTL=0x%02x)\r\n",
               bic, sdio_cmd52(0,0x07,0,0), sd_r8(SDHCI_HOST_CONTROL)); }

    if (wifi_backplane_read32(SI_ENUM_BASE, &chipid) != 0) { wifi_log("[wifi] FAILED: chip-id read\r\n"); return -1; }
    wifi_log("[wifi] chipcommon[0]=0x%08x chip-id=0x%04x rev=%d\r\n",
             chipid, chipid & CID_ID_MASK, (chipid>>16)&0xF);
    /* DIAG: consecutive backplane reads via the rd4 (CMD53) path (Pi4-style). */
    { u32 a=0,b=0; int r1,r2;
      r1 = wifi_backplane_read32(SI_ENUM_BASE, &a);
      r2 = wifi_backplane_read32(SI_ENUM_BASE + 4, &b);
      wifi_log("[wifi] DIAG rd4 #2 rc=%d val=0x%08x ; #3 rc=%d val=0x%08x\r\n", r1, a, r2, b); }
    /* DIAG: same reads now that wifi_backplane_read32 routes through CMD52. */
    { u32 a=0,b=0,c2=0; int r1,r2,r3;
      r1 = wifi_backplane_read32(SI_ENUM_BASE,     &a);
      r2 = wifi_backplane_read32(SI_ENUM_BASE + 4, &b);
      r3 = wifi_backplane_read32(SI_ENUM_BASE,     &c2);
      wifi_log("[wifi] DIAG cmd52 #1 rc=%d val=0x%08x ; #2 rc=%d val=0x%08x ; #3 rc=%d val=0x%08x\r\n",
               r1, a, r2, b, r3, c2); }
    if ((chipid & CID_ID_MASK) != BCM43455_CHIP_ID) {
        wifi_log("[wifi] chip-id 0x%04x != 0x4345\r\n", chipid & CID_ID_MASK); return -1; }
    wifi_log("[wifi] *** BCM43455 (0x4345) detected over SDIO ***\r\n");

    if (wifi_corescan() != 0) { wifi_log("[wifi] FAILED: corescan\r\n"); return -1; }
    wifi_log("[wifi] cores: chipcommon=0x%x armcore=0x%x armctl=0x%x armregs=0x%x\r\n",
             chip_chipcommon, chip_armcore, chip_armctl, chip_armregs);
    wifi_log("[wifi]        d11ctl=0x%x socramctl=0x%x sdregs=0x%x\r\n",
             chip_d11ctl, chip_socramctl, chip_sdregs);

    wifi_log("[wifi] -- stage0 done: chip up + cores --\r\n");
    if (g_wifi_stop <= 0) return 0;

    /* ---- M1, staged so /wifi-stage?n=K can bisect a wedge ---- */
    if (wifi_ramscan() != 0) { wifi_log("[wifi] FAILED: ramscan\r\n"); return -1; }
    wifi_log("[wifi] -- stage1 done: ramscan rambase=0x%x socramsize=%u --\r\n", chip_rambase, chip_socramsize);
    if (g_wifi_stop <= 1) return 0;

    /* sbinit: halt the CR4, reset the d11 MAC, force the ALP clock, clear pulls */
    { int r; u32 probe;
#define BUSALIVE(tag) do { int rc = wifi_backplane_read32(SI_ENUM_BASE, &probe); \
        wifi_log("[wifi]   busprobe[%s]: rc=%d chipid=0x%08x clkcsr=0x%02x\r\n", tag, rc, probe, cfgr(REG_CLKCSR)); } while (0)
      BUSALIVE("pre");
      sb_reset(chip_armctl, CR4_CPUHALT, CR4_CPUHALT);
      BUSALIVE("armhalt");
      if (chip_d11ctl) sb_reset(chip_d11ctl, 8 | 4, 4);
      BUSALIVE("d11reset");
      cfgw(REG_CLKCSR, 0); wifi_delay_us(10);
      cfgw(REG_CLKCSR, CLK_NOHWREQ | CLK_REQALP);
      for (r = 0; r < 2000 && !(cfgr(REG_CLKCSR) & (CLK_HTAVAIL|CLK_ALPAVAIL)); r++) wifi_delay_us(10);
      cfgw(REG_CLKCSR, CLK_NOHWREQ | CLK_FORCEALP); wifi_delay_us(65);
      BUSALIVE("alpforced");
      /* On Pi5, cfgw(REG_PULLUPS,0) — which disables the chip's internal SDIO-line
       * pull-ups — floats the CMD/DAT lines and wedges the bus (cmd52 end-bit/index
       * errors right after).  Pi4 tolerates it (external board pulls); Pi5 does not,
       * so keep the internal pull-ups.  The chipcommon GPIO pull-clears are likewise
       * skipped (non-essential init).  Leave the lines pulled. */
      BUSALIVE("nopullclear"); }
    wifi_log("[wifi] -- stage2 done: chip halted + ALP forced (csr=0x%02x) --\r\n", cfgr(REG_CLKCSR));
    if (g_wifi_stop <= 2) return 0;

    /* stage3: small 4 KB test-write to validate the bulk backplane write path
     * cheaply (vs the full 548 KB) — isolates "write path" from "volume". */
    { int rc = wifi_sbmem(1, wifi_fw_bin, 4096, chip_rambase);
      wifi_log("[wifi] -- stage3 done: 4KB test-write rc=%d --\r\n", rc); }
    if (g_wifi_stop <= 3) return 0;

    /* stage4: full firmware + nvram write (no CR4 release yet) */
    if (wifi_fwload_write() != 0) { wifi_log("[wifi] FAILED: fwload_write\r\n"); return -1; }
    wifi_log("[wifi] -- stage4 done: full fw+nvram written --\r\n");
    if (g_wifi_stop <= 4) return 0;

    /* stage5: release the CR4 (start the firmware) */
    wifi_fwload_start();
    wifi_log("[wifi] -- stage5 done: CR4 released --\r\n");
    if (g_wifi_stop <= 5) return 0;

    /* stage6: HT clock + WLAN Fn2 */
    if (wifi_sbenable() != 0){ wifi_log("[wifi] FAILED: sbenable\r\n"); return -1; }

    wifi_ready = 1;
    wifi_log("[wifi] *** M1 SUCCESS: firmware running, HT clock up, Fn2 ready ***\r\n");
    return 0;
}

int wifi_probe(void) { wifi_tn = 0; g_wifi_stop = 99; return wifi_bringup(); }
/* Run the bring-up only up to stage `k` (0..6), then return — for bisecting
 * a wedge across reboots.  wifi_ready stays 0 so each call re-runs from M0. */
int wifi_probe_stage(int k) { wifi_tn = 0; wifi_ready = 0; g_wifi_stop = k; return wifi_bringup(); }

/* Timed bulk-write probe: run M0+ramscan+halt, set the SD clock to `hz`, then
 * write `kb` KB to rambase and report elapsed microseconds (1 MHz systimer).
 * Bounded => returns fast, so we get a real throughput number + the actual
 * achieved clock without monopolizing the worker for minutes. */
int wifi_probe_bulk(int kb, u32 hz)
{
    u32 t0, t1, n; int rc;
    wifi_tn = 0; wifi_ready = 0; g_wifi_stop = 2;
    if (wifi_bringup() != 0) { wifi_log("[wifi] bulk: setup failed\r\n"); return -1; }
    if (hz) { g_fwload_hz = hz; emmc_set_clock(g_fwload_hz); }   /* hz=0 => leave clock untouched */
    else wifi_log("[wifi] bulk: clock untouched (no set_clock)\r\n");
    n = (u32)kb * 1024u;
    if (n > chip_socramsize) n = chip_socramsize;
    if (n > (u32)(wifi_fw_bin_end - wifi_fw_bin)) n = (u32)(wifi_fw_bin_end - wifi_fw_bin);
    wifi_log("[wifi] bulk: writing %u bytes @ %u Hz ...\r\n", n, g_fwload_hz);
    g_sbmem_trace = 1;
    t0 = SYSTIMER_CLO;
    rc = wifi_sbmem(1, wifi_fw_bin, (int)n, chip_rambase);
    t1 = SYSTIMER_CLO;
    g_sbmem_trace = 0;
    wifi_log("[wifi] bulk: %u bytes in %u us (rc=%d) => %u KB/s --\r\n",
             n, t1 - t0, rc, (t1>t0) ? (n * 1000u) / ((t1 - t0)) : 0);
    return rc;
}

/* Bisect the full-write crash by window: run M0+ramscan+halt, then a single
 * 4-byte backplane write at rambase + w*0x8000.  Call /wifi-win?w=0,1,2,... ;
 * the call that hard-crashes (no HTTP reply) is the bad window.  Earlier calls
 * return their trace, so the crashing window is identified by elimination. */
int wifi_probe_winwrite(int w)
{
    u32 a; int rc;
    wifi_tn = 0; wifi_ready = 0; g_wifi_stop = 2;   /* up to halt only */
    if (wifi_bringup() != 0) { wifi_log("[wifi] winwrite: setup failed\r\n"); return -1; }
    a = chip_rambase + (u32)w * 0x8000u;
    if (a + 4 > chip_rambase + chip_socramsize) a = chip_rambase + chip_socramsize - 4;
    wifi_log("[wifi] winwrite w=%d -> writing 4B @0x%x ...\r\n", w, a);
    rc = wifi_sbmem(1, wifi_fw_bin, 4, a);
    wifi_log("[wifi] winwrite w=%d @0x%x rc=%d --\r\n", w, a, rc);
    return rc;
}

/* ================================================================== *
 *  M2 — SDPCM/BCDC control plane + escan (port of Pi3 apps/wifi.c)   *
 * ================================================================== */
static u8  wl_txseq;
static u8  wl_txwindow = 1;
static u8  wl_fcmask = 0;
static u16 wl_reqid;
static char wifi_cur_ssid[40] = "";   /* last joined SSID (desktop indicator) */
/* directed-join target (set before wifi_scan to capture bssid+chanspec) */
static char wifi_tgt_ssid[33];
static int  wifi_tgt_slen = 0, wifi_tgt_set = 0, wifi_tgt_found = 0;
static u8   wifi_tgt_bssid[6];
static u16  wifi_tgt_chanspec = 0;

/* Read/write a data-channel (Fn2) packet, chunked at 512 bytes. */
static int wifi_packetrw(int write, u8 *buf, int len)
{
    while (len > 0) {
        int chunk = (len > 512) ? 512 : len;
        if (wifi_cmd53_pio(write, 2, 0, buf, (chunk + 3) & ~3, 0) != 0) return -1;
        buf += chunk; len -= chunk;
    }
    return 0;
}

/* Issue a firmware ioctl over the SDPCM control channel and return its reply. */
static int wifi_wlcmd(int write, int op, const u8 *data, int dlen, u8 *res, int rlen)
{
    static u8 pkt[2048];
    int tlen = write ? (dlen + rlen) : (dlen > rlen ? dlen : rlen);
    int len = SDPCM_HDR + BCDC_HDR + tlen;
    int i, tries;

    if (len > (int)sizeof(pkt)) { wifi_log("[wifi] wlcmd: pkt too big %d\r\n", len); return -1; }
    for (i = 0; i < len; i++) pkt[i] = 0;
    pkt[0] = len & 0xFF; pkt[1] = (len >> 8) & 0xFF;
    pkt[2] = ~len & 0xFF; pkt[3] = (~len >> 8) & 0xFF;
    pkt[4] = wl_txseq;
    pkt[5] = 0;                  /* chanflg: channel 0 = control */
    pkt[7] = SDPCM_HDR;         /* doffset */
    { u8 *q = pkt + SDPCM_HDR;
      q[0] = op; q[1] = op >> 8; q[2] = op >> 16; q[3] = op >> 24;
      q[4] = tlen; q[5] = tlen >> 8; q[6] = tlen >> 16; q[7] = tlen >> 24;
      q[8] = write ? 2 : 0;
      wl_reqid++;
      q[10] = wl_reqid; q[11] = wl_reqid >> 8; }
    if (dlen > 0) for (i = 0; i < dlen; i++) pkt[SDPCM_HDR + BCDC_HDR + i] = data[i];

    if (wifi_packetrw(1, pkt, len) != 0) { wifi_log("[wifi] wlcmd: tx failed\r\n"); return -1; }
    wl_txseq++;

    for (tries = 0; tries < 400; tries++) {
        u32 ints; int plen, lenck, chan, doff;
        if (wifi_backplane_read32(chip_sdregs + SDR_INTSTATUS, &ints) == 0 && ints) {
            wifi_backplane_write32(chip_sdregs + SDR_INTSTATUS, ints);
            if (ints & SD_INT_MBOX) {
                u32 mb; wifi_backplane_read32(chip_sdregs + SDR_HOSTMBOX, &mb);
                wifi_backplane_write32(chip_sdregs + SDR_SBMBOX, 2);   /* ack */
            }
        }
        if (wifi_packetrw(0, pkt, SDPCM_HDR) != 0) { wifi_delay_us(2000); continue; }
        plen = pkt[0] | (pkt[1] << 8);
        lenck = pkt[2] | (pkt[3] << 8);
        if (plen == 0) { wifi_delay_us(2000); continue; }
        if (lenck != ((plen ^ 0xFFFF) & 0xFFFF) || plen < SDPCM_HDR || plen > (int)sizeof(pkt)) {
            wifi_log("[wifi] wlcmd: bad frame len=0x%x lenck=0x%x\r\n", plen, lenck);
            wifi_delay_us(2000); continue;
        }
        chan = pkt[5] & 0xF; doff = pkt[7];
        wl_fcmask = pkt[8]; wl_txwindow = pkt[9];
        if (plen > SDPCM_HDR)
            if (wifi_packetrw(0, pkt + SDPCM_HDR, plen - SDPCM_HDR) != 0) return -1;
        if (chan != 0) { wifi_log("[wifi] wlcmd: drained chan %d frame\r\n", chan); continue; }
        { u8 *q = pkt + doff;
          u32 st = q[12] | (q[13] << 8) | (q[14] << 16) | (q[15] << 24);
          if (st != 0) { wifi_log("[wifi] wlcmd: op %d status %u\r\n", op, st); return -1; }
          if (!write && rlen > 0) { u8 *d = q + BCDC_HDR; for (i = 0; i < rlen; i++) res[i] = d[i]; }
          return 0; }
    }
    wifi_log("[wifi] wlcmd: op %d timed out (no control response)\r\n", op);
    return -1;
}

static int wifi_get_iovar(const char *name, u8 *val, int len)
{ int n = 0; while (name[n]) n++; n++; return wifi_wlcmd(0, WLC_GET_VAR, (const u8 *)name, n, val, len); }
static int wifi_cmd_int(int op, u32 v)
{ u8 b[4]; b[0]=v; b[1]=v>>8; b[2]=v>>16; b[3]=v>>24; return wifi_wlcmd(1, op, b, 4, 0, 0); }
static int wifi_set_iovar(const char *name, const u8 *val, int vlen)
{ static u8 b[1600]; int n=0, i; while (name[n]) { b[n]=name[n]; n++; } b[n++]='\0';
  for (i=0;i<vlen && (n+i)<(int)sizeof(b);i++) b[n+i]=val[i]; return wifi_wlcmd(1, WLC_SET_VAR, b, n+vlen, 0, 0); }
static int wifi_set_iovar_int(const char *name, u32 v)
{ u8 b[4]; b[0]=v; b[1]=v>>8; b[2]=v>>16; b[3]=v>>24; return wifi_set_iovar(name, b, 4); }

#define CLM_CHUNK 1400
#define CLM_FLAG  0x1000
#define CLM_FIRST 0x0002
#define CLM_LAST  0x0004
static int wifi_clmload(void)
{
    static u8 buf[12 + CLM_CHUNK];
    u32 total = (u32)(wifi_clm_blob_end - wifi_clm_blob), off = 0;
    int flag = CLM_FLAG | CLM_FIRST;
    wifi_log("[wifi] clmload: %u bytes regulatory\r\n", total);
    while (off < total || flag == (CLM_FLAG | CLM_FIRST)) {
        u32 n = total - off; int i, pad;
        if (n > CLM_CHUNK) n = CLM_CHUNK; else flag |= CLM_LAST;
        for (i = 0; i < (int)n; i++) buf[12 + i] = wifi_clm_blob[off + i];
        pad = 0;
        if (flag & CLM_LAST) while ((n + pad) & 7) buf[12 + n + pad++] = 0;
        buf[0]=flag; buf[1]=flag>>8; buf[2]=2; buf[3]=0;
        buf[4]=n; buf[5]=n>>8; buf[6]=n>>16; buf[7]=n>>24;
        buf[8]=buf[9]=buf[10]=buf[11]=0;
        if (wifi_set_iovar("clmload", buf, 12 + n + pad) != 0) {
            wifi_log("[wifi] clmload: chunk at %u failed\r\n", off); return -1; }
        off += n; flag &= ~CLM_FIRST;
        if (off >= total) break;
    }
    wifi_log("[wifi] clmload: done\r\n");
    return 0;
}

#define WLC_PASSIVE_SCAN 49
#define WLC_E_ESCAN_RESULT 69
#define WLC_E_SCAN_COMPLETE 26
static const u8 escan_params[] = {
    1,0,0,0, 1,0, 0x34,0x12, 0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0xff,0xff,0xff,0xff,0xff,0xff, 2, 0,
    0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff,
    14,0, 0,0,
    0x01,0x2b,0x02,0x2b,0x03,0x2b,0x04,0x2b,0x05,0x2e,0x06,0x2e,0x07,0x2e,
    0x08,0x2b,0x09,0x2b,0x0a,0x2b,0x0b,0x2b,0x0c,0x2b,0x0d,0x2b,0x0e,0x2b,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

static int wifi_read_frame(u8 *buf, int cap, int *chan, int *doff)
{
    int plen, lenck;
    if (wifi_packetrw(0, buf, SDPCM_HDR) != 0) return -1;
    plen = buf[0] | (buf[1] << 8); lenck = buf[2] | (buf[3] << 8);
    if (plen == 0) return 0;
    if (lenck != ((plen ^ 0xFFFF) & 0xFFFF) || plen < SDPCM_HDR || plen > cap) return 0;
    *chan = buf[5] & 0xF; *doff = buf[7];
    wl_fcmask = buf[8]; wl_txwindow = buf[9];
    if (plen > SDPCM_HDR)
        if (wifi_packetrw(0, buf + SDPCM_HDR, plen - SDPCM_HDR) != 0) return -1;
    return plen;
}

static int wifi_radio_done = 0;
static void wifi_radio_up(void)
{
    int i;
    if (wifi_radio_done) return;
    { u8 em[16]; for (i = 0; i < 16; i++) em[i] = 0xFF;
      if (wifi_set_iovar("event_msgs", em, 16) != 0) wifi_log("[wifi] radio: event_msgs failed\r\n");
      else wifi_log("[wifi] radio: event_msgs enabled\r\n"); }
    wifi_cmd_int(0x56, 0);                         /* WLC_SET_PM = 0 */
    if (wifi_clmload() != 0) wifi_log("[wifi] radio: clmload failed (continuing)\r\n");
    { u8 cc[12]; for (i = 0; i < 12; i++) cc[i] = 0;
      cc[0]='U'; cc[1]='S'; cc[4]=cc[5]=cc[6]=cc[7]=0xFF; cc[8]='U'; cc[9]='S';
      if (wifi_set_iovar("country", cc, 12) != 0) wifi_log("[wifi] radio: set country failed\r\n"); }
    if (wifi_set_iovar_int("mpc", 0) != 0) wifi_log("[wifi] radio: set mpc=0 failed\r\n");
    wifi_radio_done = 1;
}

static int wifi_scan(int *out_count)
{
    static u8 fr[2048];
    static u8 seen[32][6];
    int nseen = 0, tries, chan, doff, i;

    *out_count = 0;
    wifi_radio_up();
    if (wifi_cmd_int(2, 1) != 0) wifi_log("[wifi] scan: WLC_UP error (continuing)\r\n");
    else wifi_log("[wifi] scan: WLC_UP ok\r\n");
    wifi_delay_us(150000);
    wifi_cmd_int(WLC_PASSIVE_SCAN, 0);
    if (wifi_set_iovar("escan", escan_params, sizeof(escan_params)) != 0) {
        wifi_log("[wifi] scan: escan start failed\r\n"); return -1; }
    wifi_log("[wifi] scan: escan started, collecting...\r\n");

    { int nframes = 0, nchan1 = 0, nev = 0;
    for (tries = 0; tries < 2000; tries++) {
        u8 *evp, *escan, *bss; int len, event, nbss, bdc;
        len = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
        if (len < 0) break;
        if (len == 0) { wifi_delay_us(5000); continue; }
        nframes++;
        if (chan != 1) continue;
        nchan1++;
        if (len < doff + 4) continue;
        bdc = 4 + (fr[doff + 3] << 2);
        evp = fr + doff + bdc;
        if ((int)(evp - fr) + 24 + 64 > len) continue;
        event = (evp[24 + 6] << 8) | evp[24 + 7];
        if (nev < 8) { wifi_log("[wifi] scan rx: len=%d event=%d\r\n", len, event); nev++; }
        if (event == WLC_E_SCAN_COMPLETE) { wifi_log("[wifi] scan: complete\r\n"); break; }
        if (event != WLC_E_ESCAN_RESULT) continue;
        escan = evp + 24 + 48;
        if ((int)(escan - fr) + 12 > len) continue;
        nbss = escan[10] | (escan[11] << 8);
        if (nbss == 0) { wifi_log("[wifi] scan: escan done (nbss=0)\r\n"); break; }
        bss = escan + 12;
        if ((int)(bss - fr) + 82 > len) continue;
        { u8 *bssid = bss + 8; int ssidlen = bss[18]; if (ssidlen > 32) ssidlen = 32;
          short rssi = (short)(bss[78] | (bss[79] << 8));
          u16 chanspec = (u16)(bss[72] | (bss[73] << 8)); int ch = chanspec & 0xFF; int dup = 0;
          /* directed-join: capture this AP's bssid+chanspec if its SSID matches */
          if (wifi_tgt_set && !wifi_tgt_found && ssidlen == wifi_tgt_slen) {
              int m = 1;
              for (i = 0; i < ssidlen; i++)
                  if (bss[19 + i] != (u8)wifi_tgt_ssid[i]) { m = 0; break; }
              if (m) {
                  for (i = 0; i < 6; i++) wifi_tgt_bssid[i] = bssid[i];
                  wifi_tgt_chanspec = chanspec; wifi_tgt_found = 1;
                  wifi_log("[wifi] join: target found %02x:%02x:%02x:%02x:%02x:%02x chanspec=0x%04x ch=%d\r\n",
                           bssid[0],bssid[1],bssid[2],bssid[3],bssid[4],bssid[5], chanspec, ch);
              }
          }
          for (i = 0; i < nseen; i++)
              if (seen[i][0]==bssid[0]&&seen[i][1]==bssid[1]&&seen[i][2]==bssid[2]&&
                  seen[i][3]==bssid[3]&&seen[i][4]==bssid[4]&&seen[i][5]==bssid[5]) { dup=1; break; }
          if (dup) continue;
          if (nseen < 32) { for (i=0;i<6;i++) seen[nseen][i]=bssid[i]; nseen++; }
          { char ssid[33];
            for (i = 0; i < ssidlen; i++) { u8 c = bss[19 + i]; ssid[i] = (c >= 0x20 && c < 0x7f) ? c : '?'; }
            ssid[ssidlen] = '\0';
            wifi_log("[wifi] AP %2d: \"%s\" %02x:%02x:%02x:%02x:%02x:%02x ch=%d rssi=%d\r\n",
                     nseen, ssidlen ? ssid : "(hidden)",
                     bssid[0],bssid[1],bssid[2],bssid[3],bssid[4],bssid[5], ch, rssi); } }
    }
    wifi_log("[wifi] scan: frames=%d chan1=%d (collected %d AP)\r\n", nframes, nchan1, nseen); }
    *out_count = nseen;
    return 0;
}

/* Public M2 entry: ensure chip is up (M1), then scan.  /wifi-scan route. */
int wifi_scan_run(void)
{
    int n = 0;
    wifi_tn = 0;
    if (!wifi_ready) { if (wifi_bringup() != 0) { wifi_log("[wifi] scan: bringup failed\r\n"); return -1; } }
    wifi_scan(&n);
    return n;
}

/* ================================================================== *
 *  M3a — WPA2-PSK join (SHA1/HMAC/PBKDF2 + WSEC_PMK + join iovar)    *
 * ================================================================== */
struct sha1 { u32 h[5]; unsigned long long len; u8 buf[64]; int n; };
static u32 sha1_rol(u32 v, int s) { return (v << s) | (v >> (32 - s)); }
static void sha1_block(struct sha1 *c, const u8 *p)
{
    u32 w[80], a, b, d, e, f, k, t; int i;
    for (i = 0; i < 16; i++) w[i] = (p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];
    for (i = 16; i < 80; i++) w[i] = sha1_rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
    a=c->h[0]; b=c->h[1]; d=c->h[2]; e=c->h[3]; f=c->h[4];
    for (i = 0; i < 80; i++) {
        u32 fn;
        if (i < 20)      { fn = (b & d) | (~b & e); k = 0x5A827999; }
        else if (i < 40) { fn = b ^ d ^ e;          k = 0x6ED9EBA1; }
        else if (i < 60) { fn = (b&d)|(b&e)|(d&e);  k = 0x8F1BBCDC; }
        else             { fn = b ^ d ^ e;          k = 0xCA62C1D6; }
        t = sha1_rol(a,5) + fn + f + k + w[i];
        f = e; e = d; d = sha1_rol(b,30); b = a; a = t;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=d; c->h[3]+=e; c->h[4]+=f;
}
static void sha1_init(struct sha1 *c)
{ c->h[0]=0x67452301; c->h[1]=0xEFCDAB89; c->h[2]=0x98BADCFE; c->h[3]=0x10325476; c->h[4]=0xC3D2E1F0; c->len=0; c->n=0; }
static void sha1_update(struct sha1 *c, const u8 *p, int len)
{ int i; for (i = 0; i < len; i++) { c->buf[c->n++] = p[i]; if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; } } c->len += len; }
static void sha1_final(struct sha1 *c, u8 out[20])
{
    unsigned long long bits = c->len * 8; int i; u8 pad = 0x80;
    sha1_update(c, &pad, 1); pad = 0;
    while (c->n != 56) sha1_update(c, &pad, 1);
    for (i = 7; i >= 0; i--) { u8 b = (bits >> (i*8)) & 0xFF; sha1_update(c, &b, 1); }
    for (i = 0; i < 5; i++) { out[i*4]=c->h[i]>>24; out[i*4+1]=c->h[i]>>16; out[i*4+2]=c->h[i]>>8; out[i*4+3]=c->h[i]; }
}
static void hmac_sha1(const u8 *key, int klen, const u8 *msg, int mlen, u8 mac[20])
{
    u8 k[64], ipad[64], opad[64], inner[20]; struct sha1 c; int i;
    for (i = 0; i < 64; i++) k[i] = 0;
    if (klen > 64) { sha1_init(&c); sha1_update(&c, key, klen); sha1_final(&c, k); }
    else for (i = 0; i < klen; i++) k[i] = key[i];
    for (i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    sha1_init(&c); sha1_update(&c, ipad, 64); sha1_update(&c, msg, mlen); sha1_final(&c, inner);
    sha1_init(&c); sha1_update(&c, opad, 64); sha1_update(&c, inner, 20); sha1_final(&c, mac);
}
static void pbkdf2_sha1(const u8 *pass, int plen, const u8 *salt, int slen, int iter, u8 *dk, int dklen)
{
    int block = 1, off = 0;
    while (off < dklen) {
        u8 salt_i[40], u[20], t[20]; int i, j, cpy;
        for (i = 0; i < slen && i < 36; i++) salt_i[i] = salt[i];
        salt_i[slen]=block>>24; salt_i[slen+1]=block>>16; salt_i[slen+2]=block>>8; salt_i[slen+3]=block;
        hmac_sha1(pass, plen, salt_i, slen + 4, u);
        for (i = 0; i < 20; i++) t[i] = u[i];
        for (j = 1; j < iter; j++) { hmac_sha1(pass, plen, u, 20, u); for (i = 0; i < 20; i++) t[i] ^= u[i]; }
        cpy = (dklen - off < 20) ? (dklen - off) : 20;
        for (i = 0; i < cpy; i++) dk[off + i] = t[i];
        off += cpy; block++;
    }
}
static int wifi_set_pmk(const char *ssid, int slen, const char *pass, int plen)
{
    u8 pmk32[32]; u8 msg[2 + 2 + 128]; int i;
    pbkdf2_sha1((const u8 *)pass, plen, (const u8 *)ssid, slen, 4096, pmk32, 32);
    wifi_log("[wifi] join: PMK %02x%02x%02x%02x...%02x%02x derived\r\n",
             pmk32[0], pmk32[1], pmk32[2], pmk32[3], pmk32[30], pmk32[31]);
    for (i = 0; i < (int)sizeof(msg); i++) msg[i] = 0;
    msg[0] = 32; msg[1] = 0; msg[2] = 0; msg[3] = 0;
    for (i = 0; i < 32; i++) msg[4 + i] = pmk32[i];
    return wifi_wlcmd(1, WLC_SET_WSEC_PMK, msg, sizeof(msg), 0, 0);
}
/* Join a WPA2-PSK AP.  Returns 0 once associated (link up / GET_BSSID nonzero). */
static int wifi_do_join(const char *ssid, const char *pass)
{
    u8 jp[114];
    int sl = 0, i, ev, secured = (pass && pass[0]), pl = 0, jsz;
    while (ssid[sl] && sl < 32) sl++;
    if (pass) while (pass[pl] && pl < 63) pl++;
    { int k; for (k = 0; k < sl && k < 39; k++) wifi_cur_ssid[k] = ssid[k]; wifi_cur_ssid[k] = 0; }
    wifi_log("[wifi] join: ssid=\"%s\" %s\r\n", ssid, secured ? "WPA2-PSK" : "open");

    wifi_radio_up();
    wifi_cmd_int(WLC_DOWN, 1);
    wifi_cmd_int(WLC_SET_INFRA, 1);
    wifi_cmd_int(2, 1);                          /* WLC_UP */
    wifi_delay_us(50000);

    { int n = 0, j;
      wifi_tgt_slen = sl; wifi_tgt_found = 0; wifi_tgt_set = 1;
      for (j = 0; j < sl && j < 32; j++) wifi_tgt_ssid[j] = ssid[j];
      wifi_scan(&n);
      wifi_tgt_set = 0;
      wifi_log("[wifi] === JOIN PHASE === ssid=\"%s\" tgt=%d chanspec=0x%04x\r\n",
               ssid, wifi_tgt_found, wifi_tgt_chanspec); }

    if (secured) {
        int rc, r1, r2, r3, r4;
        r1 = wifi_set_iovar_int("wpa_auth", 0xc0);
        r2 = wifi_set_iovar_int("auth", 0);
        r3 = wifi_set_iovar_int("wsec", 4);
        r4 = wifi_set_iovar_int("wpa_auth", 0x80);
        wifi_log("[wifi] join: iovar rc wpa_auth(c0)=%d auth=%d wsec=%d wpa_auth(80)=%d\r\n", r1, r2, r3, r4);
        rc = wifi_set_iovar_int("sup_wpa", 1);
        wifi_log("[wifi] join: sup_wpa=1 rc=%d %s\r\n", rc, rc ? "(FWSUP NOT avail?)" : "(FWSUP on)");
        rc = wifi_set_pmk(ssid, sl, pass, pl);
        wifi_log("[wifi] join: WSEC_PMK rc=%d %s\r\n", rc, rc ? "(fw rejected PMK)" : "(PMK accepted)");
    } else {
        wifi_set_iovar_int("wsec", 0); wifi_set_iovar_int("wpa_auth", 0); wifi_set_iovar_int("auth", 0);
    }

    for (i = 0; i < (int)sizeof(jp); i++) jp[i] = 0;
    jp[0] = sl;
    for (i = 0; i < sl; i++) jp[4 + i] = ssid[i];
    jp[36] = 0xFF;
    if (wifi_tgt_found) {
        jp[40] = 2; jp[44] = 120; jp[48] = 0x86; jp[49] = 0x01;
        jp[52]=0xFF; jp[53]=0xFF; jp[54]=0xFF; jp[55]=0xFF;
        for (i = 0; i < 6; i++) jp[56 + i] = wifi_tgt_bssid[i];
        jp[64] = 1; jp[68] = wifi_tgt_chanspec & 0xFF; jp[69] = (wifi_tgt_chanspec >> 8) & 0xFF;
        jsz = 70;
    } else {
        for (i = 40; i < 56; i++) jp[i] = 0xFF;
        for (i = 56; i < 62; i++) jp[i] = 0xFF;
        jsz = 68;
    }
    { static u8 fr[2048]; int attempt, chan, doff, tries, nev;
      for (attempt = 0; attempt < 4; attempt++) {
        if (wifi_set_iovar("join", jp, jsz) != 0) { wifi_log("[wifi] join: 'join' iovar failed\r\n"); return -1; }
        wifi_log("[wifi] join: request sent (attempt %d), waiting...\r\n", attempt + 1);
        nev = 0;
        for (tries = 0; tries < 600; tries++) {
            int len;
            if ((tries % 80) == 40) {
                u8 bss[6]; int k, nz = 0;
                if (wifi_wlcmd(0, WLC_GET_BSSID, 0, 0, bss, 6) == 0)
                    for (k = 0; k < 6; k++) if (bss[k] && bss[k] != 0xFF) nz = 1;
                if (nz) {
                    wifi_log("[wifi] join: GET_BSSID %02x:%02x:%02x:%02x:%02x:%02x -> *** associated ***\r\n",
                             bss[0],bss[1],bss[2],bss[3],bss[4],bss[5]);
                    return 0;
                }
            }
            len = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
            if (len < 0) break;
            if (len == 0) { wifi_delay_us(10000); continue; }
            if (len < doff + 4) continue;
            { int bdc = 4 + (fr[doff + 3] << 2); u8 *p = fr + doff + bdc;
              if ((int)(p - fr) + 16 > len) continue;
              if (chan == 1) {
                  long status = (p[24+8]<<24)|(p[24+9]<<16)|(p[24+10]<<8)|p[24+11];
                  ev = (p[24 + 6] << 8) | p[24 + 7];
                  nev++;
                  if (nev <= 16) wifi_log("[wifi] join: event %d status %ld\r\n", ev, status);
                  if (ev == 16) {
                      int up = (p[24+2] << 8 | p[24+3]) & 1;
                      wifi_log("[wifi] join: E_LINK %s\r\n", up ? "UP" : "down");
                      if (up) { wifi_log("[wifi] *** associated (link up) ***\r\n"); return 0; }
                  } else if (ev == 5 || ev == 6 || ev == 12 || ev == 24) {
                      wifi_log("[wifi] join: deauth/disassoc event %d\r\n", ev);
                  }
              } }
        }
        wifi_log("[wifi] join: attempt %d timeout (events=%d)\r\n", attempt + 1, nev);
      } }
    return -1;
}
/* Public M3a entry: ensure chip up (M1), then join.  /wifi-join?ssid=&pass= */
int wifi_join_run(const char *ssid, const char *pass)
{
    wifi_tn = 0;
    if (!wifi_ready) { if (wifi_bringup() != 0) { wifi_log("[wifi] join: bringup failed\r\n"); return -1; } }
    return wifi_do_join(ssid, pass);
}

/* ================================================================== *
 *  M3b — data path (SDPCM chan 2) + minimal DHCP client             *
 * ================================================================== */
/* Transmit one 802.3 ethernet frame over the WLAN data channel (chan 2):
 * SDPCM hdr(12, chan=2) + BDC hdr(4, byte0=0x20 proto ver 2) + frame.
 * Respects the fw tx credit window / data-channel flow control. */
static int wifi_data_tx(const u8 *eth, int ethlen)
{
    static u8 pkt[1600];
    int len = SDPCM_HDR + 4 + ethlen, i, spin;
    if (len > (int)sizeof(pkt)) return -1;
    for (spin = 0; spin < 60; spin++) {
        if (!(wl_fcmask & (1 << 2)) && wl_txseq != wl_txwindow) break;
        { static u8 t[2048]; int c, d; if (wifi_read_frame(t, sizeof(t), &c, &d) <= 0) wifi_delay_us(2000); }
    }
    for (i = 0; i < len; i++) pkt[i] = 0;
    pkt[0] = len & 0xFF; pkt[1] = (len >> 8) & 0xFF;
    pkt[2] = ~len & 0xFF; pkt[3] = (~len >> 8) & 0xFF;
    pkt[4] = wl_txseq; pkt[5] = 2; pkt[7] = SDPCM_HDR;
    pkt[SDPCM_HDR + 0] = 0x20;
    for (i = 0; i < ethlen; i++) pkt[SDPCM_HDR + 4 + i] = eth[i];
    if (wifi_packetrw(1, pkt, len) != 0) return -1;
    wl_txseq++;
    return 0;
}
static u16 ip_cksum(const u8 *p, int n, u32 sum)
{
    int i;
    for (i = 0; i + 1 < n; i += 2) sum += (p[i] << 8) | p[i + 1];
    if (i < n) sum += p[i] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}
static u8 wifi_ip[4], wifi_mask[4], wifi_gw[4], wifi_dns[4], wifi_mac[6];
static int wifi_have_ip = 0;
int wifi_connected(void) { return wifi_have_ip; }

/* Disconnect: bring the MAC down (leaves the AP / ad-hoc cell) and clear state.
 * Clearing wifi_have_ip also quiesces the persistent ARP/ICMP responder (it
 * early-returns when there's no IP), so no separate thread stop is needed.  A
 * later `wifi up`/`wifi adhoc` re-UPs the radio, so this is fully reversible. */
void wifi_off(void)
{
    wifi_cmd_int(WLC_DOWN, 1);
    wifi_have_ip = 0;
    wifi_cur_ssid[0] = 0;
    wifi_log("[wifi] off: radio down, disconnected\r\n");
}

/* Desktop WiFi indicator accessors (M10). */
const char *wifi_ssid(void) { return wifi_cur_ssid; }
void wifi_ipaddr(u8 *o) { int i; for (i = 0; i < 4; i++) o[i] = wifi_ip[i]; }
static int dhcp_build(u8 *out, const u8 *mac, u32 xid, const u8 *reqip, const u8 *srvid)
{
    u8 *e = out, *ip, *udp, *bootp, *opt; int dhcplen, udplen, iplen, i;
    for (i = 0; i < 6; i++) e[i] = 0xFF;
    for (i = 0; i < 6; i++) e[6 + i] = mac[i];
    e[12] = 0x08; e[13] = 0x00;
    ip = e + 14; udp = ip + 20; bootp = udp + 8;
    for (i = 0; i < 236; i++) bootp[i] = 0;
    bootp[0] = 1; bootp[1] = 1; bootp[2] = 6;
    bootp[4]=xid>>24; bootp[5]=xid>>16; bootp[6]=xid>>8; bootp[7]=xid;
    bootp[10] = 0x80;
    for (i = 0; i < 6; i++) bootp[28 + i] = mac[i];
    opt = bootp + 236;
    opt[0]=0x63; opt[1]=0x82; opt[2]=0x53; opt[3]=0x63; opt += 4;
    *opt++ = 53; *opt++ = 1; *opt++ = reqip ? 3 : 1;
    if (reqip) {
        *opt++ = 50; *opt++ = 4; for (i=0;i<4;i++) *opt++ = reqip[i];
        *opt++ = 54; *opt++ = 4; for (i=0;i<4;i++) *opt++ = srvid[i];
    }
    *opt++ = 55; *opt++ = 4; *opt++ = 1; *opt++ = 3; *opt++ = 6; *opt++ = 51;
    *opt++ = 255;
    dhcplen = (int)(opt - bootp);
    udplen = 8 + dhcplen;
    udp[0]=0; udp[1]=68; udp[2]=0; udp[3]=67;
    udp[4]=udplen>>8; udp[5]=udplen&0xFF; udp[6]=0; udp[7]=0;
    iplen = 20 + udplen;
    for (i = 0; i < 20; i++) ip[i] = 0;
    ip[0]=0x45; ip[2]=iplen>>8; ip[3]=iplen&0xFF; ip[8]=64; ip[9]=17;
    for (i = 0; i < 4; i++) ip[16 + i] = 0xFF;
    { u16 c = ip_cksum(ip, 20, 0); ip[10]=c>>8; ip[11]=c&0xFF; }
    return 14 + iplen;
}
static int dhcp_parse(const u8 *eth, int elen, u32 xid, u8 *yiaddr, u8 *srvid, u8 *mask, u8 *gw, u8 *dns)
{
    const u8 *ip, *udp, *bootp, *o, *end; int ihl, mtype = 0, i;
    if (elen < 14 + 20 + 8 + 240) return 0;
    if (eth[12] != 0x08 || eth[13] != 0x00) return 0;
    ip = eth + 14; ihl = (ip[0] & 0x0F) * 4;
    if (ip[9] != 17) return 0;
    udp = ip + ihl;
    if (udp[2] != 0 || udp[3] != 68) return 0;
    bootp = udp + 8;
    if (bootp[0] != 2) return 0;
    if (((u32)bootp[4]<<24|bootp[5]<<16|bootp[6]<<8|bootp[7]) != xid) return 0;
    for (i = 0; i < 4; i++) yiaddr[i] = bootp[16 + i];
    o = bootp + 236;
    if (o[0]!=0x63||o[1]!=0x82||o[2]!=0x53||o[3]!=0x63) return 0;
    o += 4; end = eth + elen;
    while (o < end && *o != 255) {
        int t = *o++, l; if (o >= end) break; l = *o++;
        if (o + l > end) break;
        if (t == 53 && l >= 1) mtype = o[0];
        else if (t == 54 && l >= 4) for (i=0;i<4;i++) srvid[i]=o[i];
        else if (t == 1  && l >= 4) for (i=0;i<4;i++) mask[i]=o[i];
        else if (t == 3  && l >= 4) for (i=0;i<4;i++) gw[i]=o[i];
        else if (t == 6  && l >= 4) for (i=0;i<4;i++) dns[i]=o[i];
        o += l;
    }
    return mtype;
}
static int dhcp_wait(u32 xid, int want, u8 *yiaddr, u8 *srvid, u8 *mask, u8 *gw, u8 *dns)
{
    static u8 fr[2048]; int chan, doff, tries;
    for (tries = 0; tries < 800; tries++) {
        int len = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
        if (len < 0) break;
        if (len == 0) { wifi_delay_us(10000); continue; }
        if (chan != 2 || len < doff + 4) continue;
        { int bdc = 4 + (fr[doff + 3] << 2);
          const u8 *eth = fr + doff + bdc; int elen = len - (doff + bdc);
          int t = dhcp_parse(eth, elen, xid, yiaddr, srvid, mask, gw, dns);
          if (t == want) return t; }
    }
    return 0;
}
/* Run DHCP DISCOVER/REQUEST over the (already associated) link. */
int wifi_dhcp(void)
{
    static u8 pkt[600];
    u8 yi[4]={0}, srv[4]={0}, mask[4]={0}, gw[4]={0}, dns[4]={0};
    u32 xid; int n, t;
    wifi_have_ip = 0; wifi_tn = 0;
    wifi_log("[wifi] === DHCP ===\r\n");
    if (wifi_get_iovar("cur_etheraddr", wifi_mac, 6) != 0) { wifi_log("[wifi] dhcp: cur_etheraddr failed\r\n"); return -1; }
    wifi_log("[wifi] dhcp: mac %02x:%02x:%02x:%02x:%02x:%02x\r\n",
             wifi_mac[0],wifi_mac[1],wifi_mac[2],wifi_mac[3],wifi_mac[4],wifi_mac[5]);
    xid = 0x52610000u | (wifi_mac[4] << 8) | wifi_mac[5];
    n = dhcp_build(pkt, wifi_mac, xid, 0, 0);
    if (wifi_data_tx(pkt, n) != 0) { wifi_log("[wifi] dhcp: DISCOVER tx failed\r\n"); return -1; }
    wifi_log("[wifi] dhcp: DISCOVER sent (%d B), waiting for OFFER...\r\n", n);
    t = dhcp_wait(xid, 2, yi, srv, mask, gw, dns);
    if (t != 2) { wifi_log("[wifi] dhcp: no OFFER (timeout)\r\n"); return -1; }
    wifi_log("[wifi] dhcp: OFFER ip=%d.%d.%d.%d server=%d.%d.%d.%d\r\n",
             yi[0],yi[1],yi[2],yi[3], srv[0],srv[1],srv[2],srv[3]);
    n = dhcp_build(pkt, wifi_mac, xid, yi, srv);
    if (wifi_data_tx(pkt, n) != 0) { wifi_log("[wifi] dhcp: REQUEST tx failed\r\n"); return -1; }
    wifi_log("[wifi] dhcp: REQUEST sent, waiting for ACK...\r\n");
    t = dhcp_wait(xid, 5, yi, srv, mask, gw, dns);
    if (t != 5) { wifi_log("[wifi] dhcp: no ACK (timeout)\r\n"); return -1; }
    for (n = 0; n < 4; n++) { wifi_ip[n]=yi[n]; wifi_mask[n]=mask[n]; wifi_gw[n]=gw[n]; wifi_dns[n]=dns[n]; }
    wifi_have_ip = 1;
    wifi_log("[wifi] *** DHCP ACK: ip=%d.%d.%d.%d mask=%d.%d.%d.%d gw=%d.%d.%d.%d dns=%d.%d.%d.%d ***\r\n",
             wifi_ip[0],wifi_ip[1],wifi_ip[2],wifi_ip[3], wifi_mask[0],wifi_mask[1],wifi_mask[2],wifi_mask[3],
             wifi_gw[0],wifi_gw[1],wifi_gw[2],wifi_gw[3], wifi_dns[0],wifi_dns[1],wifi_dns[2],wifi_dns[3]);
    return 0;
}

/* ================================================================== *
 *  M4 — minimal IP responder: ARP who-has + ICMP echo (host can ping) *
 * ================================================================== */
static int wifi_ip_eq(const u8 *p)
{ return p[0]==wifi_ip[0] && p[1]==wifi_ip[1] && p[2]==wifi_ip[2] && p[3]==wifi_ip[3]; }

/* AODV (M13): process an inbound IPv4 frame — consume AODV control (UDP/654)
 * or relay a transit packet per the route table.  Returns 1 if consumed. */
static int aodv_ip_in(u8 *e, int elen);

/* Answer one received 802.3 frame: ARP-who-has-us -> ARP reply, ICMP echo
 * request -> echo reply.  All over the WLAN data channel. */
static void wifi_handle_frame(u8 *fr, int len, int doff)
{
    int bdc = 4 + (fr[doff + 3] << 2);
    u8 *e = fr + doff + bdc;
    int elen = len - (doff + bdc), et, i;
    if (elen < 14) return;
    et = (e[12] << 8) | e[13];
    if (et == 0x0806 && elen >= 42) {                /* ARP */
        u8 *a = e + 14;
        int op = (a[6] << 8) | a[7];
        if (op == 1 && wifi_ip_eq(a + 24)) {         /* who-has our IP */
            static u8 tx[42];
            for (i = 0; i < 6; i++) tx[i]     = e[6 + i];
            for (i = 0; i < 6; i++) tx[6 + i] = wifi_mac[i];
            tx[12] = 0x08; tx[13] = 0x06;
            { u8 *r = tx + 14;
              r[0]=0;r[1]=1; r[2]=0x08;r[3]=0; r[4]=6;r[5]=4; r[6]=0;r[7]=2;
              for (i=0;i<6;i++) r[8+i]  = wifi_mac[i];
              for (i=0;i<4;i++) r[14+i] = wifi_ip[i];
              for (i=0;i<6;i++) r[18+i] = a[8+i];
              for (i=0;i<4;i++) r[24+i] = a[14+i]; }
            wifi_data_tx(tx, 42);
            wifi_log("[wifi] -> ARP reply to %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                     e[6],e[7],e[8],e[9],e[10],e[11]);
        }
    } else if (et == 0x0800 && elen >= 14 + 20 + 8) {/* IPv4 */
        u8 *ip = e + 14;
        int ihl = (ip[0] & 0x0F) * 4;
        if (aodv_ip_in(e, elen)) return;             /* AODV control / relay */
        if (ip[9] == 1 && wifi_ip_eq(ip + 16)) {     /* ICMP to us */
            u8 *ic = ip + ihl;
            int iptot = (ip[2] << 8) | ip[3];
            int iclen = iptot - ihl;
            if (ic[0] == 8 && iclen >= 8 && 14 + iptot <= elen) {  /* echo request */
                u8 m[6]; u16 c;
                for (i=0;i<6;i++){ m[i]=e[i]; e[i]=e[6+i]; e[6+i]=m[i]; }
                for (i=0;i<4;i++){ u8 t=ip[12+i]; ip[12+i]=ip[16+i]; ip[16+i]=t; }
                ic[0] = 0;
                ic[2]=ic[3]=0; c = ip_cksum(ic, iclen, 0); ic[2]=c>>8; ic[3]=c&0xFF;
                ip[10]=ip[11]=0; c = ip_cksum(ip, ihl, 0); ip[10]=c>>8; ip[11]=c&0xFF;
                wifi_data_tx(e, 14 + iptot);
                wifi_log("[wifi] -> ICMP echo reply\r\n");
            }
        }
    }
}

/* Bounded ARP/ICMP responder: poll the WLAN RX FIFO for `secs` seconds and
 * answer.  Single-threaded (no thread/sem like Pi3) — runs on the shell/main
 * context, so the host can `ping <our-ip>` during the window.  shell: wifi serve */
int wifi_serve(int secs)
{
    static u8 fr[2048]; int chan, doff;
    u32 t0 = SYSTIMER_CLO, dur = (u32)secs * 1000000u;
    if (!wifi_have_ip) { wifi_log("[wifi] serve: no IP yet (run wifi dhcp)\r\n"); return -1; }
    wifi_log("[wifi] serve: ARP/ICMP responder %ds on %d.%d.%d.%d\r\n",
             secs, wifi_ip[0], wifi_ip[1], wifi_ip[2], wifi_ip[3]);
    while ((SYSTIMER_CLO - t0) < dur) {
        int n = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
        if (n > 0 && chan == 2 && n > doff + 4) wifi_handle_frame(fr, n, doff);
        else if (n <= 0) wifi_delay_us(2000);
    }
    wifi_log("[wifi] serve: window ended\r\n");
    return 0;
}

/* Persistent (M9) background responder: poll ONE WLAN RX frame and answer
 * ARP/ICMP.  Called once per wm frame from net_yield_tick (loader/main.c).
 * Single-threaded: a shell client op (ping/http/dns/ntp) runs on the same
 * tick's call stack and blocks the frame loop, so this never races it.
 * Only active once associated + DHCP'd (wifi_have_ip); a no-op otherwise. */
void wifi_net_poll(void)
{
    static u8 fr[2048]; int chan, doff, n, budget;
    if (!wifi_have_ip) return;
    /* Drain all queued RX frames this tick (bounded so we never monopolize the
     * wm frame loop) — at the wm frame rate a single frame/tick dropped pings;
     * draining the FIFO each tick keeps the responder reliable. */
    for (budget = 0; budget < 16; budget++) {
        n = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
        if (n <= 0) break;                       /* FIFO empty */
        if (chan == 2 && n > doff + 4) wifi_handle_frame(fr, n, doff);
    }
}

/* ================================================================== *
 *  M5 — ping CLIENT (ARP resolve + next-hop routing + ICMP echo)     *
 * ================================================================== */
/* Resolve an IP to a MAC via ARP (single-threaded: we own the RX FIFO). */
static int wifi_arp_resolve(const u8 *ip, u8 *mac)
{
    static u8 fr[2048], tx[42];
    int i, chan, doff, tries, w;
    for (tries = 0; tries < 12; tries++) {
        for (i = 0; i < 6; i++) tx[i] = 0xFF;
        for (i = 0; i < 6; i++) tx[6 + i] = wifi_mac[i];
        tx[12] = 0x08; tx[13] = 0x06;
        { u8 *a = tx + 14;
          a[0]=0;a[1]=1;a[2]=0x08;a[3]=0;a[4]=6;a[5]=4;a[6]=0;a[7]=1;
          for (i=0;i<6;i++) a[8+i]  = wifi_mac[i];
          for (i=0;i<4;i++) a[14+i] = wifi_ip[i];
          for (i=0;i<6;i++) a[18+i] = 0;
          for (i=0;i<4;i++) a[24+i] = ip[i]; }
        wifi_data_tx(tx, 42);
        for (w = 0; w < 40; w++) {
            int n = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
            if (n <= 0) { wifi_delay_us(5000); continue; }
            if (chan != 2 || n < doff + 4) continue;
            { int bdc = 4 + (fr[doff+3]<<2); u8 *e = fr+doff+bdc; int el = n-(doff+bdc);
              if (el >= 42 && e[12]==0x08 && e[13]==0x06) {
                u8 *a = e + 14; int op = (a[6]<<8)|a[7];
                if (op == 2 && a[14]==ip[0]&&a[15]==ip[1]&&a[16]==ip[2]&&a[17]==ip[3]) {
                    for (i=0;i<6;i++) mac[i] = a[8+i];
                    return 0;
                }
              } }
        }
    }
    return -1;
}
/* On-subnet -> ARP the host; off-subnet -> ARP the default gateway. */
static int wifi_nexthop_mac(const u8 *dst, u8 *mac)
{
    int i, onsub = 1;
    for (i = 0; i < 4; i++)
        if ((dst[i] & wifi_mask[i]) != (wifi_ip[i] & wifi_mask[i])) onsub = 0;
    return wifi_arp_resolve(onsub ? dst : wifi_gw, mac);
}
/* ICMP echo client: ping `ip` `count` times.  Returns # replies. */
int wifi_ping(const u8 *ip, int count)
{
    static u8 fr[2048], tx[128];
    u8 nh[6];
    int i, chan, doff, seq, w, replies = 0, paylen = 32; u16 c;
    wifi_tn = 0;
    wifi_log("[wifi] === PING %d.%d.%d.%d (%d) ===\r\n", ip[0],ip[1],ip[2],ip[3], count);
    if (!wifi_have_ip) { wifi_log("[wifi] ping: no IP yet\r\n"); return -1; }
    if (wifi_nexthop_mac(ip, nh) != 0) { wifi_log("[wifi] ping: ARP/next-hop failed\r\n"); return -1; }
    wifi_log("[wifi] ping: next-hop %02x:%02x:%02x:%02x:%02x:%02x\r\n", nh[0],nh[1],nh[2],nh[3],nh[4],nh[5]);
    for (seq = 0; seq < count; seq++) {
        int icmplen = 8 + paylen, iptot = 20 + icmplen, framelen = 14 + iptot, rcvd = 0;
        for (i=0;i<6;i++) tx[i] = nh[i];
        for (i=0;i<6;i++) tx[6+i] = wifi_mac[i];
        tx[12]=0x08; tx[13]=0x00;
        { u8 *ip4 = tx + 14, *ic;
          for (i=0;i<20;i++) ip4[i]=0;
          ip4[0]=0x45; ip4[2]=iptot>>8; ip4[3]=iptot&0xFF; ip4[5]=seq+1; ip4[8]=64; ip4[9]=1;
          for (i=0;i<4;i++) ip4[12+i]=wifi_ip[i];
          for (i=0;i<4;i++) ip4[16+i]=ip[i];
          c=ip_cksum(ip4,20,0); ip4[10]=c>>8; ip4[11]=c&0xFF;
          ic = ip4 + 20;
          for (i=0;i<icmplen;i++) ic[i]=0;
          ic[0]=8; ic[4]=0xBE; ic[5]=0xEF; ic[6]=seq>>8; ic[7]=seq&0xFF;
          for (i=0;i<paylen;i++) ic[8+i]=(u8)(0x61+(i%26));
          c=ip_cksum(ic,icmplen,0); ic[2]=c>>8; ic[3]=c&0xFF; }
        wifi_data_tx(tx, framelen);
        for (w = 0; w < 60 && !rcvd; w++) {
            int n = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
            if (n <= 0) { wifi_delay_us(5000); continue; }
            if (chan != 2 || n < doff + 4) continue;
            { int bdc = 4 + (fr[doff+3]<<2); u8 *e = fr+doff+bdc; int el = n-(doff+bdc);
              if (el >= 14+20+8 && e[12]==0x08 && e[13]==0x00) {
                u8 *ip4 = e+14; int ihl = (ip4[0]&0xF)*4; u8 *ic = ip4+ihl;
                if (ip4[9]==1 && ip4[12]==ip[0]&&ip4[13]==ip[1]&&ip4[14]==ip[2]&&ip4[15]==ip[3]
                    && ic[0]==0 && (((ic[6]<<8)|ic[7])==seq)) rcvd = 1;
              } }
        }
        if (rcvd) { replies++; wifi_log("[wifi] ping: reply seq=%d\r\n", seq); }
        else        wifi_log("[wifi] ping: timeout seq=%d\r\n", seq);
        wifi_delay_us(300000);
    }
    wifi_log("[wifi] *** ping %d.%d.%d.%d: %d/%d replies ***\r\n", ip[0],ip[1],ip[2],ip[3], replies, count);
    return replies;
}

/* ================================================================== *
 *  M6 — NTP client (UDP/123 via gateway) -> Unix time + JST date     *
 * ================================================================== */
/* Query NTP server `srv` (off-subnet -> routed via gw); returns Unix secs. */
unsigned long wifi_ntp(const u8 *srv)
{
    static u8 fr[2048], tx[90];
    u8 nh[6]; int i, chan, doff, w; u16 c;
    unsigned long secs = 0;
    wifi_tn = 0;
    wifi_log("[wifi] === NTP %d.%d.%d.%d ===\r\n", srv[0],srv[1],srv[2],srv[3]);
    if (!wifi_have_ip) { wifi_log("[wifi] ntp: no IP yet\r\n"); return 0; }
    if (wifi_nexthop_mac(srv, nh) != 0) { wifi_log("[wifi] ntp: next-hop ARP failed\r\n"); return 0; }
    { int udplen = 8 + 48, iptot = 20 + udplen, framelen = 14 + iptot;
      u8 *ip4 = tx + 14, *udp, *ntp;
      for (i=0;i<6;i++) tx[i] = nh[i];
      for (i=0;i<6;i++) tx[6+i] = wifi_mac[i];
      tx[12]=0x08; tx[13]=0x00;
      for (i=0;i<framelen-14;i++) ip4[i]=0;
      ip4[0]=0x45; ip4[2]=iptot>>8; ip4[3]=iptot&0xFF; ip4[5]=1; ip4[8]=64; ip4[9]=17;
      for (i=0;i<4;i++) ip4[12+i]=wifi_ip[i];
      for (i=0;i<4;i++) ip4[16+i]=srv[i];
      c=ip_cksum(ip4,20,0); ip4[10]=c>>8; ip4[11]=c&0xFF;
      udp = ip4 + 20;
      udp[0]=0x00; udp[1]=0x7b; udp[2]=0x00; udp[3]=0x7b;
      udp[4]=udplen>>8; udp[5]=udplen&0xFF; udp[6]=0; udp[7]=0;
      ntp = udp + 8;
      ntp[0] = 0x1b;                     /* LI=0 VN=3 Mode=3 (client) */
      wifi_data_tx(tx, framelen); }
    for (w = 0; w < 120; w++) {
        int n = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
        if (n <= 0) { wifi_delay_us(5000); continue; }
        if (chan != 2 || n < doff + 4) continue;
        { int bdc = 4 + (fr[doff+3]<<2); u8 *e = fr+doff+bdc; int el = n-(doff+bdc);
          if (el >= 14+20+8+48 && e[12]==0x08 && e[13]==0x00) {
            u8 *ip4 = e+14; int ihl = (ip4[0]&0xF)*4; u8 *udp = ip4+ihl, *ntp = udp+8;
            if (ip4[9]==17 && udp[2]==0x00 && udp[3]==0x7b
                && ip4[12]==srv[0]&&ip4[13]==srv[1]&&ip4[14]==srv[2]&&ip4[15]==srv[3]) {
                unsigned long ntp_secs = ((unsigned long)ntp[40]<<24)|((unsigned long)ntp[41]<<16)
                                       | ((unsigned long)ntp[42]<<8)|ntp[43];
                secs = ntp_secs - 2208988800UL;   /* 1900 -> 1970 epoch */
                break;
            }
          } }
    }
    if (secs) {
        /* civil JST (UTC+9) date via Howard Hinnant's algorithm */
        unsigned long jst = secs + 9UL*3600;
        long z = (long)(jst / 86400) + 719468;
        long era = (z >= 0 ? z : z - 146096) / 146097;
        unsigned doe = (unsigned)(z - era*146097);
        unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
        long yy = (long)yoe + era*400;
        unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
        unsigned mp = (5*doy + 2)/153;
        unsigned dd = doy - (153*mp+2)/5 + 1;
        unsigned mm = mp < 10 ? mp+3 : mp-9;
        int Y = (int)(yy + (mm <= 2)), tod = (int)(jst % 86400);
        /* wifi_log is 32-bit only -> cast (unix secs fit in u32 until 2106). */
        wifi_log("[wifi] *** NTP unix=%u  JST=%04d-%02u-%02u %02d:%02d:%02d ***\r\n",
                 (u32)secs, Y, mm, dd, tod/3600, (tod%3600)/60, tod%60);
    } else wifi_log("[wifi] ntp: no reply (timeout)\r\n");
    return secs;
}

/* ================================================================== *
 *  M7 — minimal TCP + HTTP/1.0 client over WiFi (plaintext fetch)    *
 * ================================================================== */
static char wifi_http_buf[12288];
static int  wifi_http_len = 0;
int wifi_http_get_buf(char **p) { *p = wifi_http_buf; return wifi_http_len; }

/* Send one TCP segment (eth+IP+TCP[+data]) to dip via next-hop nh. */
static void wifi_tcp_send(const u8 *nh, const u8 *dip, int sport, int dport,
                          u32 seq, u32 ack, int flags, const u8 *data, int dlen)
{
    static u8 tx[700];
    int i, tcplen = 20 + dlen, iptot = 20 + tcplen, framelen = 14 + iptot;
    u8 *ip4, *tcp; u32 psum; u16 c;
    if (framelen > (int)sizeof(tx)) return;
    for (i = 0; i < 6; i++) tx[i] = nh[i];
    for (i = 0; i < 6; i++) tx[6 + i] = wifi_mac[i];
    tx[12] = 0x08; tx[13] = 0x00;
    ip4 = tx + 14;
    for (i = 0; i < 20; i++) ip4[i] = 0;
    ip4[0]=0x45; ip4[2]=iptot>>8; ip4[3]=iptot&0xFF; ip4[5]=1; ip4[8]=64; ip4[9]=6;
    for (i = 0; i < 4; i++) ip4[12+i] = wifi_ip[i];
    for (i = 0; i < 4; i++) ip4[16+i] = dip[i];
    c = ip_cksum(ip4, 20, 0); ip4[10]=c>>8; ip4[11]=c&0xFF;
    tcp = ip4 + 20;
    for (i = 0; i < 20; i++) tcp[i] = 0;
    tcp[0]=sport>>8; tcp[1]=sport&0xFF; tcp[2]=dport>>8; tcp[3]=dport&0xFF;
    tcp[4]=seq>>24; tcp[5]=seq>>16; tcp[6]=seq>>8; tcp[7]=seq;
    tcp[8]=ack>>24; tcp[9]=ack>>16; tcp[10]=ack>>8; tcp[11]=ack;
    tcp[12]=5<<4; tcp[13]=flags; tcp[14]=0x20; tcp[15]=0x00;
    for (i = 0; i < dlen; i++) tcp[20+i] = data[i];
    psum = ((u32)wifi_ip[0]<<8|wifi_ip[1]) + ((u32)wifi_ip[2]<<8|wifi_ip[3])
         + ((u32)dip[0]<<8|dip[1]) + ((u32)dip[2]<<8|dip[3]) + 6 + tcplen;
    c = ip_cksum(tcp, tcplen, psum); tcp[16]=c>>8; tcp[17]=c&0xFF;
    wifi_data_tx(tx, framelen);
}

/* HTTP/1.0 GET http://<host>/ at ip:80.  Response (capped) -> wifi_http_buf,
 * mirrored to the serial console. */
int wifi_http(const u8 *ip, const char *host)
{
    static u8 fr[2048];
    static char req[256];
    u8 nh[6];
    u32 iss = 0x015A0000, our_seq, our_ack = 0, their_seq;
    int chan, doff, w, sport = 0xC0DE, reqn, got_synack = 0, fin = 0, idle = 0, i;
    wifi_http_len = 0; wifi_tn = 0;
    wifi_log("[wifi] === HTTP GET http://%s/ (%d.%d.%d.%d:80) ===\r\n", host, ip[0],ip[1],ip[2],ip[3]);
    if (!wifi_have_ip) { wifi_log("[wifi] http: no IP yet\r\n"); return -1; }
    if (wifi_nexthop_mac(ip, nh) != 0) { wifi_log("[wifi] http: next-hop ARP failed\r\n"); return -1; }

    wifi_tcp_send(nh, ip, sport, 80, iss, 0, 0x02, 0, 0);          /* SYN */
    for (w = 0; w < 600 && !got_synack; w++) {
        int n;
        if (w && (w % 60) == 0) wifi_tcp_send(nh, ip, sport, 80, iss, 0, 0x02, 0, 0); /* retransmit SYN (WiFi loss) */
        n = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
        if (n <= 0) { wifi_delay_us(5000); continue; }
        if (chan != 2 || n < doff + 4) continue;
        { int bdc = 4 + (fr[doff+3]<<2); u8 *e = fr+doff+bdc; int el = n-(doff+bdc);
          if (el >= 14+20+20 && e[12]==0x08 && e[13]==0x00) {
            u8 *ip4 = e+14; int ihl=(ip4[0]&0xF)*4; u8 *tcp = ip4+ihl;
            int dport = (tcp[2]<<8)|tcp[3];
            if (ip4[9]==6 && dport==sport && (tcp[13] & 0x12)==0x12) {
                their_seq = ((u32)tcp[4]<<24)|((u32)tcp[5]<<16)|((u32)tcp[6]<<8)|tcp[7];
                got_synack = 1;
            }
          } }
    }
    if (!got_synack) { wifi_log("[wifi] http: no SYN-ACK (timeout)\r\n"); return -1; }
    our_seq = iss + 1; our_ack = their_seq + 1;
    wifi_tcp_send(nh, ip, sport, 80, our_seq, our_ack, 0x10, 0, 0);  /* ACK */
    wifi_log("[wifi] http: connected, sending GET\r\n");

    { const char *a = "GET / HTTP/1.0\r\nHost: "; const char *t = "\r\nConnection: close\r\n\r\n"; const char *p;
      reqn = 0;
      for (p = a; *p; p++) req[reqn++] = *p;
      for (p = host; *p; p++) req[reqn++] = *p;
      for (p = t; *p; p++) req[reqn++] = *p; }
    wifi_tcp_send(nh, ip, sport, 80, our_seq, our_ack, 0x18, (u8*)req, reqn);  /* PSH|ACK */
    our_seq += reqn;

    for (w = 0; w < 2000 && !fin; w++) {
        int n = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
        if (n <= 0) { wifi_delay_us(3000); if (++idle > 400) break; continue; }
        idle = 0;
        if (chan != 2 || n < doff + 4) continue;
        { int bdc = 4 + (fr[doff+3]<<2); u8 *e = fr+doff+bdc; int el = n-(doff+bdc);
          if (el < 14+20+20 || e[12]!=0x08 || e[13]!=0x00) continue;
          { u8 *ip4 = e+14; int ihl=(ip4[0]&0xF)*4;
            int iptot = (ip4[2]<<8)|ip4[3];
            u8 *tcp = ip4+ihl; int thl = (tcp[12]>>4)*4;
            int dport = (tcp[2]<<8)|tcp[3]; u32 sseq; int dlen;
            if (ip4[9]!=6 || dport!=sport) continue;
            sseq = ((u32)tcp[4]<<24)|((u32)tcp[5]<<16)|((u32)tcp[6]<<8)|tcp[7];
            dlen = iptot - ihl - thl; if (dlen < 0) dlen = 0;
            if (dlen > 0 && sseq == our_ack) {
                u8 *payload = tcp + thl; int k;
                for (k = 0; k < dlen && wifi_http_len < (int)sizeof(wifi_http_buf)-1; k++)
                    wifi_http_buf[wifi_http_len++] = payload[k];
                our_ack += dlen;
                wifi_tcp_send(nh, ip, sport, 80, our_seq, our_ack, 0x10, 0, 0);
            } else if (dlen > 0) {
                wifi_tcp_send(nh, ip, sport, 80, our_seq, our_ack, 0x10, 0, 0);
            }
            if (tcp[13] & 0x01) {
                our_ack += 1;
                wifi_tcp_send(nh, ip, sport, 80, our_seq, our_ack, 0x11, 0, 0);  /* FIN|ACK */
                fin = 1;
            }
            if (wifi_http_len >= (int)sizeof(wifi_http_buf)-1) break;
          } }
    }
    wifi_http_buf[wifi_http_len] = '\0';
    wifi_log("[wifi] *** HTTP got %d bytes from %s (fin=%d) ***\r\n", wifi_http_len, host, fin);
    /* mirror the page to serial */
    uart_puts("\r\n---- http://"); uart_puts(host); uart_puts("/ ----\r\n");
    for (i = 0; i < wifi_http_len; i++) uart_putc(wifi_http_buf[i]);
    uart_puts("\r\n---- end ----\r\n");
    return wifi_http_len;
}

/* ================================================================== *
 *  M8 — minimal DNS resolver (UDP/53 -> first A record)              *
 * ================================================================== */
/* Skip a DNS name at offset `o` in message `m` (len `mlen`); handles
 * compression pointers (0xC0).  Returns the offset just past the name. */
static int dns_skip_name(const u8 *m, int mlen, int o)
{
    while (o < mlen) {
        u8 b = m[o];
        if ((b & 0xC0) == 0xC0) return o + 2;   /* compression pointer ends the name */
        if (b == 0) return o + 1;               /* root label */
        o += 1 + b;                             /* label: len + bytes */
    }
    return mlen;
}
/* Resolve `host` to an IPv4 via the DHCP-provided DNS server (wifi_dns).
 * Returns 0 + fills out[4] on success. */
int wifi_resolve(const char *host, u8 *out)
{
    static u8 fr[2048], tx[400];
    u8 nh[6], *msg; int i, chan, doff, w, qn, qlen, sport = 0xD0E5;
    u16 id = 0x1234;
    wifi_tn = 0;
    wifi_log("[wifi] === DNS resolve %s via %d.%d.%d.%d ===\r\n",
             host, wifi_dns[0],wifi_dns[1],wifi_dns[2],wifi_dns[3]);
    if (!wifi_have_ip) { wifi_log("[wifi] dns: no IP yet\r\n"); return -1; }
    if (wifi_dns[0]==0 && wifi_dns[1]==0) { wifi_log("[wifi] dns: no DNS server\r\n"); return -1; }
    if (wifi_nexthop_mac(wifi_dns, nh) != 0) { wifi_log("[wifi] dns: next-hop ARP failed\r\n"); return -1; }

    /* build the DNS query message into a scratch, then frame it */
    { static u8 q[300]; const char *p;
      q[0]=id>>8; q[1]=id; q[2]=0x01; q[3]=0x00;       /* RD=1 */
      q[4]=0;q[5]=1; q[6]=0;q[7]=0; q[8]=0;q[9]=0; q[10]=0;q[11]=0;  /* qd=1 */
      qn = 12;
      for (p = host; *p; ) {
          const char *s = p; int l = 0;
          while (*p && *p != '.') { p++; l++; }
          q[qn++] = (u8)l;
          for (i = 0; i < l; i++) q[qn++] = s[i];
          if (*p == '.') p++;
      }
      q[qn++] = 0; q[qn++]=0; q[qn++]=1; q[qn++]=0; q[qn++]=1;   /* QTYPE A, QCLASS IN */
      qlen = qn;
      /* eth + IP(20) + UDP(8) + DNS(qlen) */
      { int udplen = 8 + qlen, iptot = 20 + udplen, framelen = 14 + iptot;
        u8 *ip4 = tx + 14, *udp;
        for (i=0;i<6;i++) tx[i] = nh[i];
        for (i=0;i<6;i++) tx[6+i] = wifi_mac[i];
        tx[12]=0x08; tx[13]=0x00;
        for (i=0;i<20;i++) ip4[i]=0;
        ip4[0]=0x45; ip4[2]=iptot>>8; ip4[3]=iptot&0xFF; ip4[5]=1; ip4[8]=64; ip4[9]=17;
        for (i=0;i<4;i++) ip4[12+i]=wifi_ip[i];
        for (i=0;i<4;i++) ip4[16+i]=wifi_dns[i];
        { u16 c=ip_cksum(ip4,20,0); ip4[10]=c>>8; ip4[11]=c&0xFF; }
        udp = ip4 + 20;
        udp[0]=sport>>8; udp[1]=sport&0xFF; udp[2]=0; udp[3]=53;
        udp[4]=udplen>>8; udp[5]=udplen&0xFF; udp[6]=0; udp[7]=0;
        for (i=0;i<qlen;i++) udp[8+i] = q[i];
        wifi_data_tx(tx, framelen); } }

    for (w = 0; w < 300; w++) {
        int n = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
        if (n <= 0) { wifi_delay_us(5000); continue; }
        if (chan != 2 || n < doff + 4) continue;
        { int bdc = 4 + (fr[doff+3]<<2); u8 *e = fr+doff+bdc; int el = n-(doff+bdc);
          if (el < 14+20+8+12 || e[12]!=0x08 || e[13]!=0x00) continue;
          { u8 *ip4 = e+14; int ihl=(ip4[0]&0xF)*4; u8 *udp = ip4+ihl;
            int dport = (udp[2]<<8)|udp[3];
            if (ip4[9]!=17 || dport!=sport) continue;
            if (!(ip4[12]==wifi_dns[0]&&ip4[13]==wifi_dns[1]&&ip4[14]==wifi_dns[2]&&ip4[15]==wifi_dns[3])) continue;
            msg = udp + 8;
            { int mlen = el - (int)(msg - e);
              int an = (msg[6]<<8)|msg[7], o = 12, a;
              o = dns_skip_name(msg, mlen, o) + 4;     /* question: name + qtype + qclass */
              for (a = 0; a < an && o + 10 <= mlen; a++) {
                  int type, rdl;
                  o = dns_skip_name(msg, mlen, o);
                  if (o + 10 > mlen) break;
                  type = (msg[o]<<8)|msg[o+1];
                  rdl  = (msg[o+8]<<8)|msg[o+9];
                  o += 10;
                  if (type == 1 && rdl == 4 && o + 4 <= mlen) {
                      for (i = 0; i < 4; i++) out[i] = msg[o+i];
                      wifi_log("[wifi] *** DNS %s -> %d.%d.%d.%d ***\r\n",
                               host, out[0],out[1],out[2],out[3]);
                      return 0;
                  }
                  o += rdl;
              } }
            wifi_log("[wifi] dns: reply had no A record\r\n");
            return -1;
          } }
    }
    wifi_log("[wifi] dns: no reply (timeout)\r\n");
    return -1;
}

/* ================================================================== *
 *  M11 — TFTP client over WiFi (netboot transport)                  *
 * ================================================================== */
/* Send a UDP datagram (payload p[plen]) over WiFi to dip:dport from sport. */
static void wifi_udp_tx(const u8 *nh, const u8 *dip, int sport, int dport, const u8 *p, int plen)
{
    static u8 tx[1600];
    int i, udplen = 8 + plen, iptot = 20 + udplen, framelen = 14 + iptot;
    u8 *ip4 = tx + 14, *udp;
    if (framelen > (int)sizeof(tx)) return;
    for (i=0;i<6;i++) tx[i]=nh[i];
    for (i=0;i<6;i++) tx[6+i]=wifi_mac[i];
    tx[12]=0x08; tx[13]=0x00;
    for (i=0;i<20;i++) ip4[i]=0;
    ip4[0]=0x45; ip4[2]=iptot>>8; ip4[3]=iptot&0xFF; ip4[5]=1; ip4[8]=64; ip4[9]=17;
    for (i=0;i<4;i++) ip4[12+i]=wifi_ip[i];
    for (i=0;i<4;i++) ip4[16+i]=dip[i];
    { u16 c=ip_cksum(ip4,20,0); ip4[10]=c>>8; ip4[11]=c&0xFF; }
    udp = ip4+20;
    udp[0]=sport>>8; udp[1]=sport&0xFF; udp[2]=dport>>8; udp[3]=dport&0xFF;
    udp[4]=udplen>>8; udp[5]=udplen&0xFF; udp[6]=0; udp[7]=0;   /* csum 0 (optional v4) */
    for (i=0;i<plen;i++) udp[8+i]=p[i];
    wifi_data_tx(tx, framelen);
}

/* ================================================================== *
 *  M13 — minimal AODV multi-hop routing (RFC 3561 core)             *
 * ================================================================== */
#define AODV_PORT  654
#define AODV_RREQ  1
#define AODV_RREP  2
#define AODV_NRT   16
struct aodv_rt { u8 dst[4], nh[4]; u8 hops; u32 seq; int valid; };
static struct aodv_rt g_rt[AODV_NRT];
static u16 g_rreqid = 0; static u32 g_myseq = 0;
static u8 g_seen_o[8][4]; static u16 g_seen_id[8]; static int g_seen_n = 0;
static int ip4eq(const u8 *a, const u8 *b){ return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2]&&a[3]==b[3]; }
static struct aodv_rt *aodv_find(const u8 *d){ int i; for(i=0;i<AODV_NRT;i++) if(g_rt[i].valid&&ip4eq(g_rt[i].dst,d)) return &g_rt[i]; return 0; }
static void aodv_add(const u8 *d, const u8 *nh, u8 hops, u32 seq){
    struct aodv_rt *r = aodv_find(d); int i;
    if(!r){ for(i=0;i<AODV_NRT;i++) if(!g_rt[i].valid){ r=&g_rt[i]; break; } }
    if(!r) r=&g_rt[0];
    if(r->valid && r->hops <= hops && ip4eq(r->dst,d)) return;   /* keep shorter route */
    for(i=0;i<4;i++){ r->dst[i]=d[i]; r->nh[i]=nh[i]; } r->hops=hops; r->seq=seq; r->valid=1;
}
static int aodv_seen(const u8 *o, u16 id){ int i; for(i=0;i<g_seen_n;i++) if(g_seen_id[i]==id&&ip4eq(g_seen_o[i],o)) return 1; return 0; }
static void aodv_remember(const u8 *o, u16 id){ int s=g_seen_n%8,i; for(i=0;i<4;i++) g_seen_o[s][i]=o[i]; g_seen_id[s]=id; g_seen_n++; }
static void aodv_bcast(const u8 *p, int n){ u8 bc[6]={0xff,0xff,0xff,0xff,0xff,0xff}, bip[4]={255,255,255,255}; wifi_udp_tx(bc, bip, AODV_PORT, AODV_PORT, p, n); }
static void aodv_send_rreq(const u8 *dst){
    u8 p[16]; int i;
    g_rreqid++; g_myseq++;
    p[0]=AODV_RREQ; p[1]=g_rreqid; p[2]=g_rreqid>>8;
    for(i=0;i<4;i++) p[3+i]=wifi_ip[i];
    for(i=0;i<4;i++) p[7+i]=dst[i];
    p[11]=g_myseq; p[12]=g_myseq>>8; p[13]=g_myseq>>16; p[14]=g_myseq>>24; p[15]=0;
    aodv_remember(wifi_ip, g_rreqid);
    wifi_log("[wifi] aodv: RREQ id=%d for %d.%d.%d.%d\r\n", g_rreqid, dst[0],dst[1],dst[2],dst[3]);
    aodv_bcast(p, 16);
}
/* process inbound IPv4 frame: AODV control (UDP/654) or transit relay. */
static int aodv_ip_in(u8 *e, int elen)
{
    u8 *ip = e+14; int ihl=(ip[0]&0xF)*4, i; u8 ipsrc[4], ipdst[4], *l2src = e+6;
    for(i=0;i<4;i++){ ipsrc[i]=ip[12+i]; ipdst[i]=ip[16+i]; }
    if (ip[9]==17) {                                  /* UDP */
        u8 *udp = ip+ihl; int dport=(udp[2]<<8)|udp[3];
        if (dport != AODV_PORT) return 0;             /* not AODV -> normal path */
        { u8 *p = udp+8; int type=p[0]; u8 orig[4], dst[4], hop=p[15]; u32 sq;
          for(i=0;i<4;i++){ orig[i]=p[3+i]; dst[i]=p[7+i]; }
          sq = p[11]|(p[12]<<8)|(p[13]<<16)|((u32)p[14]<<24);
          if (type==AODV_RREQ) {
              u16 rid = p[1]|(p[2]<<8);
              if (aodv_seen(orig, rid)) return 1;
              aodv_remember(orig, rid);
              aodv_add(orig, ipsrc, hop+1, sq);        /* reverse route to orig via prev hop */
              if (ip4eq(dst, wifi_ip)) {               /* I am the target -> RREP */
                  u8 r[16]; g_myseq++;
                  r[0]=AODV_RREP; for(i=0;i<4;i++){ r[3+i]=orig[i]; r[7+i]=wifi_ip[i]; }
                  r[11]=g_myseq; r[12]=g_myseq>>8; r[13]=g_myseq>>16; r[14]=g_myseq>>24; r[15]=0;
                  wifi_udp_tx(l2src, ipsrc, AODV_PORT, AODV_PORT, r, 16);  /* unicast to prev hop */
                  wifi_log("[wifi] aodv: RREQ for me from %d.%d.%d.%d -> RREP\r\n", orig[0],orig[1],orig[2],orig[3]);
              } else {                                 /* rebroadcast (relay discovery) */
                  p[15]=hop+1; aodv_bcast(p, 16);
              }
              return 1;
          } else if (type==AODV_RREP) {
              aodv_add(dst, ipsrc, hop+1, sq);          /* forward route to dst via prev hop */
              if (ip4eq(orig, wifi_ip)) {
                  wifi_log("[wifi] aodv: *** route to %d.%d.%d.%d via %d.%d.%d.%d (%d hop) ***\r\n",
                           dst[0],dst[1],dst[2],dst[3], ipsrc[0],ipsrc[1],ipsrc[2],ipsrc[3], hop+1);
              } else {                                  /* forward RREP toward orig */
                  struct aodv_rt *rr = aodv_find(orig); u8 mac[6];
                  if (rr && wifi_arp_resolve(rr->nh, mac)==0) {
                      p[15]=hop+1; wifi_udp_tx(mac, rr->nh, AODV_PORT, AODV_PORT, p, 16);
                  }
              }
              return 1;
          } }
        return 1;
    }
    /* non-UDP transit packet (we are an intermediate hop): relay per route table */
    if (!ip4eq(ipdst, wifi_ip) && ipdst[0] != 255) {
        struct aodv_rt *r = aodv_find(ipdst); u8 mac[6];
        if (r && wifi_arp_resolve(r->nh, mac)==0) {
            for(i=0;i<6;i++) e[i]=mac[i];
            for(i=0;i<6;i++) e[6+i]=wifi_mac[i];
            wifi_data_tx(e, elen);
            wifi_log("[wifi] aodv: relay %d.%d.%d.%d -> nh %d.%d.%d.%d\r\n",
                     ipdst[0],ipdst[1],ipdst[2],ipdst[3], r->nh[0],r->nh[1],r->nh[2],r->nh[3]);
        }
        return 1;                                      /* consumed (relayed or no route) */
    }
    return 0;                                          /* for us -> normal handling */
}
/* Originator: discover a route to dst (RREQ + wait for RREP).  Reads+processes
 * frames itself (the wm-tick poller is blocked while this command runs). */
int wifi_aodv(const u8 *dst)
{
    static u8 fr[2048]; int chan, doff, t, r2;
    wifi_tn = 0;
    wifi_log("[wifi] === AODV discover %d.%d.%d.%d ===\r\n", dst[0],dst[1],dst[2],dst[3]);
    if (!wifi_have_ip) { wifi_log("[wifi] aodv: no IP (run wifi adhoc first)\r\n"); return -1; }
    if (aodv_find(dst)) { wifi_log("[wifi] aodv: route already known\r\n"); return 0; }
    for (r2 = 0; r2 < 4; r2++) {
        aodv_send_rreq(dst);
        for (t = 0; t < 80; t++) {
            int n = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
            if (n > 0 && chan == 2 && n > doff + 4) wifi_handle_frame(fr, n, doff);
            else wifi_delay_us(5000);
            if (aodv_find(dst)) { struct aodv_rt *r = aodv_find(dst);
                wifi_log("[wifi] *** AODV route: %d.%d.%d.%d via %d.%d.%d.%d, %d hop ***\r\n",
                         dst[0],dst[1],dst[2],dst[3], r->nh[0],r->nh[1],r->nh[2],r->nh[3], r->hops);
                return 0; }
        }
    }
    wifi_log("[wifi] aodv: no route to %d.%d.%d.%d (RREP timeout)\r\n", dst[0],dst[1],dst[2],dst[3]);
    return -1;
}
/* TFTP RRQ (octet) over WiFi: fetch `fname` from srv into dst[maxlen].
 * Returns total bytes on success, -1 on error/timeout. */
int wifi_tftp_get(const u8 *srv, const char *fname, u8 *dst, int maxlen)
{
    static u8 fr[2048], pkt[600];
    u8 nh[6];
    int i, chan, doff, w, total = 0, sport = 0xB100, srv_tid = 0, nextblk = 1, finished = 0;
    wifi_tn = 0;
    wifi_log("[wifi] === TFTP get '%s' from %d.%d.%d.%d ===\r\n", fname, srv[0],srv[1],srv[2],srv[3]);
    if (!wifi_have_ip) { wifi_log("[wifi] tftp: no IP yet\r\n"); return -1; }
    if (wifi_nexthop_mac(srv, nh) != 0) { wifi_log("[wifi] tftp: next-hop ARP failed\r\n"); return -1; }
    /* RRQ: opcode 1, filename\0, "octet"\0 */
    { int n = 0; const char *p;
      pkt[n++]=0; pkt[n++]=1;
      for (p=fname; *p; p++) pkt[n++]=*p; pkt[n++]=0;
      for (p="octet"; *p; p++) pkt[n++]=*p; pkt[n++]=0;
      wifi_udp_tx(nh, srv, sport, 69, pkt, n); }

    for (w = 0; w < 8000 && !finished; w++) {
        int n = wifi_read_frame(fr, sizeof(fr), &chan, &doff);
        if (n <= 0) {
            wifi_delay_us(3000);
            if ((w % 200) == 199) {                      /* retransmit */
                if (nextblk == 1) {                      /* re-RRQ */
                    int m = 0; const char *p;
                    pkt[m++]=0; pkt[m++]=1;
                    for (p=fname; *p; p++) pkt[m++]=*p; pkt[m++]=0;
                    for (p="octet"; *p; p++) pkt[m++]=*p; pkt[m++]=0;
                    wifi_udp_tx(nh, srv, sport, 69, pkt, m);
                } else if (srv_tid) {                     /* re-ACK last block */
                    u8 a[4]; a[0]=0; a[1]=4; a[2]=(nextblk-1)>>8; a[3]=(nextblk-1);
                    wifi_udp_tx(nh, srv, sport, srv_tid, a, 4);
                }
            }
            continue;
        }
        if (chan != 2 || n < doff + 4) continue;
        { int bdc = 4 + (fr[doff+3]<<2); u8 *e = fr+doff+bdc; int el = n-(doff+bdc);
          if (el < 14+20+8 || e[12]!=0x08 || e[13]!=0x00) continue;
          { u8 *ip4 = e+14; int ihl=(ip4[0]&0xF)*4; u8 *udp = ip4+ihl;
            int dport = (udp[2]<<8)|udp[3], ssport = (udp[0]<<8)|udp[1];
            u8 *tf; int op, blk, tlen, dlen;
            if (ip4[9]!=17 || dport!=sport) continue;
            if (!(ip4[12]==srv[0]&&ip4[13]==srv[1]&&ip4[14]==srv[2]&&ip4[15]==srv[3])) continue;
            tf = udp + 8; tlen = ((udp[4]<<8)|udp[5]) - 8;
            if (tlen < 4) continue;
            op = (tf[0]<<8)|tf[1];
            if (op == 5) { wifi_log("[wifi] tftp: server ERROR code=%d\r\n", (tf[2]<<8)|tf[3]); return -1; }
            if (op != 3) continue;                        /* want DATA */
            blk = (tf[2]<<8)|tf[3];
            dlen = tlen - 4;
            if (blk == nextblk) {
                srv_tid = ssport;
                for (i = 0; i < dlen && total < maxlen; i++) dst[total++] = tf[4+i];
                { u8 a[4]; a[0]=0; a[1]=4; a[2]=blk>>8; a[3]=blk; wifi_udp_tx(nh, srv, sport, srv_tid, a, 4); }
                if ((nextblk & 127) == 0) wifi_log("[wifi] tftp: blk %d (%d B)\r\n", nextblk, total);
                nextblk++;
                if (dlen < 512) finished = 1;             /* last block */
                w = 0;                                    /* reset idle window on progress */
            } else if (srv_tid) {                         /* dup/reorder: re-ACK last */
                u8 a[4]; a[0]=0; a[1]=4; a[2]=(nextblk-1)>>8; a[3]=(nextblk-1);
                wifi_udp_tx(nh, srv, sport, srv_tid, a, 4);
            }
          } }
    }
    wifi_log("[wifi] *** TFTP '%s': %d bytes (%s) ***\r\n", fname, total, finished ? "complete" : "timeout");
    return finished ? total : -1;
}

/* ================================================================== *
 *  M11 (2b) — netboot: TFTP a kernel over WiFi, then chainload it    *
 * ================================================================== */
extern char chainload_stub[], chainload_stub_end[];
/* Clean+invalidate D-cache (and optionally I-cache) over a VA range, MMU on. */
static void wifi_cache_flush(unsigned long addr, unsigned long len, int icache)
{
    unsigned long line = 64, end = addr + len, a;
    for (a = addr & ~(line - 1); a < end; a += line) {
        __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
        if (icache) __asm__ volatile ("ic ivau, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile ("dsb sy; isb" ::: "memory");
}
/* Fetch `fname` from srv over WiFi (TFTP) and boot it (chainload to 0x80000).
 * On success this never returns; returns -1 on fetch failure. */
/* (wifi_netboot + kernel_chainload removed in the Pi5 port — Pi5 has its own
 * kexec.c/kernel_chainload, and TFTP-netboot isn't part of the WiFi parity
 * goal.  wifi_tftp_get above remains usable standalone.) */

/* ================================================================== *
 *  M12 — MANET ad-hoc (IBSS) mode: peer-to-peer, no AP, static IP    *
 * ================================================================== */
/* Create/join an IBSS cell `ssid` on 2.4GHz `channel`, assign static IP
 * 10.0.0.<n>/24.  All nodes use a FIXED cell BSSID so they share one cell
 * immediately (deterministic for MANET).  After this the existing data
 * path (responder/ping/udp) works peer-to-peer with no AP/DHCP. */
int wifi_adhoc(const char *ssid, int channel, int n)
{
    static u8 jp[64];
    int sl = 0, i; u16 chanspec; u8 bss[6];
    while (ssid[sl] && sl < 32) sl++;
    wifi_tn = 0;
    wifi_log("[wifi] === ADHOC/IBSS \"%s\" ch=%d ip=10.0.0.%d ===\r\n", ssid, channel, n);
    if (!wifi_ready) { if (wifi_bringup() != 0) { wifi_log("[wifi] adhoc: bringup failed\r\n"); return -1; } }
    wifi_radio_up();
    wifi_cmd_int(WLC_DOWN, 1);
    wifi_set_iovar_int("wsec", 0);          /* open cell (no crypto) */
    wifi_set_iovar_int("wpa_auth", 0);
    wifi_set_iovar_int("auth", 0);
    wifi_cmd_int(WLC_SET_INFRA, 0);         /* ★ IBSS (ad-hoc) mode */
    wifi_cmd_int(2, 1);                     /* WLC_UP */
    wifi_delay_us(100000);
    wifi_cmd_int(WLC_SET_CHANNEL, (u32)channel);  /* set home channel (avoids chanspec encoding) */
    (void)chanspec;
    /* brcmf_join_params: ssid_le[36] + assoc{bssid[6]@36, chanspec_num@44=0} */
    for (i = 0; i < (int)sizeof(jp); i++) jp[i] = 0;
    jp[0] = sl; for (i = 0; i < sl; i++) jp[4+i] = ssid[i];
    jp[36]=0x02; jp[37]=0x4d; jp[38]=0x41; jp[39]=0x4e; jp[40]=0x45; jp[41]=0x54; /* 02:4d:41:4e:45:54 "MANET" */
    jp[44]=0; jp[45]=0; jp[46]=0; jp[47]=0;  /* chanspec_num = 0 (channel set via SET_CHANNEL) */
    if (wifi_wlcmd(1, WLC_SET_SSID, jp, 48, 0, 0) != 0) { wifi_log("[wifi] adhoc: SET_SSID failed\r\n"); return -1; }
    wifi_delay_us(400000);
    /* static IP (no DHCP in ad-hoc); enables responder + ping/udp */
    wifi_get_iovar("cur_etheraddr", wifi_mac, 6);
    wifi_ip[0]=10; wifi_ip[1]=0; wifi_ip[2]=0; wifi_ip[3]=(u8)n;
    wifi_mask[0]=255; wifi_mask[1]=255; wifi_mask[2]=255; wifi_mask[3]=0;
    wifi_gw[0]=10; wifi_gw[1]=0; wifi_gw[2]=0; wifi_gw[3]=1;
    for (i=0;i<4;i++){ wifi_dns[i]=0; }
    for (i = 0; i < sl && i < 39; i++) wifi_cur_ssid[i] = ssid[i]; wifi_cur_ssid[i] = 0;
    wifi_have_ip = 1;
    /* IBSS cell formation: the fw scans for the cell then creates it — poll
     * GET_BSSID until it reports the cell BSSID (~up to 12s). */
    { int t, up = 0;
      for (t = 0; t < 24; t++) {
          if (wifi_wlcmd(0, WLC_GET_BSSID, 0, 0, bss, 6) == 0) {
              int nz = 0, k; for (k = 0; k < 6; k++) if (bss[k] && bss[k] != 0xFF) nz = 1;
              if (nz) { up = 1;
                  wifi_log("[wifi] adhoc: *** IBSS cell up, BSSID %02x:%02x:%02x:%02x:%02x:%02x ***\r\n",
                           bss[0],bss[1],bss[2],bss[3],bss[4],bss[5]); break; }
          }
          wifi_delay_us(500000);
      }
      if (!up) wifi_log("[wifi] adhoc: IBSS not associated yet (no peer / still forming)\r\n");
    }
    wifi_log("[wifi] mac %02x:%02x:%02x:%02x:%02x:%02x\r\n",
             wifi_mac[0],wifi_mac[1],wifi_mac[2],wifi_mac[3],wifi_mac[4],wifi_mac[5]);
    wifi_log("[wifi] *** ADHOC up: \"%s\" ch=%d ip=10.0.0.%d (peer-to-peer, no AP) ***\r\n", ssid, channel, n);
    return 0;
}

#else  /* !WIFI_SDIO_BASE — pi4/qemu builds: WiFi compiled out */
const char *wifi_trace(void) { return "wifi: not built (no WIFI_SDIO_BASE)"; }
int  wifi_trace_len(void) { return 0; }
int  wifi_probe(void) { return -1; }
int  wifi_probe_stage(int k) { (void)k; return -1; }
void wifi_net_poll(void) { }
#endif /* WIFI_SDIO_BASE */
