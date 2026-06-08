// device/sd/sd.c — read-only SDHCI block driver.
//
// Bare minimum to talk to a pre-initialised Pi 4 EMMC2 controller.
// We never touch the controller's clocks, power, or command-init
// sequence — the firmware bootloader handled all that to read
// kernel8.img off the card.  Issuing CMD17 against an already-
// selected card with block addressing in effect just works (as long
// as no other code has messed with the controller in between).

#include "sd.h"

#ifdef SD_BASE

/* SDHCI v3 register layout (offsets from SD_BASE).  Reference:
 * Broadcom BCM2711 ARM peripherals chapter 5 "External Mass Media
 * Controller", + the standard SDHCI spec for shared register meaning. */
#define EMMC_ARG2          (*(volatile unsigned int *)(SD_BASE + 0x00))
#define EMMC_BLKSIZECNT    (*(volatile unsigned int *)(SD_BASE + 0x04))
#define EMMC_ARG1          (*(volatile unsigned int *)(SD_BASE + 0x08))
#define EMMC_CMDTM         (*(volatile unsigned int *)(SD_BASE + 0x0C))
#define EMMC_RESP0         (*(volatile unsigned int *)(SD_BASE + 0x10))
#define EMMC_DATA          (*(volatile unsigned int *)(SD_BASE + 0x20))
#define EMMC_STATUS        (*(volatile unsigned int *)(SD_BASE + 0x24))
#define EMMC_INTERRUPT     (*(volatile unsigned int *)(SD_BASE + 0x30))

/* CMDTM bit fields per SDHCI spec.
 *   bits 24-31 = command index
 *   bit  21    = data direction is data (ISDATA)
 *   bit  20    = check response index against CMD index
 *   bit  19    = check response CRC7
 *   bits 16-17 = response type (10 = 48 bit, 01 = 136 bit, 00 = none)
 *   bit  4     = transfer direction (0 = host->card, 1 = card->host)
 */
#define CMDTM_CMD17_READ_SINGLE_BLOCK \
    ((17u << 24) | (1u << 21) | (1u << 20) | (1u << 19) | (2u << 16) | (1u << 4))
/* CMD24 WRITE_BLOCK: same flags but data direction host->card (bit4 = 0). */
#define CMDTM_CMD24_WRITE_SINGLE_BLOCK \
    ((24u << 24) | (1u << 21) | (1u << 20) | (1u << 19) | (2u << 16) | (0u << 4))

/* INTERRUPT register bits we care about (low half is normal events,
 * high half is errors). */
#define INT_CMD_DONE       (1u << 0)
#define INT_DATA_DONE      (1u << 1)
#define INT_WRITE_RDY      (1u << 4)
#define INT_READ_RDY       (1u << 5)
#define INT_ERROR_MASK     0xFFFF8000u   /* any of bits 15..31 = error */

/* STATUS (0x24) inhibit bits — a new command/data transfer may only be issued
 * once these are clear. */
#define SR_CMD_INHIBIT     (1u << 0)
#define SR_DAT_INHIBIT     (1u << 1)

/* Spin budgets — generous enough for a slow card but bounded so a
 * dead controller doesn't hang the boot forever. */
#define POLL_LIMIT         5000000UL

static int wait_intr(unsigned int flag)
{
    for (unsigned long t = 0; t < POLL_LIMIT; t++) {
        unsigned int r = EMMC_INTERRUPT;
        if (r & INT_ERROR_MASK) return -1;
        if (r & flag) {
            EMMC_INTERRUPT = flag;   /* w1c — clear the bit we waited on */
            return 0;
        }
    }
    return -1;
}

/* Wait for the command (and optionally data) line to be free before issuing. */
static int wait_ready(unsigned int mask)
{
    for (unsigned long t = 0; t < POLL_LIMIT; t++)
        if (!(EMMC_STATUS & mask)) return 0;
    return -1;
}

