/* Internal: service implementations exported to apps + per-app resource tracking. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "papp_abi.h"
#include "lx_app.h"

void lx_svc_init(lx_app_log_sink_t log_sink);

/* Interface registry lookup: NULL if the id is unknown or too old. */
const papp_iface_hdr_t *lx_svc_find(uint32_t id, uint16_t min_version);

/* Per-run bookkeeping */
void     lx_svc_begin_run(void);
void     lx_svc_end_run(uint32_t *leaked_allocs, uint32_t *leaked_bytes, uint32_t *leaked_files);
void     lx_svc_request_exit(void);
#define LX_FILE_BUF_SIZE (16 * 1024)
void    *lx_file_fast_io(FILE *f);  /* DMA-aligned stdio buffer, free after fclose */
uint32_t lx_svc_busy(void);   /* >0 while the app is inside a service call */

/* Every service entry point brackets its work with these (see lx_services.c). */
extern volatile uint32_t lx_svc_busy_count;
#define SVC_ENTER()  __atomic_add_fetch(&lx_svc_busy_count, 1, __ATOMIC_SEQ_CST)
#define SVC_LEAVE()  __atomic_sub_fetch(&lx_svc_busy_count, 1, __ATOMIC_SEQ_CST)

extern volatile bool lx_svc_exit_flag;     /* set by lx_svc_request_exit() */

extern const papp_if_disp_t lx_if_disp;
extern const papp_if_audi_t lx_if_audi;
extern const papp_if_inpt_t lx_if_inpt;
extern const papp_if_thrd_t lx_if_thrd;
extern const papp_if_ui_t lx_if_ui;
extern const papp_if_net_t lx_if_net;
extern const papp_if_ble_t lx_if_ble;     /* only on boards with BLE (lx_svc_ble_present) */

/* NET bookkeeping (lx_svc_net.c) */
void lx_svc_net_init(void);
int  lx_svc_net_end_run(void);              /* -> sockets / requests the app left open */
int  lx_svc_net_hold(void);                 /* Wi-Fi for the run (UI's picker): PAPP_E_* */

/* BLE bookkeeping (lx_svc_ble.c): the host runs only during a BLE app's run */
void lx_svc_ble_init(void);
int  lx_svc_ble_end_run(void);              /* -> 1 if the host was running */
bool lx_svc_ble_present(void);

/* THRD bookkeeping (lx_svc_thrd.c), driven by the App Manager */
void lx_thrd_begin_run(void *main_task);
void lx_thrd_suspend_all(bool suspend);    /* app threads only, not the main task; suspend waits until off the CPU */
void lx_task_wait_off_cpu(void *task);     /* spin until a (suspended) task has left the CPU */
int  lx_thrd_kill_all(void);               /* -> threads that were still alive */
int  lx_thrd_end_run(void);                /* -> sync objects freed */

/* In-game overlay (lx_svc_ui.c): presents open it when HOME asked for it */
void lx_overlay_poll(void);
void lx_overlay_reset(void);

/* Ends the run from any app thread with `code` (lx_app.c). */
void lx_app_post_exit(int code);
