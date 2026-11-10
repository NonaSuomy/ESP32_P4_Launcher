/*
 * LVGL Demo (education): a user interface built with LVGL, the graphics library the launcher's own
 * home screen uses. The app links its own copy of LVGL 9.2 (see lv_conf.h and build.py).
 *
 * How an LVGL app is put together:
 *   1. lv_init(), and a clock for LVGL (lv_tick_set_cb).
 *   2. A display: LVGL draws straight into our 800x480 surface (direct mode: only the parts that
 *      changed), and flush() shows it when a frame is done.
 *   3. Input devices: the touch screen is a pointer; the pad and a USB keyboard are a keypad.
 *   4. Widgets: objects made with lv_<widget>_create(parent), placed with lv_obj_align/set_pos,
 *      and event callbacks (lv_obj_add_event_cb) that run our code when something happens.
 *   5. The main loop calls lv_timer_handler(): it reads the inputs, runs timers, redraws.
 *
 * Touch, or pad: Up / Down = next / previous widget, Left / Right = change it, A = press,
 * B = back, L1 / R1 = tabs, X = exit.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "papp/papp.h"
#include "lvgl.h"

PAPP_MANIFEST(.id = "edu.lvgl", .name = "LVGL Demo", .version = PAPP_APP_VERSION, .main_stack = 32768);

#define W 800
#define H 480
#define TABS 3

static papp_surface_t *s_surf;
static lv_display_t *s_disp;
static bool s_quit;

/* ── 1. Clock ──────────────────────────────────────────────────────────── */

static uint32_t tick_ms(void) { return (uint32_t)(papp_time_us() / 1000); }

/* ── 2. Display ────────────────────────────────────────────────────────── */

/* LVGL has drawn an area into the surface's buffer: show the frame when it's the last area */
static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    if (lv_display_flush_is_last(d)) papp_disp_present(s_surf);
    lv_display_flush_ready(d);
}

/* ── 3. Input ──────────────────────────────────────────────────────────── */

static void touch_read(lv_indev_t *in, lv_indev_data_t *data)
{
    (void)in;
    papp_touch_point_t p;
    if (papp_disp_touch(s_surf, &p, 1) > 0) {
        data->point.x = p.x;
        data->point.y = p.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;            /* keeps the last point */
    }
}

/* Keys from the pad and the keyboard wait here; LVGL reads each as a press, then a release */
static uint32_t s_keys[16];
static int s_nkeys;
static uint32_t s_key;
static bool s_key_down;

static void key_push(uint32_t k)
{
    if (s_nkeys < (int)(sizeof(s_keys) / sizeof(s_keys[0]))) s_keys[s_nkeys++] = k;
}

static void keypad_read(lv_indev_t *in, lv_indev_data_t *data)
{
    (void)in;
    if (s_key_down) {                                     /* release the key pressed last time */
        s_key_down = false;
        data->state = LV_INDEV_STATE_RELEASED;
    } else if (s_nkeys) {
        s_key = s_keys[0];
        memmove(s_keys, s_keys + 1, --s_nkeys * sizeof(s_keys[0]));
        s_key_down = true;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->key = s_key;
    data->continue_reading = s_key_down || s_nkeys;
}

/* ── 4. The user interface ─────────────────────────────────────────────── */

static lv_obj_t *s_tabview;
static lv_group_t *s_group[TABS];         /* the widgets the pad moves between, one group per tab */
static lv_indev_t *s_keypad;

/* Widgets tab */
static lv_obj_t *s_arc, *s_level, *s_led, *s_count_label, *s_led_switch;
static int s_clicks;
static int s_color = 0;
static bool s_dark = true;

static const lv_palette_t palette[] = { LV_PALETTE_BLUE, LV_PALETTE_GREEN, LV_PALETTE_ORANGE, LV_PALETTE_PINK };

static void apply_theme(void)
{
    lv_theme_t *th = lv_theme_default_init(s_disp, lv_palette_main(palette[s_color]), lv_palette_main(LV_PALETTE_RED),
                                           s_dark, LV_FONT_DEFAULT);
    lv_display_set_theme(s_disp, th);
    lv_theme_apply(lv_screen_active());                   /* restyle everything already made */
    lv_led_set_color(s_led, lv_palette_main(palette[s_color]));
}

static void slider_changed(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    lv_arc_set_value(s_arc, v);
    lv_label_set_text_fmt(s_level, "%d%%", v);
    lv_led_set_brightness(s_led, (uint8_t)(80 + v * 175 / 100));   /* the LED follows the slider */
}

static void switch_changed(lv_event_t *e)
{
    if (lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)) lv_led_on(s_led);
    else lv_led_off(s_led);
}

