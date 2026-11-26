#define PAPP_APP_SIDE 1
#include "papp/papp.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PAPP_MANIFEST(.id="tool.ble_remote", .name="BLE Remote",
              .version=PAPP_APP_VERSION, .main_stack=32768);

#define WIDTH 1024
#define HEIGHT 600
#define KEYBOARD_REPORT 1
#define MOUSE_REPORT 2
#define GAMEPAD_REPORT 3

static const papp_if_ble_t *ble;
static papp_surface_t *surface;
static uint16_t *canvas;
static lv_display_t *display;
static lv_indev_t *pointer;
static lv_obj_t *status_label, *content_panel, *advertise_label;
static papp_ble_conn_t connection = PAPP_BLE_CONN_NONE;
static bool advertising, connected, keyboard_subscribed, mouse_subscribed, gamepad_subscribed;
static bool quitting;
static uint8_t modifiers, keys[6], mouse_buttons, hat = 8;
static uint16_t gamepad_buttons;
static uint8_t protocol_mode = 1;
static int mouse_last_x, mouse_last_y;
static uint16_t h_info, h_map, h_protocol, h_control, h_boot_kbd, h_boot_mouse;
static uint16_t h_report_kbd, h_report_mouse, h_report_gamepad;
static uint8_t value_info[4] = {0x11, 0x01, 0, 0x02};
static const uint8_t report_map[] = {
    0x05,0x01,0x09,0x06,0xA1,0x01,0x85,0x01,0x05,0x07,0x19,0xE0,0x29,0xE7,0x15,0x00,
    0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x06,
    0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,0x19,0x00,0x29,0x65,0x81,0x00,0xC0,
    0x05,0x01,0x09,0x02,0xA1,0x01,0x85,0x02,0x09,0x01,0xA1,0x00,0x05,0x09,0x19,0x01,
    0x29,0x03,0x15,0x00,0x25,0x01,0x95,0x03,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x05,
    0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x02,
    0x81,0x06,0x09,0x38,0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x01,0x81,0x06,0xC0,0xC0,
    0x05,0x01,0x09,0x05,0xA1,0x01,0x85,0x03,0x05,0x09,0x19,0x01,0x29,0x10,0x15,0x00,
    0x25,0x01,0x75,0x01,0x95,0x10,0x81,0x02,0x05,0x01,0x09,0x39,0x15,0x00,0x25,0x08,
    0x35,0x00,0x46,0x3B,0x01,0x65,0x14,0x75,0x04,0x95,0x01,0x81,0x42,0x05,0x01,0x09,
    0x30,0x09,0x31,0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x02,0x81,0x02,0xC0
};

static const papp_ble_dsc_t keyboard_ref[] = {{{2,{0x08,0x29}},{KEYBOARD_REPORT,1},2,PAPP_BLE_READ}};
static const papp_ble_dsc_t mouse_ref[] = {{{2,{0x08,0x29}},{MOUSE_REPORT,1},2,PAPP_BLE_READ}};
static const papp_ble_dsc_t gamepad_ref[] = {{{2,{0x08,0x29}},{GAMEPAD_REPORT,1},2,PAPP_BLE_READ}};
static const papp_ble_chr2_t hid_chars[] = {
    {PAPP_BLE_UUID16(0x2A4A),PAPP_BLE_READ,sizeof(value_info),&h_info,NULL,0},
    {PAPP_BLE_UUID16(0x2A4B),PAPP_BLE_READ,sizeof(report_map),&h_map,NULL,0},
    {PAPP_BLE_UUID16(0x2A4E),PAPP_BLE_READ|PAPP_BLE_WRITE,1,&h_protocol,NULL,0},
    {PAPP_BLE_UUID16(0x2A4C),PAPP_BLE_WRITE_NR,1,&h_control,NULL,0},
    {PAPP_BLE_UUID16(0x2A22),PAPP_BLE_NOTIFY,8,&h_boot_kbd,NULL,0},
    {PAPP_BLE_UUID16(0x2A33),PAPP_BLE_NOTIFY,4,&h_boot_mouse,NULL,0},
    {PAPP_BLE_UUID16(0x2A4D),PAPP_BLE_NOTIFY,9,&h_report_kbd,keyboard_ref,1},
    {PAPP_BLE_UUID16(0x2A4D),PAPP_BLE_NOTIFY,5,&h_report_mouse,mouse_ref,1},
    {PAPP_BLE_UUID16(0x2A4D),PAPP_BLE_NOTIFY,6,&h_report_gamepad,gamepad_ref,1}
};
static const papp_ble_svc2_t hid_service = {PAPP_BLE_UUID16(0x1812), hid_chars, 9};

