/* shell.h - the QRT shell ("Ermine"-inspired story UI) and the app ABI. */
#pragma once
#include "../kernel/kernel.h"
#include "gfx.h"

typedef struct {
    int W, H;             /* logical (rotated) canvas size */
    float s;              /* density: px per dp */
    int landscape;
    const font_t *small, *body, *label, *title, *h1, *display, *huge;
    u32 accent;
    u32 text, text2, text3, card, card_hi, stroke, bg_top, bg_bottom;
} ui_t;

extern ui_t ui;
static inline int dp(float v) { return (int)(v * ui.s + 0.5f); }

typedef struct app {
    const char *name;
    const char *blurb;
    u32 color;
    void (*icon)(canvas_t *c, float cx, float cy, float r, u32 fg);
    void (*open)(void);
    void (*draw)(canvas_t *c, rect_t area);
    int  (*event)(const event_t *e, rect_t area);   /* logical coords; 1 = redraw */
    int  (*tick)(u64 now_ms);                      /* called ~60 Hz while open; 1 = redraw */
} app_t;

extern const app_t app_clock, app_sketch, app_files, app_system, app_settings, app_life, app_lab, app_terminal, app_wifi;

/* shell services for apps */
void shell_redraw(void);                /* redraw the whole screen */
void shell_damage(rect_t r);            /* redraw only r (logical coords); call from event/tick */
rect_t shell_app_area(void);            /* where the open app draws */
void shell_keyboard(int show);          /* show/hide the on-screen keyboard (call when a text field is tapped) */
const char *shell_net_status(void);     /* one line about the network for the home screen, or NULL */
/* Wall-clock time of the frame being drawn.  draw() callbacks may run on any
 * CPU core, so they must use this instead of k_walltime() and must not
 * allocate memory, call firmware services or change app state. */
void shell_time(EFI_TIME *t);

typedef struct {
    u32 compose_us, present_us, area_permille, frames;
    char bench[4][96];
} shell_stats_t;
extern shell_stats_t shell_stats;
void shell_go_home(void);
void shell_set_rotation(int rot);
int  shell_rotation(void);
void shell_set_accent(int idx);
int  shell_accent_index(void);
extern const u32 accent_palette[];
extern const char *accent_names[];
#define N_ACCENTS 6

/* widgets */
typedef struct { int down, x0, y0, moved; } tap_t;
int  tap_track(tap_t *t, const event_t *e, int slop);   /* 1 on tap release */
void ui_card(canvas_t *c, rect_t r, int radius, int hi);
void ui_button(canvas_t *c, rect_t r, const char *label, u32 fill, u32 fg);
void ui_chip(canvas_t *c, rect_t r, const char *label, int selected);
void ui_section(canvas_t *c, int x, int y, const char *title);
/* vertical drag / wheel scrolling for long content */
typedef struct { int off, max, dragging, last_y, moved; } scroll_t;
int  scroll_event(scroll_t *s, const event_t *e, rect_t area, int step);

void ui_kv(canvas_t *c, rect_t r, const char *key, const char *value);