static void button_clicked(lv_event_t *e)
{
    (void)e;
    s_clicks++;
    lv_label_set_text_fmt(s_count_label, "Clicked %d time%s", s_clicks, s_clicks == 1 ? "" : "s");
}

static void color_changed(lv_event_t *e)
{
    s_color = lv_dropdown_get_selected(lv_event_get_target(e));
    apply_theme();
}

static void dark_changed(lv_event_t *e)
{
    s_dark = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    apply_theme();
}

static lv_obj_t *small_text(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_opa(l, LV_OPA_70, 0);
    return l;
}

static void make_widgets_tab(lv_obj_t *tab)
{
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    /* left: a slider drives the arc gauge and its label */
    lv_obj_t *slider = lv_slider_create(tab);
    lv_obj_set_width(slider, 300);
    lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 20, 30);
    lv_slider_set_value(slider, 40, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, slider_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align_to(small_text(tab, "Slider: drag it, or Left / Right"), slider, LV_ALIGN_OUT_TOP_LEFT, 0, -8);

    s_arc = lv_arc_create(tab);
    lv_obj_set_size(s_arc, 200, 200);
    lv_arc_set_bg_angles(s_arc, 135, 45);
    lv_arc_set_value(s_arc, 40);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);         /* a gauge, not a control */
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_arc, LV_ALIGN_TOP_LEFT, 70, 80);
    s_level = lv_label_create(tab);
    lv_obj_set_style_text_font(s_level, &lv_font_montserrat_40, 0);
    lv_label_set_text(s_level, "40%");
    lv_obj_align_to(s_level, s_arc, LV_ALIGN_CENTER, 0, 0);

    /* right: a switch and an LED, a button and its counter, the theme */
    lv_obj_t *col = lv_obj_create(tab);
    lv_obj_set_size(col, 380, 330);
    lv_obj_align(col, LV_ALIGN_TOP_RIGHT, -10, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row = lv_obj_create(col);                     /* switch + LED on one line */
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 24, 0);
    lv_label_set_text(lv_label_create(row), "LED");
    s_led_switch = lv_switch_create(row);
    lv_obj_add_state(s_led_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_led_switch, switch_changed, LV_EVENT_VALUE_CHANGED, NULL);
    s_led = lv_led_create(row);
    lv_obj_set_size(s_led, 32, 32);
    lv_led_set_brightness(s_led, 80 + 40 * 175 / 100);
    lv_led_on(s_led);

    lv_obj_t *btn = lv_button_create(col);
    lv_label_set_text(lv_label_create(btn), LV_SYMBOL_OK "  Click me");
    lv_obj_add_event_cb(btn, button_clicked, LV_EVENT_CLICKED, NULL);
    s_count_label = lv_label_create(col);
    lv_label_set_text(s_count_label, "Not clicked yet");

    lv_obj_t *dd = lv_dropdown_create(col);
    lv_dropdown_set_options(dd, "Blue\nGreen\nOrange\nPink");
    lv_obj_set_width(dd, 200);
    lv_obj_add_event_cb(dd, color_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *cb = lv_checkbox_create(col);
    lv_checkbox_set_text(cb, "Dark theme");
    lv_obj_add_state(cb, LV_STATE_CHECKED);
    lv_obj_add_event_cb(cb, dark_changed, LV_EVENT_VALUE_CHANGED, NULL);
}

/* Chart tab: a timer adds a point to each line 20 times a second */
static lv_obj_t *s_chart;
static lv_chart_series_t *s_wave, *s_noise;
static lv_obj_t *s_speed;
static float s_phase;
static uint32_t s_rng = 12345;