/* ---- full controller + SD-card init (Pi 5: firmware leaves sdio1 with the
 *      clock off and CMD-inhibit stuck, so we must bring it up ourselves).
 *
 * This mirrors the *proven* brcmstb host bring-up in device/wifi/wifi.c — the
 * WiFi side (sdio2) and the SD card (sdio1) are the SAME sdhci-brcmstb IP, so
 * the controller quirks are identical.  The non-obvious ones (learnt the hard
 * way on the WiFi port) that the first naive sd_init got wrong:
 *   - the byte/halfword registers must be written as 32-bit RMW or the store
 *     doesn't commit (POWER read back as 0x0e — the power-on bit never latched);
 *   - cfg must be set up BEFORE power: force card-presence (the slot is wired
 *     non-removable here), tell the controller its 200 MHz base clock, and set
 *     the PHY output delay — without these power-on won't stick;
 *   - the controller MIS-SAMPLES at the 400 kHz identify clock (shifted CMD
 *     responses -> CMD never completes -> CMD_INHIBIT stuck, exactly the
 *     symptom the diag showed), so run identification at ~6 MHz instead. ---- */
#define SDREG(off)   (*(volatile unsigned int *)(SD_BASE + (off)))
#ifdef SD_CFG_BASE
#define CFGREG(off)  (*(volatile unsigned int *)(SD_CFG_BASE + (off)))
#endif
#define SD_RESP0           SDREG(0x10)
#define SD_BASE_HZ         200000000u            /* clk_emmc2 (same as the WiFi sdio2) */

/* sub-word RMW accessors (brcmstb commits only 32-bit aligned stores) */
static unsigned int sd_r8 (unsigned int off){ return (SDREG(off & ~3u) >> ((off&3)*8)) & 0xFFu; }
static unsigned int sd_r16(unsigned int off){ return (SDREG(off & ~3u) >> ((off&3)*8)) & 0xFFFFu; }
static void sd_w8 (unsigned int off, unsigned int v){
    unsigned int wo = off & ~3u, sh = (off&3)*8;
    SDREG(wo) = (SDREG(wo) & ~(0xFFu<<sh)) | ((v&0xFFu)<<sh);
}
static void sd_w16(unsigned int off, unsigned int v){
    unsigned int wo = off & ~3u, sh = (off&3)*8;
    SDREG(wo) = (SDREG(wo) & ~(0xFFFFu<<sh)) | ((v&0xFFFFu)<<sh);
}

/* register offsets / bits (standard SDHCI, brcmstb single-Cmdtm @ 0x0C) */
#define SD_CLOCK_CONTROL   0x2C   /* 16-bit */
#define SD_SOFTWARE_RESET  0x2F   /* 8-bit  */
#define SD_POWER_CONTROL   0x29   /* 8-bit  */
#define SD_HOST_CONTROL    0x28   /* 8-bit  */
#define SD_TIMEOUT_CTRL    0x2E   /* 8-bit  */
#define R_NONE    0u
#define R_48      (2u << 16)
#define R_136     (1u << 16)
#define R_48BUSY  (3u << 16)
#define F_CRC     (1u << 19)
#define F_IXCHK   (1u << 20)
#define CC_INT_CLK_EN      (1u << 0)
#define CC_INT_CLK_STABLE  (1u << 1)
#define CC_SD_CLK_EN       (1u << 2)
#define SRST_ALL           0x01
#define PWR_ON             0x01
#define PWR_330            0x0E
#define HC_4BIT            0x02

#ifdef SD_CFG_BASE
/* Pi5 SD card power + I/O-voltage regulators (from bcm2712-rpi-5-b.dts &sdio1):
 * they hang off the always-on GPIO block gio_aon@7d517c00 (brcmstb-gpio; bank0
 * DATA=+0x04, IODIR=+0x08, IODIR 0=output), which the SDHCI controller itself
 * cannot drive:
 *   sd_vcc_reg    = gio_aon GPIO4, enable-active-high   -> card VDD (3.3V)
 *   sd_io_1v8_reg = gio_aon GPIO3, state 1=1.8V / 0=3.3V -> I/O signalling
 * The firmware hands the card over in UHS 1.8V signalling (GPIO3 high), but the
 * controller comes up in 3.3V mode -> mismatch -> the card answers nothing
 * (exactly our "every command times out" symptom).  Force the I/O rail back to
 * 3.3V and power-cycle VDD so the card cold-restarts in 3.3V default mode. */