static uint32_t tick_ms(void) { return (uint32_t)(papp_time_us() / 1000); }
static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *pixels) {
    (void)area; papp_disp_present_ext(surface, pixels); lv_display_flush_ready(d);
}
static void read_touch(lv_indev_t *in, lv_indev_data_t *data) {
    (void)in; papp_touch_point_t point;
    if (papp_disp_touch(surface, &point, 1) > 0) {
        data->point.x = point.x; data->point.y = point.y; data->state = LV_INDEV_STATE_PRESSED;
    } else data->state = LV_INDEV_STATE_RELEASED;
}
static void set_status(const char *text) {
    if (status_label) lv_label_set_text(status_label, text);
}
static int notify(uint16_t handle, const void *data, size_t len, bool subscribed) {
    return ble && connected && subscribed ? ble->notify(connection, handle, data, len) : -1;
}
static void keyboard_send(void) {
    uint8_t report[8] = {modifiers,0,keys[0],keys[1],keys[2],keys[3],keys[4],keys[5]};
    uint8_t with_id[9] = {KEYBOARD_REPORT}; memcpy(with_id + 1, report, sizeof(report));
    if (protocol_mode == 0) notify(h_boot_kbd, report, sizeof(report), keyboard_subscribed);
    else notify(h_report_kbd, with_id, sizeof(with_id), keyboard_subscribed);
}
static void mouse_send(int dx, int dy, int wheel) {
    uint8_t report[4] = {mouse_buttons,(uint8_t)dx,(uint8_t)dy,(uint8_t)wheel};
    uint8_t with_id[5] = {MOUSE_REPORT}; memcpy(with_id + 1, report, sizeof(report));
    if (protocol_mode == 0) notify(h_boot_mouse, report, sizeof(report), mouse_subscribed);
    else notify(h_report_mouse, with_id, sizeof(with_id), mouse_subscribed);
}
static void gamepad_send(void) {
    uint8_t report[6] = {GAMEPAD_REPORT,(uint8_t)gamepad_buttons,(uint8_t)(gamepad_buttons >> 8),hat,0,0};
    notify(h_report_gamepad, report, sizeof(report), gamepad_subscribed);
}
static void send_key(lv_event_t *e) {
    uint8_t usage = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) { memset(keys,0,sizeof(keys)); keys[0]=usage; keyboard_send(); }
    else if (lv_event_get_code(e) == LV_EVENT_RELEASED) { memset(keys,0,sizeof(keys)); keyboard_send(); }
}
static void send_special(lv_event_t *e) {
    uint8_t value = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        modifiers = value ? value : modifiers;
        if (value == 0) { keys[0] = 0x2C; }
        keyboard_send();
    } else if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        if (value) modifiers = 0; else memset(keys,0,sizeof(keys));
        keyboard_send();
    }
}
static void set_gamepad(lv_event_t *e) {
    uintptr_t packed = (uintptr_t)lv_event_get_user_data(e);
    const uint16_t bit = (uint16_t)(packed & 0xFFFFu);
    const uint8_t direction = (uint8_t)(packed >> 16);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) { gamepad_buttons |= bit; if (direction) hat=direction; }
    else if (lv_event_get_code(e) == LV_EVENT_RELEASED) { gamepad_buttons &= (uint16_t)~bit; if (direction && hat==direction) hat=8; }
    gamepad_send();
}
static void set_mouse_button(lv_event_t *e) {
    const uint8_t bit=(uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e)==LV_EVENT_PRESSED) mouse_buttons |= bit;
    else if (lv_event_get_code(e)==LV_EVENT_RELEASED) mouse_buttons &= (uint8_t)~bit;
    mouse_send(0,0,0);
}
static void mouse_pad_event(lv_event_t *e) {
    lv_indev_t *in=lv_indev_active();if(!in)return;
    lv_point_t p;lv_indev_get_point(in,&p);
    if(lv_event_get_code(e)==LV_EVENT_PRESSED){mouse_last_x=p.x;mouse_last_y=p.y;}
    else if(lv_event_get_code(e)==LV_EVENT_PRESSING){
        int dx=p.x-mouse_last_x,dy=p.y-mouse_last_y;mouse_last_x=p.x;mouse_last_y=p.y;
        if(dx||dy)mouse_send(dx,dy,0);
    }
}
static void mouse_scroll(lv_event_t *e) {
    if(lv_event_get_code(e)==LV_EVENT_CLICKED)mouse_send(0,0,(int)(intptr_t)lv_event_get_user_data(e));
}
static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int w, int h, int size) {
    lv_obj_t *o=lv_label_create(parent); lv_label_set_text(o,text); lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
    lv_obj_set_style_text_font(o,size>=24?&lv_font_montserrat_28:&lv_font_montserrat_14,0);
    lv_obj_set_style_text_color(o,lv_color_hex(0xEAF3FF),0); lv_label_set_long_mode(o,LV_LABEL_LONG_WRAP); return o;
}
static lv_obj_t *button(lv_obj_t *parent,const char *text,int x,int y,int w,int h,lv_event_cb_t cb,uintptr_t data) {
    lv_obj_t *b=lv_button_create(parent);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
    lv_obj_set_style_bg_color(b,lv_color_hex(0x174D6B),0);lv_obj_set_style_bg_color(b,lv_color_hex(0x26769B),LV_STATE_PRESSED);
    lv_obj_add_event_cb(b,cb,LV_EVENT_PRESSED,(void*)data);lv_obj_add_event_cb(b,cb,LV_EVENT_RELEASED,(void*)data);
    lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,text);lv_obj_center(l);return b;
}
static void show_keyboard(lv_obj_t *panel) {
    static const struct {const char *s;uint8_t key;} row1[]={{"Q",20},{"W",26},{"E",8},{"R",21},{"T",23},{"Y",28},{"U",24},{"I",12},{"O",18},{"P",19}};
    static const struct {const char *s;uint8_t key;} row2[]={{"A",4},{"S",22},{"D",7},{"F",9},{"G",10},{"H",11},{"J",13},{"K",14},{"L",15}};
    static const struct {const char *s;uint8_t key;} row3[]={{"Z",29},{"X",27},{"C",6},{"V",25},{"B",5},{"N",17},{"M",16}};
    lv_obj_set_size(panel,968,400);lv_obj_set_style_bg_color(panel,lv_color_hex(0x0E2032),0);
    lv_obj_set_style_border_color(panel,lv_color_hex(0x24445B),0);
    label(panel,"Touch a key to send it over BLE HID. Modifier keys stay active until released.",18,10,900,28,16);
    for(unsigned i=0;i<10;i++)button(panel,row1[i].s,18+(int)i*92,58,82,55,send_key,row1[i].key);
    for(unsigned i=0;i<9;i++)button(panel,row2[i].s,62+(int)i*92,123,82,55,send_key,row2[i].key);
    for(unsigned i=0;i<7;i++)button(panel,row3[i].s,154+(int)i*92,188,82,55,send_key,row3[i].key);
    button(panel,"Shift",35,253,112,54,send_special,2);button(panel,"Ctrl",157,253,100,54,send_special,1);
    button(panel,"Alt",267,253,92,54,send_special,4);button(panel,"Space",369,253,420,54,send_special,0);
    button(panel,"Enter",799,253,120,54,send_key,40);
}
static void show_mouse(lv_obj_t *root, lv_obj_t *panel) {
    (void)root;
    lv_obj_set_size(panel,968,400);lv_obj_set_style_bg_color(panel,lv_color_hex(0x0E2032),0);
    lv_obj_set_style_border_color(panel,lv_color_hex(0x24445B),0);
    lv_obj_t *pad=lv_obj_create(panel);lv_obj_set_pos(pad,18,58);lv_obj_set_size(pad,700,310);
    lv_obj_set_style_bg_color(pad,lv_color_hex(0x132A40),0);lv_obj_set_style_border_color(pad,lv_color_hex(0x48BDEB),0);
    lv_obj_add_event_cb(pad,mouse_pad_event,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(pad,mouse_pad_event,LV_EVENT_PRESSING,NULL);
    label(panel,"Drag here to move the pointer",30,14,700,32,16);
    button(panel,"Left click",744,76,196,64,set_mouse_button,1);
    button(panel,"Right click",744,153,196,64,set_mouse_button,2);
    lv_obj_t *up=lv_button_create(panel);lv_obj_set_pos(up,744,230);lv_obj_set_size(up,92,58);
    lv_obj_add_event_cb(up,mouse_scroll,LV_EVENT_CLICKED,(void*)(intptr_t)1);
    lv_obj_t *up_label=lv_label_create(up);lv_label_set_text(up_label,"Scroll +");lv_obj_center(up_label);
    lv_obj_t *down=lv_button_create(panel);lv_obj_set_pos(down,844,230);lv_obj_set_size(down,96,58);
    lv_obj_add_event_cb(down,mouse_scroll,LV_EVENT_CLICKED,(void*)(intptr_t)-1);
    lv_obj_t *down_label=lv_label_create(down);lv_label_set_text(down_label,"Scroll −");lv_obj_center(down_label);
}
static void show_gamepad(lv_obj_t *panel) {
    lv_obj_set_size(panel,968,400);lv_obj_set_style_bg_color(panel,lv_color_hex(0x0E2032),0);
    lv_obj_set_style_border_color(panel,lv_color_hex(0x24445B),0);
    label(panel,"Hold a direction or button to send a gamepad report.",20,12,850,32,16);
    button(panel,"↑",151,66,82,66,set_gamepad,(uintptr_t)(1u<<16));
    button(panel,"←",62,136,82,66,set_gamepad,(uintptr_t)(6u<<16));
    button(panel,"→",240,136,82,66,set_gamepad,(uintptr_t)(2u<<16));
    button(panel,"↓",151,206,82,66,set_gamepad,(uintptr_t)(4u<<16));
    button(panel,"Y",560,66,80,72,set_gamepad,1u<<3);button(panel,"X",650,135,80,72,set_gamepad,1u<<2);
    button(panel,"B",560,204,80,72,set_gamepad,1u<<1);button(panel,"A",470,135,80,72,set_gamepad,1u<<0);
    button(panel,"L",750,66,94,64,set_gamepad,1u<<8);button(panel,"R",852,66,94,64,set_gamepad,1u<<9);
}
static void switch_tab(lv_event_t *e) {
    unsigned tab=(unsigned)(uintptr_t)lv_event_get_user_data(e);
    if(content_panel)lv_obj_delete(content_panel);
    content_panel=lv_obj_create(lv_screen_active());lv_obj_set_pos(content_panel,28,168);
    if(tab==0)show_keyboard(content_panel);
    else if(tab==1)show_mouse(lv_screen_active(),content_panel);
    else if(tab==2)show_gamepad(content_panel);
}
static void close_app(lv_event_t *e) {(void)e;quitting=true;}
static int start_advertising(void) {
    if(!ble)return -1;
    uint8_t adv[31],rsp[31];int n=0,rn=0;papp_ble_uuid_t uuid=PAPP_BLE_UUID16(0x1812);
    papp_ble_ad_flags(adv,&n);papp_ble_ad_uuid(adv,&n,&uuid);papp_ble_ad_name(rsp,&rn,"PAX BLE Remote");
    papp_ble_adv_cfg_t cfg={.adv=adv,.adv_len=(uint8_t)n,.rsp=rsp,.rsp_len=(uint8_t)rn,.connectable=1,.interval_ms=100};
    int rc=ble->adv_start(&cfg);advertising=rc==0;return rc;
}
static void toggle_advertising(lv_event_t *e) {
    (void)e;
    if(!ble)return;
    if(advertising){ble->adv_stop();advertising=false;set_status("Advertising paused");lv_label_set_text(advertise_label,"Advertise");}
    else {
        if(start_advertising()==0){set_status("Advertising · connect from Bluetooth settings");lv_label_set_text(advertise_label,"Stop advertising");}
        else set_status("Could not start BLE advertising");
    }
}
static void build_ui(void) {
    lv_obj_t *root=lv_screen_active();lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root,lv_color_hex(0x081827),0);lv_obj_set_style_text_color(root,lv_color_hex(0xEAF3FF),0);
    label(root,"BLE Remote",28,14,310,42,24);
    status_label=label(root,"Starting Bluetooth LE…",320,20,550,34,16);
    button(root,"Keyboard",28,78,190,58,switch_tab,0);button(root,"Mouse",230,78,190,58,switch_tab,1);
    button(root,"Gamepad",432,78,190,58,switch_tab,2);
    lv_obj_t *adv=lv_button_create(root);lv_obj_set_pos(adv,650,78);lv_obj_set_size(adv,170,58);
    lv_obj_add_event_cb(adv,toggle_advertising,LV_EVENT_CLICKED,NULL);advertise_label=lv_label_create(adv);
    lv_label_set_text(advertise_label,"Stop advertising");lv_obj_center(advertise_label);
    button(root,"Close",842,78,150,58,close_app,0);
    content_panel=lv_obj_create(root);lv_obj_set_pos(content_panel,28,168);show_keyboard(content_panel);
}
static void process_ble_events(void) {
    if(!ble)return;
    papp_ble_event_t e;
    while(ble->poll_event(&e)){
        if(e.type==PAPP_BLE_EV_CONNECTED){
            connection=e.conn;connected=true;advertising=false;set_status("Connected · BLE HID ready");
            if(advertise_label)lv_label_set_text(advertise_label,"Advertise");
        }
        else if(e.type==PAPP_BLE_EV_DISCONNECTED&&e.conn==connection){
            connection=PAPP_BLE_CONN_NONE;connected=false;keyboard_subscribed=mouse_subscribed=gamepad_subscribed=false;
            memset(keys,0,sizeof(keys));modifiers=mouse_buttons=0;gamepad_buttons=0;hat=8;
            if(start_advertising()==0){set_status("Disconnected · advertising");if(advertise_label)lv_label_set_text(advertise_label,"Stop advertising");}
            else set_status("Disconnected · tap Advertise to reconnect");
        } else if(e.type==PAPP_BLE_EV_SUBSCRIBE&&e.conn==connection){
            bool on=e.code!=0;
            if(e.handle==h_boot_kbd||e.handle==h_report_kbd)keyboard_subscribed=on;
            if(e.handle==h_boot_mouse||e.handle==h_report_mouse)mouse_subscribed=on;
            if(e.handle==h_report_gamepad)gamepad_subscribed=on;
        } else if(e.type==PAPP_BLE_EV_WRITE&&e.handle==h_protocol&&e.len){protocol_mode=e.data[0]?1:0;ble->set_value(h_protocol,&protocol_mode,1);}
    }
}
static int ble_start(void) {
    ble=papp_ble();if(!ble||!ble->available())return -1;
    if(ble->set_name&&ble->set_name("PAX BLE Remote")<0)return -2;
    if(ble->set_appearance)ble->set_appearance(0x03C1);
    if(ble->security){papp_ble_sec_cfg_t sec={.io_cap=PAPP_BLE_IO_NONE,.bond=1,.mitm=0,.sc=1,.initiate=1};if(ble->security(&sec)<0)return -3;}
    if(!ble->gatt_register2||ble->gatt_register2(&hid_service,1)<0)return -4;
    ble->set_value(h_info,value_info,sizeof(value_info));ble->set_value(h_map,report_map,sizeof(report_map));
    ble->set_value(h_protocol,&protocol_mode,1);
    return start_advertising();
}
int main(void) {
    papp_surface_cfg_t cfg={.width=WIDTH,.height=HEIGHT,.format=PAPP_PIXFMT_RGB565,.scale=PAPP_SCALE_FIT_ASPECT,.buffers=1};
    surface=papp_disp_surface_create(&cfg);if(!surface)return -1;
    canvas=papp_alloc(WIDTH*HEIGHT*sizeof(uint16_t),PAPP_MEM_LARGE|PAPP_MEM_DMA,64);
    if(!canvas){papp_disp_surface_destroy(surface);return -2;}
    lv_init();lv_tick_set_cb(tick_ms);display=lv_display_create(WIDTH,HEIGHT);lv_display_set_default(display);
    lv_display_set_flush_cb(display,flush);lv_display_set_buffers(display,canvas,NULL,WIDTH*HEIGHT*sizeof(uint16_t),LV_DISPLAY_RENDER_MODE_FULL);
    pointer=lv_indev_create();lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(pointer,read_touch);lv_indev_set_display(pointer,display);
    build_ui();int rc=ble_start();
    set_status(rc==0?"Advertising · connect from Bluetooth settings":"Bluetooth LE unavailable; check C6 firmware.");
    if(advertise_label)lv_label_set_text(advertise_label,rc==0?"Stop advertising":"Advertise");
    while(!quitting&&!papp_exit_requested()) {process_ble_events();lv_timer_handler();papp_delay_ms(10);}
    if(ble){if(connected)ble->disconnect(connection);if(advertising)ble->adv_stop();}
    lv_indev_delete(pointer);lv_display_delete(display);lv_deinit();papp_free(canvas);papp_disp_surface_destroy(surface);return 0;
}
