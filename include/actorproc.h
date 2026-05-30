// include/actorproc.h — actors as Xinu processes (mailbox + select).

#ifndef XINU_RPI4_ACTORPROC_H
#define XINU_RPI4_ACTORPROC_H

struct ap_msg { long method, a0, a1, a2, a3, reply_to; };

/* Behaviour callback: invoked by each actor process for every received
 * message.  (The AIPL JIT `dispatch` has exactly this shape; its return
 * value is ignored for async delivery.) */
void ap_set_dispatch(long (*fn)(long self, long method, long a0, long a1, long a2, long a3));

void ap_reset(void);                 /* drop all actors (no free)        */
void ap_killall(void);               /* reap all actor processes + stacks */
int  ap_spawn(void);                 /* new actor process; returns id/-1  */
/* Terminate actor `id` and free its slot for reuse by a later ap_spawn.
 * Called from inside the actor's own dispatch handler (suicide pattern);
 * the receive loop notices the dead flag, releases the vheap lock, and
 * proc_exits.  Safe to call only on `id == self`. */
void ap_suicide(int id);
void ap_send(long to, long method, long a0, long a1, long a2, long a3);
/* Synchronous call: deliver to `to`, block `self` until `to` replies with
 * its method's return value, and return that value.  (AIPL `now`.) */
long ap_call(long self, long to, long method, long a0, long a1, long a2, long a3);
void ap_run(void);                   /* drive actors until quiescent      */

/* Introspection for the wm "Actors" window. */
int  ap_live_count(void);                                /* # live actors    */
int  ap_actor_stat(int i, int *pid, int *qlen, int *waiting, unsigned int *nmsg);
void ap_note_msg(int actor);         /* count a direct-dispatch message      */

/* Selective receive used by AIPL `select`: block until a message whose
 * method is one of meths[0..n); returns the matched method, *out gets it. */
long ap_select(long self, int n, const long *meths, struct ap_msg *out);

/* let-it-crash: abandon the current actor's in-flight handler and return to
 * its receive loop (the process stays alive).  A synchronous `now` caller is
 * unblocked with AP_CRASH_REPLY so a supervisor can detect + retry.  No-op if
 * not called from inside an actor handler. */
void ap_crash(void);
#define AP_CRASH_REPLY 0x80000001L   /* == v_int(0x40000000): supervisor sentinel */

#endif /* XINU_RPI4_ACTORPROC_H */
