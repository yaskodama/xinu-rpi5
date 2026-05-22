// system/xinu_compat.c — Minimal Xinu kernel compat layer.
//
// The xinu-raz network/ port (ARP/IPv4/ICMP/...) calls into an
// Embedded-Xinu kernel API for synchronisation (semaphore,
// mailbox), packet buffer allocation (bufpool), interrupt
// control, and (limited) threading.  This file stubs those
// APIs on top of xinu-rpi5's cooperative scheduler so the
// network code can build and run in a single-threaded mode.
//
// Semantics deliberately simplified:
//   - sem/mbox waits never block: caller is responsible for
//     checking count > 0 / empty == false, just like polling.
//   - create() returns a fake TID but doesn't actually spawn a
//     thread.  Network code that relies on daemon threads will
//     need to be driven from a polling loop (e.g. the wm tick)
//     in xinu-rpi5.
//   - kprintf is implemented with uart_puts and a tiny %d/%x/%s
//     formatter.

#include "kmalloc.h"
#include "uart.h"
#include "irq.h"

/* ===== Xinu type aliases ===== */
typedef int            syscall;
typedef unsigned int   semaphore;
typedef unsigned int   mailbox;
typedef int            tid_typ;
typedef unsigned int   uint;
typedef unsigned long  irqmask;
typedef int            qid_typ;
typedef unsigned char  uchar;
typedef unsigned long  ulong;
typedef unsigned short ushort;
typedef int            message;

#define SYSERR        ((syscall)-1)
#define OK            ((syscall)1)
#define NULLPROC      0
#define SFREE         0x01
#define SUSED         0x02
#define MAILBOX_FREE  0
#define MAILBOX_ALLOC 1
#define BFPFREE       1
#define BFPUSED       2

#define NSEM           64
#define NMBOX          32
#define NPOOL          16
#define MBOX_MAX_MSGS  128

/* ===== Semaphore ===== */

struct sement {
    char     state;
    int      count;
    qid_typ  queue;            /* unused in cooperative mode */
};
struct sement semtab[NSEM];

semaphore semcreate(int count)
{
    for (int i = 1; i < NSEM; i++) {     /* skip 0 to keep it as "invalid" */
        if (semtab[i].state != SUSED) {
            semtab[i].state = SUSED;
            semtab[i].count = count;
            return (semaphore)i;
        }
    }
    return (semaphore)SYSERR;
}

syscall semfree(semaphore s)
{
    if (s == 0 || s >= NSEM) return SYSERR;
    semtab[s].state = SFREE;
    return OK;
}

syscall semcount(semaphore s)
{
    if (s == 0 || s >= NSEM) return SYSERR;
    return semtab[s].count;
}

syscall wait(semaphore s)
{
    if (s == 0 || s >= NSEM) return SYSERR;
    /* Cooperative single-threaded: decrement and trust the caller
     * to have checked count > 0 first.  When count goes negative
     * the caller would have blocked in a real Xinu; here we keep
     * the value to surface the bug if it happens. */
    semtab[s].count--;
    return OK;
}

syscall signal(semaphore s)
{
    if (s == 0 || s >= NSEM) return SYSERR;
    semtab[s].count++;
    return OK;
}

syscall signaln(semaphore s, int n)
{
    if (s == 0 || s >= NSEM) return SYSERR;
    semtab[s].count += n;
    return OK;
}

/* ===== Mailbox ===== */

struct mbox {
    semaphore sender;
    semaphore receiver;
    uint      max;
    uint      count;
    uint      start;
    uchar     state;
    int      *msgs;
};
static struct mbox mboxtab[NMBOX];
static int         mbox_msgs_pool[NMBOX * MBOX_MAX_MSGS];

syscall mailboxInit(void)
{
    for (int i = 0; i < NMBOX; i++) mboxtab[i].state = MAILBOX_FREE;
    return OK;
}

syscall mailboxAlloc(uint count)
{
    if (count > MBOX_MAX_MSGS) return SYSERR;
    for (int i = 0; i < NMBOX; i++) {
        if (mboxtab[i].state == MAILBOX_FREE) {
            mboxtab[i].state = MAILBOX_ALLOC;
            mboxtab[i].max   = count;
            mboxtab[i].count = 0;
            mboxtab[i].start = 0;
            mboxtab[i].msgs  = &mbox_msgs_pool[i * MBOX_MAX_MSGS];
            mboxtab[i].sender   = semcreate((int)count);
            mboxtab[i].receiver = semcreate(0);
            return (mailbox)i;
        }
    }
    return (mailbox)SYSERR;
}

syscall mailboxFree(mailbox m)
{
    if (m >= NMBOX || mboxtab[m].state != MAILBOX_ALLOC) return SYSERR;
    semfree(mboxtab[m].sender);
    semfree(mboxtab[m].receiver);
    mboxtab[m].state = MAILBOX_FREE;
    return OK;
}

