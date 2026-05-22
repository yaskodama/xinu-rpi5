// kernel/proc.c — cooperative scheduler over `proctab[]`.
//
// Pattern follows Embedded Xinu's system/resched.c / system/create.c:
//   - Single global ready list (FIFO in this Round-1 cut; Xinu uses
//     a priority queue — we can drop that in later without changing
//     callers).
//   - resched() saves the current SP into proctab[currpid].sp,
//     loads the next ready process's SP, and ctxsw()'s.
//   - create() pre-loads a fake "ctxsw save frame" on the new
//     stack so the first ctxsw INTO the new process pops it and
//     returns directly into the entry function.

#include "proc.h"
#include "memory.h"

struct procent proctab[NPROC];
int            currpid;

static struct procent *ready_head;
static struct procent *ready_tail;

static void copy_name(char *dst, const char *src)
{
    int i;
    for (i = 0; i < PROC_NAME_LEN - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void ready_push(struct procent *p)
{
    p->next = 0;
    if (ready_head == 0) {
        ready_head = ready_tail = p;
    } else {
        ready_tail->next = p;
        ready_tail = p;
    }
}

static struct procent *ready_pop(void)
{
    struct procent *p = ready_head;
    if (p == 0) return 0;
    ready_head = p->next;
    if (ready_head == 0) ready_tail = 0;
    p->next = 0;
    return p;
}

void proc_init(void)
{
    int i;
    for (i = 0; i < NPROC; i++) {
        proctab[i].state = PR_FREE;
        proctab[i].next  = 0;
    }

    /* NULLPROC = the live boot/shell context. We don't allocate a
     * stack for it (it inherits boot.S's stack at _start) and we
     * leave .sp = 0 until the first ctxsw OUT writes the real SP. */
    struct procent *p = &proctab[NULLPROC];
    p->state   = PR_CURR;
    p->prio    = 0;
    p->stkbase = 0;
    p->stklen  = 0;
    p->sp      = 0;
    copy_name(p->name, "null/shell");

    ready_head = ready_tail = 0;
    currpid    = NULLPROC;
}

static int alloc_slot(void)
{
    int i;
    for (i = 1; i < NPROC; i++) {
        if (proctab[i].state == PR_FREE) return i;
    }
    return -1;
}

int proc_create(proc_entry_t entry, unsigned long stksize, const char *name)
{
    int pid = alloc_slot();
    if (pid < 0) return -1;

    if (stksize < 1024) stksize = 1024;
    stksize = ROUNDMB(stksize);

    void *stk = getmem(stksize);
    if (stk == 0) return -1;

    struct procent *p = &proctab[pid];
    p->state   = PR_READY;
    p->prio    = 1;
    p->stkbase = stk;
    p->stklen  = stksize;
    copy_name(p->name, name);
    p->next    = 0;

    /* Lay out an initial saved-register frame at the top of the
     * stack, in the exact order ctxsw.S restores them:
     *   [sp + 0  ] x29 (FP)
     *   [sp + 8  ] x30 (LR)   <-- where `ret` jumps; we put `entry` here
     *   [sp + 16 ] x27
     *   [sp + 24 ] x28
     *   [sp + 32 ] x25
     *   [sp + 40 ] x26
     *   ...
     *   [sp + 88 ] x20
     * 12 quadwords = 96 bytes, keeping the 16-byte SP alignment. */
    unsigned long *sp_top = (unsigned long *)((unsigned char *)stk + stksize);
    unsigned long *sp     = sp_top - 12;
    sp[0]  = 0;                          /* x29 (FP)            */
    sp[1]  = (unsigned long)entry;       /* x30 (LR -> entry)   */
    sp[2]  = 0; sp[3]  = 0;              /* x27, x28            */
    sp[4]  = 0; sp[5]  = 0;              /* x25, x26            */
    sp[6]  = 0; sp[7]  = 0;              /* x23, x24            */
    sp[8]  = 0; sp[9]  = 0;              /* x21, x22            */
    sp[10] = 0; sp[11] = 0;              /* x19, x20            */
    p->sp = (void *)sp;

    ready_push(p);
    return pid;
}

void proc_ready(int pid)
{
    if (pid <= 0 || pid >= NPROC) return;
    struct procent *p = &proctab[pid];
    p->state = PR_READY;
    ready_push(p);
}

/* Pick the next ready process and ctxsw into it.  Returns once we
 * resume on the original stack.  If the ready list is empty, we
 * stay where we are (the no-op makes proc_yield() safe to call
 * unconditionally). */
void proc_resched(void)
{
    struct procent *newp = ready_pop();
    if (newp == 0) return;

    int new_pid       = (int)(newp - proctab);
    struct procent *oldp = &proctab[currpid];
    int old_pid       = currpid;

    /* If the current proc is still runnable (and isn't the null
     * proc — which never goes on the ready list), park it. */
    if (oldp->state == PR_CURR && old_pid != NULLPROC) {
        oldp->state = PR_READY;
        ready_push(oldp);
    }

    newp->state = PR_CURR;
    currpid     = new_pid;

    ctxsw(&oldp->sp, newp->sp);
    /* Returns here when somebody ctxsw()'s back to us. */
}

void proc_yield(void)
{
    proc_resched();
}

/* Process voluntarily exits.  Marks slot free, picks next ready
 * (or NULLPROC if none), and ctxsw away — never returns. */
void proc_exit(void)
{
    int me = currpid;
    proctab[me].state = PR_FREE;

    struct procent *newp = ready_pop();
    if (newp == 0) newp = &proctab[NULLPROC];

    newp->state = PR_CURR;
    currpid     = (int)(newp - proctab);

    /* Throw-away storage for the saved-SP write.  Nobody will read
     * proctab[me].sp again because we're PR_FREE. */
    static void *graveyard_sp;
    ctxsw(&graveyard_sp, newp->sp);

    /* Unreachable. */
    for (;;) __asm__ volatile ("wfe");
}
