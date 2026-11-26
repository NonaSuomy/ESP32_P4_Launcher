#pragma once
/* POSIX threads for FFmpeg on THRD (pthread_shim.c) */
void shim_init(void);               /* once, before the first thread or lock */
void shim_set_prio(int prio);       /* PAPP_PRIO_* of the threads made from now on */
