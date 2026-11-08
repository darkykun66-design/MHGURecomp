// Basic touch controls (runtime/touch.cpp): a floating stick on the left, the game's buttons around the right thumb,
// a drag anywhere else turns the camera, and a menu layout (d-pad, A/B/X/Y, L/R) switched by an on-screen button.
// Shown on Android while no controller is connected. MHGU_TOUCH=1 forces them on (a desktop with a touch screen),
// MHGU_TOUCH=0 turns them off.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// host main thread (runtime/host_input.c)
bool touch_event(const SDL_Event *e);  // a finger event: true when it was the controls' (always, for finger events)
// once per input update: adds the controls' buttons and left stick, and gives the camera drag in pixels of a 720 line screen
void touch_input(bool pad_connected, bool typing, uint64_t *buttons, int *lx, int *ly, float *cam_dx, float *cam_dy);
bool touch_active(void);  // the controls are switched on (Android, or MHGU_TOUCH=1): SDL's mouse events made from fingers are dropped
void touch_reset(void);  // every finger gone (the window lost focus)
// game thread (runtime/overlay.cpp), inside the frame, under the port's menus
bool touch_drawn(void);
void touch_draw(float display_w, float display_h);

#ifdef __cplusplus
}
#endif
