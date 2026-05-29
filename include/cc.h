// include/cc.h — public interface to the on-device C compiler.

#ifndef XINU_RPI4_CC_H
#define XINU_RPI4_CC_H

/* `cc <file.c>` shell command. */
int cmd_cc(int argc, char **argv);

/* Compile C source (`src`, `srclen` bytes; need not be NUL-terminated)
 * and run it in place (JIT).  Program output from the builtins
 * print/putchar/puts is captured into `out` (NUL-terminated, capped at
 * `outcap`).  On success returns 0 and sets *retval to the program's
 * return value.  On compile error returns -1 and writes "cc: <message>"
 * into `out`.  Returns -2 on out-of-memory. */
int cc_run_source(const char *src, int srclen, char *out, int outcap, long *retval);

/* ---- resident actor program (backs /actor/load and /actor/send) ---- */

/* Compile AIPL-generated C and keep it resident: runs main() (which
 * spawns the actors) and keeps the code + actor state alive.  `out` gets
 * main()'s output plus a "[resident: N actor(s) live]" line.  Returns 0
 * on success, -1 on compile error (out = message). */
int cc_actor_load(const char *src, int srclen, char *out, int outcap);

/* Send a message to a resident actor: apply `method` (by name) with
 * integer `arg` to actor `actor`, rendering the result into `out`.
 * Returns 0, or -1 if no program is loaded / the method is unknown. */
int cc_actor_send_msg(int actor, const char *method, long a0, long a1, long a2,
                      char *out, int outcap);

#endif /* XINU_RPI4_CC_H */