#define SD_GIO_AON_BASE 0x107D517C00UL
#define GIOAON(off) (*(volatile unsigned int *)(SD_GIO_AON_BASE + (off)))
#define AON_DATA   0x04
#define AON_IODIR  0x08
static void sd_busy(unsigned long n){ while (n--) asm volatile("nop"); }
static void sd_card_power(void)
{
    /* I/O voltage rail -> 3.3V: gio_aon GPIO3 = low, output */
    GIOAON(AON_DATA)  &= ~(1u << 3);
    GIOAON(AON_IODIR) &= ~(1u << 3);
    /* VDD power-cycle: GPIO4 output, low (off) -> settle -> high (on) -> ramp */
    GIOAON(AON_IODIR) &= ~(1u << 4);
    GIOAON(AON_DATA)  &= ~(1u << 4);    sd_busy(30000000);   /* VDD off ~tens of ms */
    GIOAON(AON_DATA)  |=  (1u << 4);    sd_busy(30000000);   /* VDD on + power-up    */
}
#endif

/* Real microsecond delay off the generic timer (the nop-loop delay is far too
 * short and unreliable for the ACMD41 power-up wait, which the SD spec allows up
 * to 1 second). */
static void sd_delay_us(unsigned int us)
{
    unsigned long f, t0, t;
    __asm__ volatile("mrs %0, cntfrq_el0":"=r"(f));
    __asm__ volatile("mrs %0, cntpct_el0":"=r"(t0));
    if (!f) f = 54000000UL;
    unsigned long d = (f/1000000UL) * us;
    do { __asm__ volatile("mrs %0, cntpct_el0":"=r"(t)); } while (t - t0 < d);
}

static unsigned int sd_divisor(unsigned int hz)
{
    unsigned int d = 1; if (!hz) hz = 400000;
    while ((SD_BASE_HZ / (2u*d)) > hz && d < 1023) d++;
    return d;
}
static int sd_set_clock(unsigned int hz)
{
    unsigned int div = sd_divisor(hz), c, t;
    sd_w16(SD_CLOCK_CONTROL, 0);                             /* stop clock         */
    c = CC_INT_CLK_EN | ((div & 0xFF) << 8) | (((div >> 8) & 0x3) << 6);
    sd_w16(SD_CLOCK_CONTROL, c);
    for (t = 0; t < 1000000; t++) if (sd_r16(SD_CLOCK_CONTROL) & CC_INT_CLK_STABLE) break;
    if (!(sd_r16(SD_CLOCK_CONTROL) & CC_INT_CLK_STABLE)) return -1;
    sd_w16(SD_CLOCK_CONTROL, sd_r16(SD_CLOCK_CONTROL) | CC_SD_CLK_EN);
    for (t = 0; t < 200000; t++) ;                           /* settle             */
    return 0;
}

/* Issue a command; returns 0 on CMD_DONE, -1 on error/timeout.  resp48 (if not
 * NULL) gets RESP0. */
static int sd_cmd(unsigned int idx, unsigned int arg, unsigned int flags, unsigned int *resp48)
{
    unsigned int r; unsigned long t;
    if (wait_ready(SR_CMD_INHIBIT | ((flags & (1u<<21)) ? SR_DAT_INHIBIT : 0)) != 0) return -1;
    EMMC_INTERRUPT = 0xFFFFFFFFu;
    EMMC_ARG1  = arg;
    EMMC_CMDTM = (idx << 24) | flags;
    for (t = 0; t < POLL_LIMIT; t++) {
        r = EMMC_INTERRUPT;
        if (r & INT_ERROR_MASK) return -1;
        if (r & INT_CMD_DONE) { EMMC_INTERRUPT = INT_CMD_DONE; break; }
    }
    if (t >= POLL_LIMIT) return -1;
    if (resp48) *resp48 = SD_RESP0;
    /* R1b: wait out the busy on DAT0 */
    if ((flags & (3u<<16)) == R_48BUSY) (void)wait_ready(SR_DAT_INHIBIT);
    return 0;
}

