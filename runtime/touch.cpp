// Basic touch controls: see touch.h.
//   - left thumb: a floating stick (it appears where the thumb lands, in the left part of the screen)
//   - right thumb: A / X / B / Y in a diamond, R, ZR, L, ZL, Plus (the game's menu) and Minus (the map)
//   - a drag anywhere else turns the camera
//   - the "MENU" button switches to a menu layout: a d-pad on the left, A / B / X / Y, L / R, and no camera drag
//   - the gear opens the port's settings menu
// Fingers arrive on the host's main thread (touch_event, touch_input); the game thread draws (touch_draw).
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "imgui.h"

extern "C" {
#include "hle/input.h"
}
#include "overlay.h"
#include "touch.h"

namespace {

#define B(x) (1ull << (x))

enum Id { ID_NONE = -1, ID_GEAR = 100, ID_MODE = 101 };

struct Ctl {
    int id;          // a BTN_* bit, or ID_GEAR / ID_MODE
    float x, y, r;   // centre and radius, in screen pixels
    const char *label;
};

struct Finger {
    bool used = false;
    SDL_FingerID id = 0;
    enum Kind { NONE, BUTTON, STICK, CAMERA } kind = NONE;
    int ctl = ID_NONE;
    float x = 0, y = 0;    // now
    float ox = 0, oy = 0;  // where the stick began
};

constexpr int MAX_FINGERS = 10;
constexpr float CAMERA_GAIN = 1.6f;  // mouse counts per pixel of a 720 line screen: the mouse sensitivity setting scales it

std::mutex g_lock;
Finger g_f[MAX_FINGERS];
float g_w = 1280, g_h = 720;  // the overlay's size, in pixels (what the controls are drawn on)
bool g_menu_mode;             // the menu layout
float g_cam_x, g_cam_y;       // camera drag not yet given to the game
std::atomic<bool> g_shown{false};

int g_enabled = -1;
bool enabled() {
    if (g_enabled < 0) {
        const char *e = getenv("MHGU_TOUCH");
#ifdef __ANDROID__
        g_enabled = e && e[0] == '0' ? 0 : 1;
#else
        g_enabled = e && e[0] == '1' ? 1 : 0;
#endif
    }
    return g_enabled != 0;
}

// The controls of the current layout, placed from the screen's height (a 1280x720 screen is the design size)
int layout(Ctl out[], bool menu) {
    float s = g_h / 720.0f, w = g_w, h = g_h;
    int n = 0;
    auto add = [&](int id, float x, float y, float r, const char *label) { out[n++] = {id, x, y, r * s, label}; };
    float ax = w - 210 * s, ay = h - 185 * s;  // the face buttons' centre
    const float d = 92 * s;
    add(BTN_X, ax, ay - d, 46, "X");
    add(BTN_A, ax + d, ay, 46, "A");
    add(BTN_B, ax, ay + d, 46, "B");
    add(BTN_Y, ax - d, ay, 46, "Y");
    if (menu) {
        float dx = 210 * s, dy = h - 185 * s;  // the d-pad's centre
        add(BTN_UP, dx, dy - d, 46, "^");
        add(BTN_RIGHT, dx + d, dy, 46, ">");
        add(BTN_DOWN, dx, dy + d, 46, "v");
        add(BTN_LEFT, dx - d, dy, 46, "<");
        add(BTN_L, 150 * s, 200 * s, 40, "L");
        add(BTN_R, w - 150 * s, 200 * s, 40, "R");
    } else {
        add(BTN_R, w - 70 * s, ay - 250 * s, 42, "R");
        add(BTN_ZR, w - 70 * s, ay - 355 * s, 42, "ZR");
        add(BTN_L, w - 300 * s, ay - 250 * s, 42, "L");
        add(BTN_ZL, w - 300 * s, ay - 355 * s, 42, "ZL");
    }
    add(BTN_MINUS, w * 0.5f - 90 * s, 42 * s, 30, "-");
    add(BTN_PLUS, w * 0.5f + 90 * s, 42 * s, 30, "+");
    add(ID_GEAR, 50 * s, 42 * s, 30, "SET");
    add(ID_MODE, 150 * s, 42 * s, 30, menu ? "PLAY" : "MENU");
    return n;
}

int hit_control(float x, float y, bool menu) {
    Ctl c[16];
    int n = layout(c, menu);
    int best = ID_NONE;
    float best_d = 1e9f;
    for (int i = 0; i < n; i++) {
        float dx = x - c[i].x, dy = y - c[i].y, d = std::sqrt(dx * dx + dy * dy);
        if (d <= c[i].r * 1.25f && d < best_d) best = c[i].id, best_d = d;
    }
    return best;
}

float stick_radius() { return 105.0f * g_h / 720.0f; }

Finger *find(SDL_FingerID id) {
    for (Finger &f : g_f)
        if (f.used && f.id == id) return &f;
    return nullptr;
}

void release_all() {
    for (Finger &f : g_f) f = Finger();
    g_cam_x = g_cam_y = 0;
}

}  // namespace

