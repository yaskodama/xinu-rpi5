// include/avm.h — AIPL actor-bytecode VM + "Blender" polygon display
// (device/video/avm.c).  rpi5 port of the Pi4 display system.
#ifndef XINU_RPI5_AVM_H
#define XINU_RPI5_AVM_H

void avm_stage_reset(void);                                   /* begin a new upload */
int  avm_stage_put(int off, const unsigned char *b, int n);   /* stage a chunk      */
void avm_load_progress(int cur, int total);                   /* upload bar feed    */
int  avm_loadrun(int len);                                    /* load + run staged  */
void avm_draw_loadbar(int sw, int sh);                        /* upload progress bar */

#endif /* XINU_RPI5_AVM_H */