static void chart_tick(lv_timer_t *t)
{
    (void)t;
    s_phase += lv_slider_get_value(s_speed) / 200.0f;
    lv_chart_set_next_value(s_chart, s_wave, (int32_t)(80 * sinf(s_phase)));
    static int32_t walk;                                   /* a made-up "sensor": each step goes a bit up or down */
    s_rng = s_rng * 1103515245u + 12345u;
    walk += (int32_t)((s_rng >> 16) % 13) - 6;
    walk = walk > 90 ? 90 : walk < -90 ? -90 : walk;
    lv_chart_set_next_value(s_chart, s_noise, walk);
}

static void make_chart_tab(lv_obj_t *tab)
{
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    s_chart = lv_chart_create(tab);
    lv_obj_set_size(s_chart, 720, 270);
    lv_obj_align(s_chart, LV_ALIGN_TOP_MID, 0, 0);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, 120);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, -100, 100);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);     /* lines without dots */
    lv_obj_remove_flag(s_chart, LV_OBJ_FLAG_CLICKABLE);
    s_wave = lv_chart_add_series(s_chart, lv_palette_main(LV_PALETTE_CYAN), LV_CHART_AXIS_PRIMARY_Y);
    s_noise = lv_chart_add_series(s_chart, lv_palette_main(LV_PALETTE_AMBER), LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *l = small_text(tab, "Cyan: a sine wave.  Amber: a random walk, like a sensor.  The slider sets the wave's speed.");
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 20, 285);
    s_speed = lv_slider_create(tab);
    lv_obj_set_width(s_speed, 400);
    lv_slider_set_range(s_speed, 5, 60);
    lv_slider_set_value(s_speed, 20, LV_ANIM_OFF);
    lv_obj_align(s_speed, LV_ALIGN_TOP_LEFT, 30, 325);
    lv_timer_create(chart_tick, 50, NULL);
}

/* Text tab: a text area and the on-screen keyboard (a USB keyboard types too) */
static lv_obj_t *s_ta, *s_echo;

static void text_ready(lv_event_t *e)
{
    (void)e;
    lv_label_set_text_fmt(s_echo, "You typed: %s", lv_textarea_get_text(s_ta));
}

static void make_text_tab(lv_obj_t *tab)
{
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    s_ta = lv_textarea_create(tab);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_placeholder_text(s_ta, "Type here, then OK or Enter");
    lv_obj_set_width(s_ta, 480);
    lv_obj_align(s_ta, LV_ALIGN_TOP_LEFT, 10, 0);
    lv_obj_add_event_cb(s_ta, text_ready, LV_EVENT_READY, NULL);
    s_echo = lv_label_create(tab);
    lv_label_set_text(s_echo, "");
    lv_obj_align(s_echo, LV_ALIGN_TOP_LEFT, 510, 12);
    lv_obj_set_width(s_echo, 240);
    lv_label_set_long_mode(s_echo, LV_LABEL_LONG_DOT);

    lv_obj_t *kb = lv_keyboard_create(tab);
    lv_obj_set_size(kb, LV_PCT(100), 280);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_ta);
}

static void tab_changed(lv_event_t *e)
{
    (void)e;
    lv_indev_set_group(s_keypad, s_group[lv_tabview_get_tab_active(s_tabview)]);
}

static void close_clicked(lv_event_t *e)
{
    (void)e;
    s_quit = true;
}