int sd_init(void)
{
    unsigned int ocr = 0, rca, t; unsigned char block[SD_BLOCK_SIZE];
    extern void uart_puts(const char *);

#ifdef SD_CFG_BASE
    /* Power the card (3.3V VDD + 3.3V I/O) before touching the controller. */
    sd_card_power();
    /* brcmstb cfg (must precede power): SD pin mode + force card presence +
     * 200 MHz base clock + PHY output delay. */
    CFGREG(0x44) = (CFGREG(0x44) & ~0x3u) | 0x2u;            /* SD_PIN_SEL = SD     */
    CFGREG(0x00) = (CFGREG(0x00) & ~(1u<<30)) | (1u<<31);    /* force presence      */
    CFGREG(0x4C) = (3u << 12) | 200u;                        /* CQ_CAP base=200MHz  */
    CFGREG(0x34) = 0x80000003u;                              /* OP_DLY default      */
#endif
    /* Full host-controller reset (clears the stuck CMD-inhibit). */
    sd_w8(SD_SOFTWARE_RESET, SRST_ALL);
    for (t = 0; t < 1000000; t++) if (!(sd_r8(SD_SOFTWARE_RESET) & SRST_ALL)) break;
    if (sd_r8(SD_SOFTWARE_RESET) & SRST_ALL) { uart_puts("sd: SRST stuck\n"); return -1; }

    /* power on at 3.3 V, 1-bit bus for identify (byte writes via 32-bit RMW). */
    sd_w8(SD_POWER_CONTROL, PWR_330 | PWR_ON);
    sd_w8(SD_HOST_CONTROL,  0);
    sd_w8(SD_TIMEOUT_CTRL,  0x0E);

    SDREG(0x34) = 0xFFFFFFFFu;                               /* INT_ENABLE          */
    SDREG(0x38) = 0;                                         /* SIGNAL_ENABLE (poll) */
    EMMC_INTERRUPT = 0xFFFFFFFFu;

    /* Identify at ~6 MHz — the brcmstb mis-samples at the spec 400 kHz id clock. */
    if (sd_set_clock(6000000) != 0) { uart_puts("sd: id clk unstable\n"); return -1; }

    /* --- SD card init --- */
    if (sd_cmd(0,  0,          R_NONE,                0) != 0) { uart_puts("sd: CMD0 fail\n"); return -1; }
    /* CMD8: voltage check (0x1AA).  Ignore failure (SD v1 cards don't answer). */
    sd_cmd(8, 0x1AA, R_48 | F_CRC | F_IXCHK, 0);
    /* ACMD41 loop (CMD55 then CMD41) until the card powers up (OCR bit31).  Poll
     * at ~2 ms for up to ~1.5 s (the spec's max power-up time is 1 s). */
    for (t = 0; t < 750; t++) {
        if (sd_cmd(55, 0, R_48 | F_CRC | F_IXCHK, 0) != 0) { uart_puts("sd: CMD55 fail\n"); return -1; }
        if (sd_cmd(41, 0x40FF8000u, R_48, &ocr) != 0)       { uart_puts("sd: ACMD41 fail\n"); return -1; }
        if (ocr & 0x80000000u) break;                       /* card ready          */
        sd_delay_us(2000);
    }
    if (!(ocr & 0x80000000u)) { uart_puts("sd: ACMD41 not ready\n"); return -1; }
    if (sd_cmd(2, 0,   R_136 | F_CRC,         0)    != 0) { uart_puts("sd: CMD2 fail\n"); return -1; }
    if (sd_cmd(3, 0,   R_48  | F_CRC | F_IXCHK, &rca) != 0) { uart_puts("sd: CMD3 fail\n"); return -1; }
    rca &= 0xFFFF0000u;
    if (sd_cmd(7, rca, R_48BUSY | F_CRC | F_IXCHK, 0) != 0) { uart_puts("sd: CMD7 fail\n"); return -1; }
    /* CMD16: set block length 512 (harmless on SDHC, required on SDSC). */
    sd_cmd(16, SD_BLOCK_SIZE, R_48 | F_CRC | F_IXCHK, 0);
    sd_set_clock(25000000);                                 /* operational 25 MHz  */

    /* Verify with an MBR read. */
    if (sd_read_block(0, block) != 0) { uart_puts("sd: read LBA0 fail\n"); return -1; }
    if (block[510] != 0x55 || block[511] != 0xAA) { uart_puts("sd: no MBR sig\n"); return -1; }
    uart_puts("sd: card initialised (MBR ok)\n");
    return 0;
}

int sd_read_block(unsigned long lba, void *buf)
{
    /* Don't issue until the previous transfer has fully drained. */
    if (wait_ready(SR_CMD_INHIBIT | SR_DAT_INHIBIT) != 0) return -1;
    /* Clear any stale interrupt status from the previous transaction. */
    EMMC_INTERRUPT = 0xFFFFFFFFu;

    /* One block of 512 bytes. */
    EMMC_BLKSIZECNT = (1u << 16) | SD_BLOCK_SIZE;

    /* SDHC/SDXC cards (which Pi firmware leaves the EMMC speaking to)
     * use block addressing — the ARG is the LBA directly, not a byte
     * offset.  Block address >32 bits is unusual but Pi 4 supports it. */
    EMMC_ARG1 = (unsigned int)lba;

    /* Issue CMD17. */
    EMMC_CMDTM = CMDTM_CMD17_READ_SINGLE_BLOCK;

    /* Wait for the command to be accepted. */
    if (wait_intr(INT_CMD_DONE) != 0) return -1;

    /* Wait for the data buffer to fill. */
    if (wait_intr(INT_READ_RDY) != 0) return -1;

    /* Drain 128 32-bit words from the data port. */
    unsigned int *p = (unsigned int *)buf;
    for (int i = 0; i < SD_BLOCK_SIZE / 4; i++) {
        p[i] = EMMC_DATA;
    }

    /* Wait for the controller to acknowledge end of transfer. */
    if (wait_intr(INT_DATA_DONE) != 0) return -1;

    return 0;
}

