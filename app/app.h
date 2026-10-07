// The remote's app (app.cpp) and the board it runs on. The app is shared by every board: the screens, the box link,
// the loader, the safety glue. A board (m5/src/board/main.cpp, radr/src/main.cpp) brings the knobs and buttons, the
// display, the serial port, and calls app_setup() / app_loop() from Arduino's setup() / loop().
//
// Build flags (the board's platformio.ini):
//   BOARD_RADR=1   the RADR hardware's display (LovyanGFX) instead of the M5's (M5GFX through M5Unified)
#pragma once

#include <Arduino.h>

#if defined(BOARD_RADR) && BOARD_RADR
#include <LovyanGFX.hpp>
// the screen is drawn into a sprite in PSRAM and pushed in one piece (as M5Canvas does on the M5)
class AppCanvas : public lgfx::LGFX_Sprite {
public:
    explicit AppCanvas(lgfx::LovyanGFX *parent) : lgfx::LGFX_Sprite(parent) { setPsram(true); }
};
#else
#include <M5GFX.h>
typedef M5Canvas AppCanvas;
#endif

extern "C" {
#include "boxlink.h"
#include "ctrl.h"
}

// ---- what a board provides ----------------------------------------------------------------------------------------
const char *board_name();                       // "m5" / "radr": HELLO's board word (the PC tells the images apart)
void board_link_ids(uint32_t *first, uint32_t *last);   // its request-id range (boxlink.h BOXLINK_IDS_*)
void board_begin();                             // first thing: the display on (the app's splash follows)
lgfx::LovyanGFX &board_display();               // the 320 x 240 landscape display
void board_serial_begin(size_t rx_queue);       // the loader's serial port, with a receive queue this big
void board_inputs_begin();                      // knobs and buttons (a button held at boot is not a press)
void board_inputs(ctrl_t *c, double now);       // control task, under the app's lock: the knobs and buttons into c
void board_controller_ready(ctrl_t *c);         // a new controller (boot, every load): the board's options
bool board_may_drive();                         // false: no Wi-Fi, no box (the RADR until its check has passed)
void board_poll();                              // loop, first: the board's own (power button, the check)
bool board_draw(AppCanvas &canvas);             // true: the board drew the whole screen (the RADR's check)
void board_show(const ctrl_t &c, const boxlink_t &link);   // loop, after each snapshot: lights, sounds
int board_battery_percent();                    // the remote's own battery, 0..100
int board_focus();                              // the right knob's focus (0 A, 1 B, 2 MA) on the run screen; -1 none

// the controls' names on the screens
enum { BT_LIST = 0, BT_OPTIONS_TITLE, BT_OPTIONS_1, BT_OPTIONS_2, BT_PAIR_A, BT_PAIR_B, BT_COUNT };
const char *board_text(int which);

// ---- what the app provides ----------------------------------------------------------------------------------------
void app_setup();
void app_loop();
void app_shutdown();                            // STOP, zero the box, stop its signal, save the settings
AppCanvas &app_canvas();
lgfx::LovyanGFX &app_brand(lgfx::LovyanGFX &g, int x, int mid, int scale);   // the PlaStim wordmark