static void make_ui(void)
{
    lv_group_set_default(NULL);                           /* the tab buttons aren't in a group */
    s_tabview = lv_tabview_create(lv_screen_active());
    lv_tabview_set_tab_bar_size(s_tabview, 56);
    lv_obj_set_style_pad_right(lv_tabview_get_tab_bar(s_tabview), 64, 0);   /* room for the close button */
    lv_obj_t *tab[TABS] = { lv_tabview_add_tab(s_tabview, "Widgets"), lv_tabview_add_tab(s_tabview, "Chart"),
                            lv_tabview_add_tab(s_tabview, "Text") };
    void (*make[TABS])(lv_obj_t *) = { make_widgets_tab, make_chart_tab, make_text_tab };
    for (int i = 0; i < TABS; i++) {
        s_group[i] = lv_group_create();
        lv_group_set_default(s_group[i]);                 /* widgets made now join this tab's group */
        make[i](tab[i]);
    }
    lv_group_set_default(NULL);
    lv_obj_add_event_cb(s_tabview, tab_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *x = lv_button_create(lv_layer_top());       /* above everything */
    lv_obj_set_size(x, 48, 48);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_center(xl);
    lv_obj_add_event_cb(x, close_clicked, LV_EVENT_CLICKED, NULL);
}

/* ── 5. Main loop ──────────────────────────────────────────────────────── */

static void read_pad_and_keyboard(void)
{
    static uint32_t prev = 0xFFFFFFFFu;                   /* nothing counts until released once */
    static int64_t repeat_at;
    papp_gamepad_t gp = { 0 };
    papp_input_gamepad(0, &gp);
    uint32_t press = gp.buttons & ~prev;
    prev = gp.buttons;
    int64_t now = papp_time_us();

    if (press & PAPP_BTN_X) s_quit = true;
    if (press & PAPP_BTN_UP) key_push(LV_KEY_PREV);
    if (press & PAPP_BTN_DOWN) key_push(LV_KEY_NEXT);
    if (press & PAPP_BTN_A) key_push(LV_KEY_ENTER);
    if (press & PAPP_BTN_B) key_push(LV_KEY_ESC);
    if (press & (PAPP_BTN_LEFT | PAPP_BTN_RIGHT)) repeat_at = now + 400000;
    else if ((gp.buttons & (PAPP_BTN_LEFT | PAPP_BTN_RIGHT)) && now >= repeat_at) {
        press |= gp.buttons & (PAPP_BTN_LEFT | PAPP_BTN_RIGHT);      /* held: repeat */
        repeat_at = now + 60000;
    }
    if (press & PAPP_BTN_LEFT) key_push(LV_KEY_LEFT);
    if (press & PAPP_BTN_RIGHT) key_push(LV_KEY_RIGHT);
    int tab = lv_tabview_get_tab_active(s_tabview);
    if ((press & PAPP_BTN_L1) && tab > 0) lv_tabview_set_active(s_tabview, tab - 1, LV_ANIM_ON);
    if ((press & PAPP_BTN_R1) && tab < TABS - 1) lv_tabview_set_active(s_tabview, tab + 1, LV_ANIM_ON);
    if (press & (PAPP_BTN_L1 | PAPP_BTN_R1)) lv_indev_set_group(s_keypad, s_group[lv_tabview_get_tab_active(s_tabview)]);

    papp_input_event_t ev;                                /* USB keyboard */
    while (papp_input_poll_event(&ev)) {
        if (ev.type != PAPP_EV_KEY_DOWN) continue;
        switch (ev.code) {
        case 0x28: case 0x58: key_push(LV_KEY_ENTER); break;        /* Enter */
        case 0x29: key_push(LV_KEY_ESC); break;
        case 0x2A: key_push(LV_KEY_BACKSPACE); break;
        case 0x4C: key_push(LV_KEY_DEL); break;
        case 0x2B: key_push((ev.mods & (PAPP_MOD_LSHIFT | PAPP_MOD_RSHIFT)) ? LV_KEY_PREV : LV_KEY_NEXT); break;
        case 0x4F: key_push(LV_KEY_RIGHT); break;
        case 0x50: key_push(LV_KEY_LEFT); break;
        case 0x51: key_push(LV_KEY_NEXT); break;          /* Down */
        case 0x52: key_push(LV_KEY_PREV); break;          /* Up */
        default:
            if (ev.text >= 32 && ev.text < 127) key_push(ev.text);   /* a character for the text area */
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };   /* one buffer: it keeps its picture */
    s_surf = papp_disp_surface_create(&cfg);
    if (!s_surf) return 1;
    int stride;
    void *fb = papp_disp_lock(s_surf, &stride);

    lv_init();
    lv_tick_set_cb(tick_ms);

    s_disp = lv_display_create(W, H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, fb, NULL, (uint32_t)stride * H * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush);

    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read);
    s_keypad = lv_indev_create();
    lv_indev_set_type(s_keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_keypad, keypad_read);

    make_ui();
    apply_theme();
    lv_indev_set_group(s_keypad, s_group[0]);

    while (!papp_exit_requested() && !s_quit) {
        read_pad_and_keyboard();
        uint32_t wait = lv_timer_handler();               /* ms until LVGL has work again */
        papp_delay_ms(wait > 10 ? 10 : wait ? wait : 1);
    }
    lv_deinit();
    papp_disp_surface_destroy(s_surf);
    return 0;
}