syscall mailboxSend(mailbox m, int msg)
{
    if (m >= NMBOX || mboxtab[m].state != MAILBOX_ALLOC) return SYSERR;
    struct mbox *b = &mboxtab[m];
    if (b->count >= b->max) return SYSERR;
    b->msgs[(b->start + b->count) % b->max] = msg;
    b->count++;
    signal(b->receiver);
    /* sender semaphore would normally be wait()ed by send caller
     * to throttle; in our single-thread model just decrement. */
    wait(b->sender);
    return OK;
}

syscall mailboxReceive(mailbox m)
{
    if (m >= NMBOX || mboxtab[m].state != MAILBOX_ALLOC) return SYSERR;
    struct mbox *b = &mboxtab[m];
    if (b->count == 0) return SYSERR;
    int msg = b->msgs[b->start];
    b->start = (b->start + 1) % b->max;
    b->count--;
    wait(b->receiver);
    signal(b->sender);
    return msg;
}

syscall mailboxCount(mailbox m)
{
    if (m >= NMBOX || mboxtab[m].state != MAILBOX_ALLOC) return SYSERR;
    return (int)mboxtab[m].count;
}

/* ===== Buffer pool ===== */

struct poolbuf {
    struct poolbuf *next;
    int             poolid;
};

struct bfpentry {
    uchar           state;
    uint            bufsize;
    uint            nbuf;
    struct poolbuf *next;       /* free list head */
    void           *head;       /* base of contiguous allocation */
    semaphore       freebuf;    /* number of available buffers   */
};
struct bfpentry bfptab[NPOOL];

int bfpalloc(uint bufsize, uint nbuf)
{
    int pool = -1;
    for (int i = 0; i < NPOOL; i++) {
        if (bfptab[i].state != BFPUSED) { pool = i; break; }
    }
    if (pool < 0) return SYSERR;

    unsigned long elem = bufsize + sizeof(struct poolbuf);
    unsigned long total = elem * nbuf;
    unsigned char *mem = (unsigned char *)kmalloc(total);
    if (!mem) return SYSERR;

    bfptab[pool].state   = BFPUSED;
    bfptab[pool].bufsize = bufsize;
    bfptab[pool].nbuf    = nbuf;
    bfptab[pool].head    = mem;
    bfptab[pool].freebuf = semcreate((int)nbuf);

    struct poolbuf *prev = 0;
    for (uint i = 0; i < nbuf; i++) {
        struct poolbuf *b = (struct poolbuf *)(mem + i * elem);
        b->poolid = pool;
        b->next   = prev;
        prev      = b;
    }
    bfptab[pool].next = prev;
    return pool;
}

void *bufget(int poolid)
{
    if (poolid < 0 || poolid >= NPOOL) return 0;
    struct bfpentry *p = &bfptab[poolid];
    if (p->state != BFPUSED || !p->next) return 0;
    struct poolbuf *b = p->next;
    p->next = b->next;
    if (p->freebuf) wait(p->freebuf);
    return (void *)(b + 1);
}

syscall buffree(void *buf)
{
    if (!buf) return SYSERR;
    struct poolbuf *b = (struct poolbuf *)buf - 1;
    int poolid = b->poolid;
    if (poolid < 0 || poolid >= NPOOL) return SYSERR;
    struct bfpentry *p = &bfptab[poolid];
    b->next = p->next;
    p->next = b;
    if (p->freebuf) signal(p->freebuf);
    return OK;
}

syscall bfpfree(int poolid)
{
    if (poolid < 0 || poolid >= NPOOL) return SYSERR;
    bfptab[poolid].state = BFPFREE;
    return OK;
}

/* ===== Thread stubs ===== */

tid_typ gettid(void)
{
    return (tid_typ)1;
}

tid_typ create(void *procaddr, uint ssize, int priority,
               const char *name, int nargs, ...)
{
    /* Stub: don't actually spawn a thread.  Network code's
     * daemon threads (e.g. arpDaemon) will need to be invoked
     * by the host (wm tick or shell command) in xinu-rpi5. */
    (void)procaddr; (void)ssize; (void)priority;
    (void)name;     (void)nargs;
    return (tid_typ)2;
}

syscall ready(tid_typ tid, int reschedflag)
{
    (void)tid; (void)reschedflag;
    return OK;
}

syscall kill(tid_typ tid)        { (void)tid; return OK; }
syscall sleep(uint ms)           { (void)ms;  return OK; }
syscall yield(void)              { return OK; }
syscall recvclr(void)            { return OK; }
syscall recvtime(uint maxwait)   { (void)maxwait; return OK; }
syscall send(tid_typ tid, message msg) { (void)tid; (void)msg; return OK; }
syscall getprio(tid_typ tid)     { (void)tid; return 0; }
syscall unsleep(tid_typ tid)     { (void)tid; return OK; }

/* ===== IRQ mask ===== */

irqmask disable(void)
{
    irqmask saved;
    __asm__ volatile ("mrs %0, daif" : "=r"(saved));
    irq_disable_all();
    return saved;
}