int sd_write_block(unsigned long lba, const void *buf)
{
    if (wait_ready(SR_CMD_INHIBIT | SR_DAT_INHIBIT) != 0) return -1;
    EMMC_INTERRUPT = 0xFFFFFFFFu;
    EMMC_BLKSIZECNT = (1u << 16) | SD_BLOCK_SIZE;
    EMMC_ARG1 = (unsigned int)lba;                    /* block (LBA) addressing */
    EMMC_CMDTM = CMDTM_CMD24_WRITE_SINGLE_BLOCK;
    if (wait_intr(INT_CMD_DONE) != 0)  return -1;     /* command accepted        */
    if (wait_intr(INT_WRITE_RDY) != 0) return -1;     /* FIFO ready for our data  */
    const unsigned int *p = (const unsigned int *)buf;
    for (int i = 0; i < SD_BLOCK_SIZE / 4; i++)
        EMMC_DATA = p[i];                             /* push 128 words           */
    if (wait_intr(INT_DATA_DONE) != 0) return -1;     /* transfer complete        */
    /* The card now programs the block (busy on DAT0); wait for it to release so
     * the next access doesn't hit DAT-inhibit. */
    (void)wait_ready(SR_DAT_INHIBIT);
    return 0;
}

/* ---- diagnostics ---- */
extern void uart_puts(const char *);
extern void uart_putc(char);
static void sd_hx(const char *label, unsigned int v)
{
    uart_puts(label);
    for (int i = 7; i >= 0; i--) { unsigned int n = (v >> (i*4)) & 0xF; uart_putc(n < 10 ? '0'+n : 'a'+n-10); }
    uart_putc('\n');
}
void sd_diag(void)
{
    uart_puts("=== sd diag ===\n");
    /* ---- fresh verbose init: re-run the bring-up from scratch, dumping every
     *      command's INT-status + response so we can see exactly where (and why)
     *      it wedges, over HTTP. ---- */
    uart_puts("sd: --- fresh init ---\n");
#ifdef SD_CFG_BASE
    sd_hx("sd: AON pre DATA=0x", GIOAON(AON_DATA));
    sd_card_power();
    sd_hx("sd: AON post DATA=0x", GIOAON(AON_DATA));
    CFGREG(0x44) = (CFGREG(0x44) & ~0x3u) | 0x2u;
    CFGREG(0x00) = (CFGREG(0x00) & ~(1u<<30)) | (1u<<31);
    CFGREG(0x4C) = (3u << 12) | 200u;
    CFGREG(0x34) = 0x80000003u;
#endif
    sd_w8(SD_SOFTWARE_RESET, SRST_ALL);
    { unsigned long t; for (t = 0; t < 1000000; t++) if (!(sd_r8(SD_SOFTWARE_RESET) & SRST_ALL)) break; }
    sd_hx("sd: post-reset SRST=0x", sd_r8(SD_SOFTWARE_RESET));
    sd_w8(SD_POWER_CONTROL, PWR_330 | PWR_ON);
    sd_w8(SD_HOST_CONTROL,  0);
    sd_w8(SD_TIMEOUT_CTRL,  0x0E);
    sd_hx("sd: POWER=0x", sd_r8(SD_POWER_CONTROL));
    SDREG(0x34) = 0xFFFFFFFFu; SDREG(0x38) = 0; EMMC_INTERRUPT = 0xFFFFFFFFu;
    sd_hx("sd: set_clock rc=0x", (unsigned int)sd_set_clock(6000000));
    sd_hx("sd: CLKCTL now =0x", SDREG(0x2C));
    sd_hx("sd: HOSTCTL2 =0x", sd_r16(0x3E));   /* bit3 = 1.8V signalling enable */

    /* issue one command; on error, recover the CMD circuit (SRST_CMD) so the
     * NEXT command can run (SDHCI requires this after a command timeout). */
#define DIAG_CMD(IDX, ARG, FLAGS) do {                                               \
        unsigned long _t; unsigned int _r = 0;                                       \
        EMMC_INTERRUPT = 0xFFFFFFFFu;                                                \
        EMMC_ARG1  = (ARG);                                                          \
        EMMC_CMDTM = ((unsigned)(IDX) << 24) | (FLAGS);                              \
        for (_t = 0; _t < 2000000; _t++) { _r = EMMC_INTERRUPT;                      \
            if (_r & (INT_CMD_DONE|INT_ERROR_MASK)) break; }                         \
        uart_puts("sd: CMD" #IDX " INT=0x");                                         \
        { unsigned int _v=_r; for (int _i=7;_i>=0;_i--){unsigned int _n=(_v>>(_i*4))&0xF; uart_putc(_n<10?'0'+_n:'a'+_n-10);} } \
        uart_puts(" R=0x");                                                          \
        { unsigned int _v=SD_RESP0; for (int _i=7;_i>=0;_i--){unsigned int _n=(_v>>(_i*4))&0xF; uart_putc(_n<10?'0'+_n:'a'+_n-10);} } \
        uart_putc('\n');                                                             \
        if (_r & INT_ERROR_MASK) {  /* recover CMD line: SRST_CMD (bit1 @0x2F) */   \
            sd_w8(SD_SOFTWARE_RESET, 0x02);                                          \
            for (_t = 0; _t < 100000; _t++) if (!(sd_r8(SD_SOFTWARE_RESET) & 0x02)) break; \
        }                                                                            \
    } while (0)

    { unsigned long d; for (d = 0; d < 400000; d++) ; }   /* >74 clk before CMD0 */
    DIAG_CMD(0,  0,          R_NONE);
    DIAG_CMD(8,  0x1AA,      R_48 | F_CRC | F_IXCHK);
#undef DIAG_CMD
    /* Full ACMD41 ready-loop (card sets OCR bit31 when powered up), then the rest
     * of the real init sequence, reporting where it lands. */
    { unsigned int ocr=0, rca=0, r=0, resp; unsigned long t; int iter;
      for (iter=0; iter<750; iter++) {
          if (sd_cmd(55,0,R_48|F_CRC|F_IXCHK,0)!=0) { uart_puts("sd: CMD55 fail\n"); break; }
          if (sd_cmd(41,0x40FF8000u,R_48,&ocr)!=0)  { uart_puts("sd: ACMD41 fail\n"); break; }
          if (ocr & 0x80000000u) break;
          sd_delay_us(2000);
      }
      sd_hx("sd: ACMD41 iters=0x", (unsigned)iter);
      sd_hx("sd: ACMD41 OCR =0x", ocr);
      if (ocr & 0x80000000u) {
          r = sd_cmd(2,0,R_136|F_CRC,0);          sd_hx("sd: CMD2 rc=0x", r);
          r = sd_cmd(3,0,R_48|F_CRC|F_IXCHK,&rca); sd_hx("sd: CMD3 rc=0x", r);
          sd_hx("sd: RCA       =0x", rca & 0xFFFF0000u);
          r = sd_cmd(7,rca&0xFFFF0000u,R_48BUSY|F_CRC|F_IXCHK,0); sd_hx("sd: CMD7 rc=0x", r);
          sd_cmd(16,SD_BLOCK_SIZE,R_48|F_CRC|F_IXCHK,0);
          sd_set_clock(25000000);
          { unsigned char blk[SD_BLOCK_SIZE];
            r = (unsigned)sd_read_block(0, blk);
            sd_hx("sd: readLBA0 rc=0x", r);
            if (r==0) { sd_hx("sd: MBR[510]=0x", blk[510]); sd_hx("sd: MBR[511]=0x", blk[511]); } }
      }
      (void)resp; (void)t;
    }
    uart_puts("=== sd diag end ===\n");
}

#else /* !SD_BASE — QEMU / boards with no controller we can talk to */

int sd_init(void)                                       { return -1; }
int sd_read_block(unsigned long lba, void *buf)         { (void)lba; (void)buf; return -1; }
int sd_write_block(unsigned long lba, const void *buf)  { (void)lba; (void)buf; return -1; }

#endif
