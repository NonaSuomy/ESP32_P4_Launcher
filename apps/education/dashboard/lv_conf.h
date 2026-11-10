/*
 * LVGL 9.2 settings for the LVGL Demo app. Anything not set here keeps LVGL's default
 * (lvgl/src/lv_conf_internal.h). The app links its own copy of LVGL: the launcher's copy is
 * not shared with apps.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16                       /* RGB565, like the app's surface */

/* memory, strings and printf from the C library (the launcher tracks the app's malloc) */
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE                    /* one thread: the app's main loop */
#define LV_DEF_REFR_PERIOD 16                   /* ms between screen updates (60 Hz) */
#define LV_DPI_DEF 130

#define LV_USE_DRAW_SW 1                        /* the CPU draws */
#define LV_DRAW_SW_COMPLEX 1                    /* rounded corners, shadows, arcs */

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

/* fonts */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_DEFAULT &lv_font_montserrat_20

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

/* not needed */
#define LV_USE_SYSMON 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0

#endif
