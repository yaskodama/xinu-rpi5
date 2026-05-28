// include/actorproc.h — actors as Xinu processes (mailbox + select).

#ifndef XINU_RPI4_ACTORPROC_H
#define XINU_RPI4_ACTORPROC_H

struct ap_msg { long method, a0, a1, a2, a3; };

/* Behaviour callback: invoked by each actor process for every received
 * message.  (The AIPL JIT `dispatch` has exactly this shape; its return
 * value is ignored for async delivery.) */
void ap_set_dispatch(long (*fn)(long self, long method, long a0, long a1, long a2, long a3));

void ap_reset(void);                 /* drop all actors (no free)        */
void ap_killall(void);               /* reap all actor processes + stacks */
int  ap_spawn(void);                 /* new actor process; returns id/-1  */
void ap_send(long to, long method, long a0, long a1, long a2, long a3);
void ap_run(void);                   /* drive actors until quiescent      */

/* Selective receive used by AIPL `select`: block until a message whose
 * method is one of meths[0..n); returns the matched method, *out gets it. */
long ap_select(long self, int n, const long *meths, struct ap_msg *out);

#endif /* XINU_RPI4_ACTORPROC_H */