void restore(irqmask im)
{
    __asm__ volatile ("msr daif, %0" :: "r"(im) : "memory");
}

/* ===== kprintf (minimal printf-lite) ===== */

/* stdarg without <stdarg.h> (we build -nostdinc). */
typedef __builtin_va_list va_list;
#define va_start(ap, l)  __builtin_va_start(ap, l)
#define va_end(ap)       __builtin_va_end(ap)
#define va_arg(ap, t)    __builtin_va_arg(ap, t)

static void kp_putdec(unsigned long v)
{
    char buf[24]; int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) uart_putc(buf[n]);
}

static void kp_puthex(unsigned long v)
{
    char buf[24]; int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) {
        unsigned long d = v & 0xF;
        buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
    while (n--) uart_putc(buf[n]);
}

/* ===== libc-style helpers used by xinu-raz network code ===== */

void *bzero(void *s, unsigned long n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = 0;
    return s;
}

int memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) { if (*pa != *pb) return *pa - *pb; pa++; pb++; }
    return 0;
}

/* sprintf / sscanf — extremely minimal.  network/ uses them mostly
 * for diagnostic messages; we just write a placeholder so links
 * succeed.  Real format-string parsing can be added later. */
int sprintf(char *s, const char *fmt, ...)
{
    (void)fmt;
    if (s) s[0] = 0;
    return 0;
}
int sscanf(const char *s, const char *fmt, ...)
{
    (void)s; (void)fmt;
    return 0;
}
int snprintf(char *s, unsigned long size, const char *fmt, ...)
{
    (void)fmt;
    if (s && size) s[0] = 0;
    return 0;
}

/* ===== Clock ===== */

/* Forward decls so we don't need to include timer.h. */
unsigned long timer_ticks(void);

unsigned long clktime  = 0;   /* seconds since boot — updated by tick */
unsigned long clkticks = 0;
unsigned long clkcount(void) { return timer_ticks(); }

/* ===== Xinu device API stubs ===== */

struct dentry { int dummy; };   /* opaque; network code uses pointer/index only */
struct dentry devtab[64];        /* NDEV plenty; entries are stubs */

syscall open(int dev, ...)        { (void)dev; return SYSERR; }
syscall close(int dev)            { (void)dev; return SYSERR; }
syscall read(int dev, void *buf, uint len)  { (void)dev; (void)buf; (void)len; return SYSERR; }
syscall write(int dev, const void *buf, uint len){ (void)dev; (void)buf; (void)len; return SYSERR; }
syscall control(int dev, int func, long arg1, long arg2) { (void)dev; (void)func; (void)arg1; (void)arg2; return SYSERR; }
syscall getc_dev(int dev)         { (void)dev; return SYSERR; }
syscall putc_dev(int dev, char c) { (void)dev; (void)c; return SYSERR; }

/* ===== Routing / higher-layer protocol stubs ===== */

syscall rtInit(void)             { return OK; }
syscall rtAdd(void *a, void *b, void *c, int d) { (void)a; (void)b; (void)c; (void)d; return OK; }
syscall rtClear(void *e)         { (void)e; return OK; }
syscall rtDefault(void *a, int b){ (void)a; (void)b; return OK; }
syscall rtLookup(void *dst, void *gw) { (void)dst; (void)gw; return SYSERR; }
void    rtRecv(void *pkt)        { (void)pkt; }

void tcpRecv(void *pkt)          { (void)pkt; }
void tcpTimer(void)              { }
void udpRecv(void *pkt)          { (void)pkt; }
void rawRecv(void *pkt)          { (void)pkt; }

void snoopCapture(void *snoop, void *pkt) { (void)snoop; (void)pkt; }

/* ===== Kernel printf ===== */

syscall kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const char *p = fmt;
    while (*p) {
        if (*p != '%') { uart_putc(*p++); continue; }
        p++;
        int is_long = 0;
        if (*p == 'l') { is_long = 1; p++; }
        switch (*p) {
            case 'd':
            case 'i': {
                long v = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
                if (v < 0) { uart_putc('-'); kp_putdec((unsigned long)-v); }
                else        kp_putdec((unsigned long)v);
                break;
            }
            case 'u': {
                unsigned long v = is_long ? va_arg(ap, unsigned long)
                                          : (unsigned long)va_arg(ap, unsigned);
                kp_putdec(v);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long v = is_long ? va_arg(ap, unsigned long)
                                          : (unsigned long)va_arg(ap, unsigned);
                kp_puthex(v);
                break;
            }
            case 's': { const char *s = va_arg(ap, const char *);
                        uart_puts(s ? s : "(null)"); break; }
            case 'c': { int c = va_arg(ap, int); uart_putc((char)c); break; }
            case 'p': { void *v = va_arg(ap, void *);
                        uart_puts("0x"); kp_puthex((unsigned long)v); break; }
            case '%': uart_putc('%'); break;
            default:  uart_putc('%'); if (*p) uart_putc(*p); break;
        }
        if (*p) p++;
    }
    va_end(ap);
    return OK;
}
