/*
 * pwr_test: the PWR interface (battery and charging). Reads the battery, turns charging off and reads again
 * (what stock P4 Launcher left the Tab5 in), turns it back on and reads again, over a few seconds each; then
 * leaves it off so the launcher's end-of-run restore can be seen in its log ("charging back on after the app").
 *   no args : that
 *   "watch" : just reads it every 2 s until stopped
 */
#include "papp/papp.h"
#include <string.h>

PAPP_MANIFEST(.id = "test.pwr", .name = "PWR test", .version = PAPP_APP_VERSION, .main_stack = 8192,
              .flags = PAPP_MF_HIDDEN);

static void show(const papp_if_pwr_t *p, const char *when)
{
    papp_power_t s;
    if (p->read(&s) != PAPP_E_OK) { papp_log("%s: no battery reading\n", when); return; }
    papp_log("%s: pack %ld mV (cell %ld), %+ld mA, %d%%, charger says %s, charging %s%s\n", when, (long)s.pack_mv,
             (long)s.cell_mv, (long)s.ma, s.percent, s.charging ? "CHARGING" : "not charging",
             s.charge_enabled ? "allowed" : "off", s.can_switch ? "" : " (cannot switch)");
}

/* A few readings a second apart: the charger takes a moment to start and stop. */
static void settle(const papp_if_pwr_t *p, const char *when)
{
    for (int i = 0; i < 4 && !papp_exit_requested(); i++) { papp_delay_ms(1000); show(p, when); }
}

int main(int argc, char **argv)
{
    const papp_if_pwr_t *p = papp_pwr();
    if (!p) { papp_log("no PWR interface in this launcher\n"); return 2; }
    if (argc > 1 && !strcmp(argv[1], "watch")) {
        while (!papp_exit_requested()) { show(p, "now"); papp_delay_ms(2000); }
        return 0;
    }
    show(p, "as found");
    papp_log("set_charge(off): %d\n", p->set_charge(false));
    settle(p, "charging off");
    papp_log("set_charge(on): %d\n", p->set_charge(true));
    settle(p, "charging on");
    papp_log("set_charge(off) and leaving: the launcher should turn it back on\n");
    p->set_charge(false);
    return 0;
}
