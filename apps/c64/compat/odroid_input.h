#pragma once

#include <string.h>
#include "psram_app.h"

/* The legacy emulator cores use the ODROID input names.  A PAX has the
 * same logical button layout, so keep this compatibility layer header-only
 * and source the state from the launcher's service table. */
typedef pax_gamepad_state_t odroid_gamepad_state;

enum {
    ODROID_INPUT_UP = PAX_INPUT_UP,
    ODROID_INPUT_RIGHT = PAX_INPUT_RIGHT,
    ODROID_INPUT_DOWN = PAX_INPUT_DOWN,
    ODROID_INPUT_LEFT = PAX_INPUT_LEFT,
    ODROID_INPUT_SELECT = PAX_INPUT_SELECT,
    ODROID_INPUT_START = PAX_INPUT_START,
    ODROID_INPUT_A = PAX_INPUT_A,
    ODROID_INPUT_B = PAX_INPUT_B,
    ODROID_INPUT_X = PAX_INPUT_X,
    ODROID_INPUT_Y = PAX_INPUT_Y,
    ODROID_INPUT_L = PAX_INPUT_L,
    ODROID_INPUT_R = PAX_INPUT_R,
    ODROID_INPUT_MENU = PAX_INPUT_MENU,
    ODROID_INPUT_VOLUME = PAX_INPUT_VOLUME,
    ODROID_INPUT_MAX = PAX_INPUT_MAX
};

extern const app_services_t *_pax_svc;
extern volatile int odroid_paddle_adc_raw;
extern bool odroid_input_xy_menu_disable;
extern bool odroid_input_touch_buttons_disable;
#ifdef __cplusplus
extern "C" {
#endif
void odroid_paddle_adc_init(void);
#ifdef __cplusplus
}
#endif

static inline void odroid_input_gamepad_read(odroid_gamepad_state *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    if (_pax_svc && _pax_svc->input_gamepad_read)
        _pax_svc->input_gamepad_read(state);
}
