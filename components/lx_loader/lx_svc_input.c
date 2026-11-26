/*
 * INPT interface: gamepads, keyboard state and the event queue (lx_input).
 * The queue is flushed when an app starts, so it only sees its own events.
 */
#include "lx_services.h"
#include "lx_input.h"

static bool inpt_gamepad(int player, papp_gamepad_t *out)
{
    SVC_ENTER();
    bool c = lx_input_gamepad(player, out);
    SVC_LEAVE();
    return c;
}

static bool inpt_key_down(uint16_t usage)
{
    return lx_input_key_down(usage);
}

static uint16_t inpt_modifiers(void)
{
    return lx_input_modifiers();
}

static bool inpt_poll_event(papp_input_event_t *out)
{
    SVC_ENTER();
    bool r = lx_input_poll_event(out);
    SVC_LEAVE();
    return r;
}

static void inpt_flush(void)
{
    SVC_ENTER();
    lx_input_flush();
    SVC_LEAVE();
}

static bool inpt_keyboard(void)
{
    return lx_input_keyboard_attached();
}

static int inpt_analog(int ch)
{
    return lx_input_analog(ch);
}

const papp_if_inpt_t lx_if_inpt = {
    .hdr = { PAPP_IF_INPT, PAPP_IF_INPT_VER, sizeof(papp_if_inpt_t) },
    .gamepad    = inpt_gamepad,
    .key_down   = inpt_key_down,
    .modifiers  = inpt_modifiers,
    .poll_event = inpt_poll_event,
    .flush      = inpt_flush,
    .analog     = inpt_analog,
    .keyboard   = inpt_keyboard,
};