extern "C" bool touch_event(const SDL_Event *e) {
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION && e->type != SDL_EVENT_FINGER_UP &&
        e->type != SDL_EVENT_FINGER_CANCELED)
        return false;
    if (!enabled()) return false;
    bool open_settings = false;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (!g_shown.load()) return true;  // (hidden, or the port's menu is open: that one gets the mouse events SDL makes)
        float x = e->tfinger.x * g_w, y = e->tfinger.y * g_h;
        if (e->type == SDL_EVENT_FINGER_DOWN) {
            Finger *f = nullptr;
            for (Finger &g : g_f)
                if (!g.used) { f = &g; break; }
            if (!f) return true;
            *f = Finger();
            f->used = true, f->id = e->tfinger.fingerID, f->x = f->ox = x, f->y = f->oy = y;
            int c = hit_control(x, y, g_menu_mode);
            if (c == ID_GEAR) {
                open_settings = true;  // (a tap: the menu opens; its own fingers are the mouse)
                f->used = false;
            } else if (c == ID_MODE) {
                g_menu_mode = !g_menu_mode;
                release_all();
            } else if (c != ID_NONE) {
                f->kind = Finger::BUTTON, f->ctl = c;
            } else if (!g_menu_mode) {
                bool left = x < g_w * 0.42f && y > g_h * 0.22f;
                f->kind = left ? Finger::STICK : Finger::CAMERA;
                if (left) {  // (the stick's base stays whole on the screen)
                    float r = stick_radius();
                    f->ox = std::clamp(x, r, g_w - r), f->oy = std::clamp(y, r, g_h - r);
                }
            }
        } else if (Finger *f = find(e->tfinger.fingerID)) {
            if (e->type == SDL_EVENT_FINGER_MOTION) {
                if (f->kind == Finger::CAMERA) g_cam_x += x - f->x, g_cam_y += y - f->y;
                f->x = x, f->y = y;
            } else {
                *f = Finger();
            }
        }
    }
    if (open_settings) overlay_toggle_settings();
    return true;
}

extern "C" void touch_input(bool pad_connected, bool typing, uint64_t *buttons, int *lx, int *ly, float *cam_dx,
                            float *cam_dy) {
    *cam_dx = *cam_dy = 0;
    std::lock_guard<std::mutex> l(g_lock);
    bool show = enabled() && !pad_connected && !typing && !overlay_menu_open();
    if (show != g_shown.load()) {
        g_shown.store(show);
        release_all();
    }
    if (!show) return;
    uint64_t b = 0;
    for (const Finger &f : g_f) {
        if (!f.used) continue;
        if (f.kind == Finger::BUTTON && f.ctl < 64) b |= B(f.ctl);
        if (f.kind == Finger::STICK) {
            float dx = f.x - f.ox, dy = f.y - f.oy, r = stick_radius(), m = std::sqrt(dx * dx + dy * dy) / r;
            if (m < 0.12f) continue;  // (a dead zone)
            float k = std::min(m, 1.0f) / m;
            *lx += (int)(dx * k / r * 32767.0f), *ly += (int)(-dy * k / r * 32767.0f);
        }
    }
    *buttons |= b;
    float to720 = 720.0f / g_h * CAMERA_GAIN;
    *cam_dx = g_cam_x * to720, *cam_dy = g_cam_y * to720;
    g_cam_x = g_cam_y = 0;
}

extern "C" bool touch_active(void) { return enabled(); }

extern "C" void touch_reset(void) {
    std::lock_guard<std::mutex> l(g_lock);
    release_all();
}

extern "C" bool touch_drawn(void) { return g_shown.load(); }

extern "C" void touch_draw(float display_w, float display_h) {
    std::lock_guard<std::mutex> l(g_lock);
    g_w = display_w, g_h = display_h;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    const ImU32 rim = IM_COL32(197, 138, 46, 200), rim_down = IM_COL32(255, 214, 120, 255);
    const ImU32 fill = IM_COL32(24, 20, 22, 120), fill_down = IM_COL32(120, 82, 30, 190);
    const ImU32 text = IM_COL32(245, 233, 205, 235);
    float s = g_h / 720.0f;

    uint64_t down = 0;
    const Finger *stick = nullptr;
    for (const Finger &f : g_f) {
        if (!f.used) continue;
        if (f.kind == Finger::BUTTON && f.ctl < 64) down |= B(f.ctl);
        if (f.kind == Finger::STICK) stick = &f;
    }

    if (!g_menu_mode) {  // the stick: where the thumb is, or its resting place
        float r = stick_radius();
        ImVec2 base = stick ? ImVec2(stick->ox, stick->oy) : ImVec2(190 * s + r * 0.3f, g_h - 190 * s);
        ImVec2 knob = base;
        if (stick) {
            float dx = stick->x - stick->ox, dy = stick->y - stick->oy, m = std::sqrt(dx * dx + dy * dy);
            if (m > r) dx *= r / m, dy *= r / m;
            knob = ImVec2(base.x + dx, base.y + dy);
        }
        dl->AddCircleFilled(base, r, fill, 48);
        dl->AddCircle(base, r, stick ? rim_down : rim, 48, 3.0f * s);
        dl->AddCircleFilled(knob, r * 0.42f, stick ? fill_down : fill, 32);
        dl->AddCircle(knob, r * 0.42f, stick ? rim_down : rim, 32, 3.0f * s);
    }

    Ctl c[16];
    int n = layout(c, g_menu_mode);
    for (int i = 0; i < n; i++) {
        bool pressed = c[i].id < 64 && (down & B(c[i].id));
        ImVec2 p(c[i].x, c[i].y);
        dl->AddCircleFilled(p, c[i].r, pressed ? fill_down : fill, 40);
        dl->AddCircle(p, c[i].r, pressed ? rim_down : rim, 40, 3.0f * s);
        float size = c[i].r * (strlen(c[i].label) > 2 ? 0.55f : 0.9f);
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, c[i].label);
        dl->AddText(font, size, ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), text, c[i].label);
    }
}
