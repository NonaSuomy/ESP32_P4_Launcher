/* PAX platform glue for the line-based Frodo C64 core. */

#include "C64.h"
#include "CPUC64.h"
#include "CPU1541.h"
#include "VIC.h"
#include "SID.h"
#include "CIA.h"
#include "Prefs.h"
#include "Display.h"
#include "pax_c64_compat.h"

void C64::c64_ctor1(void) {}
void C64::c64_ctor2(void) {}
void C64::c64_dtor(void) {}

void C64::open_close_joysticks(bool oldjoy1, bool oldjoy2,
                               bool newjoy1, bool newjoy2)
{
    (void)oldjoy1;
    (void)oldjoy2;
    (void)newjoy1;
    (void)newjoy2;
}

uint8 C64::poll_joystick(int port)
{
    (void)port;
    return 0xff;
}

void C64::Run(void)
{
    TheCPU->Reset();
    TheSID->Reset();
    TheCIA1->Reset();
    TheCIA2->Reset();
    TheCPU1541->Reset();

    ThePrefs.LimitSpeed = true;
    orig_kernal_1d84 = Kernal[0x1d84];
    orig_kernal_1d85 = Kernal[0x1d85];
    PatchKernal(ThePrefs.FastReset, ThePrefs.Emul1541Proc);

    quit_thyself = false;
    c64_started();
    thread_func();
}

void C64::Quit(void)
{
    quit_thyself = true;
}

void C64::Pause(void)
{
    have_a_break = true;
}

void C64::Resume(void)
{
    have_a_break = false;
}

void C64::VBlank(bool draw_frame)
{
    if (draw_frame) {
        TheDisplay->PollKeyboard(TheCIA1->KeyMatrix, TheCIA1->RevMatrix,
                                 &joykey, &joykey2);
        /* PollKeyboard updates the host-side joystick masks.  The normal
         * ESP32 frontend copies these into CIA1; do the same for PAX so
         * joystick games can actually read ports 1 and 2. */
        TheCIA1->Joystick1 = joykey;
        TheCIA1->Joystick2 = joykey2;
        TheDisplay->Update();
    }

    TheCIA1->CountTOD();
    TheCIA2->CountTOD();
}

void C64::thread_func(void)
{
    while (!quit_thyself) {
        int cycles = TheVIC->EmulateLine();
        TheSID->EmulateLine();
        TheCIA1->EmulateLine(ThePrefs.CIACycles);
        TheCIA2->EmulateLine(ThePrefs.CIACycles);

        if (ThePrefs.Emul1541Proc) {
            int cycles_1541 = ThePrefs.FloppyCycles;
            TheCPU1541->CountVIATimers(cycles_1541);
            if (!TheCPU1541->Idle) {
                while (cycles >= 0 || cycles_1541 >= 0) {
                    if (cycles > cycles_1541)
                        cycles -= TheCPU->EmulateLine(1);
                    else
                        cycles_1541 -= TheCPU1541->EmulateLine(1);
                }
            } else {
                TheCPU->EmulateLine(cycles);
            }
        } else {
            TheCPU->EmulateLine(cycles);
        }
    }
}
