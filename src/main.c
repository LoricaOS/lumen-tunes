/* user/bin/tunes/main.c — Tunes, a minimal WAV/MP3 player (external Lumen client)
 *
 * Opens the audio file given as argv[1] (the file manager spawns it on a
 * .wav/.mp3) and plays it through /dev/audio via libaudio, which streams with
 * backpressure so a track of any length plays. Playback runs in a forked child
 * (audio_play_file_async) so the window stays responsive; Stop kills it. A
 * SIGCHLD tells us when the track finishes on its own.
 *
 * Keys: Space/Enter play-pause toggle, S/Esc stop, R restart, Q/close quit.
 * Or click the round transport buttons.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#include <glyph.h>
#include <lumen_client.h>
#include "font.h"
#include "audio.h"

#define SYS_AUDIO_STOP 504

#define WIN_W   460
#define WIN_H   250

/* Album-art card on the left; a text + transport column on the right. */
#define ART     170
#define ART_X   28
#define ART_Y   ((WIN_H - ART) / 2)          /* 40 */
#define RX      (ART_X + ART + 28)           /* right column x, 226 */
#define COLW    (WIN_W - RX - 28)            /* right column width, 206 */

#define TITLE_PX 20
#define SEEK_Y   124
#define SEEK_H   6

/* Round transport buttons, centered in the right column. */
#define TB_CY    192
#define PLAY_CX  (RX + COLW / 2)
#define PLAY_R   26
#define RST_CX   (PLAY_CX - (PLAY_R + 16 + 17))
#define RST_R    17

enum { ST_READY, ST_PLAYING, ST_STOPPED, ST_DONE, ST_NOFILE };

typedef struct {
    int             lfd;
    lumen_window_t *lwin;
    surface_t       surf;
    int             w, h;

    char            path[512];
    char            name[160];
    int             has_file;

    pid_t           pid;            /* >0 while a playback child runs */
    int             status;
    int             dur_ms;         /* track length, 0 if unknown */
    long            start_ms;       /* monotonic ms at playback start */
    int             dirty;
} tunes_t;

/* Monotonic milliseconds. */
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void fmt_time(int ms, char *out, int n)
{
    if (ms < 0) ms = 0;
    int s = ms / 1000;
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static tunes_t g_t;
static volatile sig_atomic_t s_term;
static volatile sig_atomic_t s_child;       /* a child exited (reap in main) */
static volatile sig_atomic_t s_stopping;    /* the exit was a user Stop */

static void on_term(int s) { (void)s; s_term = 1; }
static void on_chld(int s) { (void)s; s_child = 1; }

static void basename_of(const char *p, char *out, int n)
{
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s == '/') b = s + 1;
    snprintf(out, n, "%s", b);
}

/* ── Rendering ────────────────────────────────────────────────────────────── */

static int at_least(int v, int min) { return v < min ? min : v; }

/* Larger UI text via the TTF font when available, else the bitmap UI font. */
static void text_px(int x, int y, const char *str, uint32_t col, int px)
{
    if (g_font_ui)
        font_draw_text(&g_t.surf, g_font_ui, px, x, y, str, col);
    else
        draw_text_ui(&g_t.surf, x, y, str, col);
}
static int text_px_w(const char *str, int px)
{
    return g_font_ui ? font_text_width(g_font_ui, px, str) : glyph_text_width(str);
}

/* Trim in place until it fits maxw at the given size. */
static void fit(char *buf, int px, int maxw)
{
    while (buf[0] && text_px_w(buf, px) > maxw)
        buf[strlen(buf) - 1] = '\0';
}

static const char *format_label(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot) {
        if (!strcasecmp(dot, ".mp3")) return "MP3 audio";
        if (!strcasecmp(dot, ".wav")) return "WAV audio";
    }
    return "Audio";
}

/* Transport glyphs, centered on (cx,cy). */
static void glyph_play(int cx, int cy, int h, uint32_t col)
{
    int half = h / 2, leftX = cx - h / 3, i;
    for (i = 0; i < h; i++) {
        int a = i - half, w = half - (a < 0 ? -a : a);
        draw_fill_rect(&g_t.surf, leftX, cy - half + i, at_least(w, 1), 1, col);
    }
}
static void glyph_stop(int cx, int cy, int sz, uint32_t col)
{
    draw_fill_rect(&g_t.surf, cx - sz / 2, cy - sz / 2, sz, sz, col);
}
static void glyph_restart(int cx, int cy, int h, uint32_t col)
{
    int half = h / 2, bar = at_least(h / 6, 2);
    int lx = cx - half + bar + 1, i;
    draw_fill_rect(&g_t.surf, cx - half, cy - half, bar, h, col);   /* |◀ bar */
    for (i = 0; i < h; i++) {                                       /* apex left */
        int a = i - half, w = half - (a < 0 ? -a : a);
        w = at_least(w, 1);
        draw_fill_rect(&g_t.surf, lx + (half - w), cy - half + i, w, 1, col);
    }
}

/* A vinyl record as album art — dark disc, grooves, accent center label. */
static void draw_album_art(int playing)
{
    surface_t *s = &g_t.surf;
    int cx = ART_X + ART / 2, cy = ART_Y + ART / 2;
    int R = ART / 2 - 16, gr;

    draw_rounded_rect(s, ART_X, ART_Y, ART, ART, R_LG, THEME_SURFACE_2);
    draw_circle_filled(s, cx, cy, R, 0x00151519);          /* the record */
    for (gr = R - 8; gr > R / 3; gr -= 8)                  /* grooves */
        draw_circle(s, cx, cy, gr, 0x0026262E);
    draw_circle_filled(s, cx, cy, R / 3,
                       playing ? THEME_ACCENT : 0x003A3A44); /* label */
    draw_circle_filled(s, cx, cy, at_least(R / 16, 2), 0x00151519); /* spindle */
}

static void render(void)
{
    if (!g_t.dirty) return;
    g_t.dirty = 0;
    surface_t *s = &g_t.surf;
    int playing = (g_t.status == ST_PLAYING);

    draw_gradient_v(s, 0, 0, g_t.w, g_t.h, THEME_SURFACE_2, THEME_SURFACE);
    draw_album_art(playing);

    /* Title + subtitle. */
    char title[160];
    snprintf(title, sizeof(title), "%s", g_t.has_file ? g_t.name : "No track loaded");
    fit(title, TITLE_PX, COLW);
    text_px(RX, 52, title, g_t.has_file ? THEME_TEXT : THEME_TEXT_DIM, TITLE_PX);

    /* Subtitle: just the format (the duration already sits at the seek end). */
    const char *sub = g_t.has_file ? format_label(g_t.name)
                                   : "Open a .wav or .mp3";
    draw_text_ui(s, RX, 84, sub, THEME_TEXT_DIM);

    /* Seek bar with elapsed / total at the ends. */
    int elapsed = playing ? (int)(now_ms() - g_t.start_ms) : 0;
    if (g_t.status == ST_DONE) elapsed = g_t.dur_ms;
    if (g_t.dur_ms > 0 && elapsed > g_t.dur_ms) elapsed = g_t.dur_ms;

    draw_rounded_rect(s, RX, SEEK_Y, COLW, SEEK_H, SEEK_H / 2, THEME_SURFACE_2);
    if (g_t.has_file && g_t.dur_ms > 0) {
        int fillw = (int)((long long)COLW * elapsed / g_t.dur_ms);
        if (fillw > 0)
            draw_rounded_rect(s, RX, SEEK_Y, fillw, SEEK_H, SEEK_H / 2, THEME_ACCENT);
        if (playing)   /* seek knob */
            draw_circle_filled(s, RX + fillw, SEEK_Y + SEEK_H / 2, 5, THEME_ACCENT);
    }
    char te[12], tt[12];
    fmt_time(g_t.has_file ? elapsed : 0, te, sizeof(te));
    fmt_time(g_t.has_file ? g_t.dur_ms : 0, tt, sizeof(tt));
    draw_text_ui(s, RX, SEEK_Y + SEEK_H + 8, te, THEME_TEXT_DIM);
    draw_text_ui(s, RX + COLW - glyph_text_width(tt), SEEK_Y + SEEK_H + 8, tt,
                 THEME_TEXT_DIM);

    /* Transport: restart + big play/stop toggle. */
    uint32_t rst_fg = g_t.has_file ? THEME_TEXT : THEME_TEXT_DIM;
    draw_circle_filled(s, RST_CX, TB_CY, RST_R, THEME_SURFACE_2);
    glyph_restart(RST_CX, TB_CY, RST_R, rst_fg);

    draw_circle_filled(s, PLAY_CX, TB_CY, PLAY_R,
                       g_t.has_file ? THEME_ACCENT : THEME_SURFACE_2);
    uint32_t play_fg = g_t.has_file ? THEME_TEXT_ON_ACCENT : THEME_TEXT_DIM;
    if (playing)
        glyph_stop(PLAY_CX, TB_CY, PLAY_R - 6, play_fg);
    else
        glyph_play(PLAY_CX, TB_CY, PLAY_R - 4, play_fg);

    /* A small status line only when there's something worth saying. */
    const char *st =
        g_t.status == ST_STOPPED ? "Stopped" :
        g_t.status == ST_DONE    ? "Finished" :
        g_t.status == ST_NOFILE  ? "Nothing to play" : NULL;
    if (st) {
        int sw = glyph_text_width(st);
        draw_text_ui(s, PLAY_CX - sw / 2, TB_CY + PLAY_R + 12, st, THEME_TEXT_DIM);
    }

    if (g_t.lwin)
        lumen_window_present(g_t.lwin);
}

/* ── Actions ──────────────────────────────────────────────────────────────── */

static void do_play(void)
{
    if (!g_t.has_file || g_t.status == ST_PLAYING) return;
    g_t.dur_ms = audio_duration_ms(g_t.path);
    pid_t pid = audio_play_file_async(g_t.path);
    if (pid < 0) return;
    g_t.pid = pid;
    g_t.start_ms = now_ms();
    g_t.status = ST_PLAYING;
    g_t.dirty = 1;
    dprintf(2, "[TUNES] play %s pid=%d dur=%dms\n", g_t.path, (int)pid, g_t.dur_ms);
}

static void do_stop(void)
{
    if (g_t.status != ST_PLAYING || g_t.pid <= 0) return;
    s_stopping = 1;
    kill(g_t.pid, SIGKILL);          /* stop the producer */
    syscall(SYS_AUDIO_STOP);         /* halt the DMA now (no drain tail) */
    dprintf(2, "[TUNES] stop pid=%d\n", (int)g_t.pid);
}

/* Replay from the top. libaudio has no seek, so restart == stop + play from 0.
 * Reap the old child synchronously and clear its pending SIGCHLD so the main
 * loop's reaper doesn't mistake it for the new playback ending. */
static void do_restart(void)
{
    if (!g_t.has_file) return;
    if (g_t.pid > 0) {
        kill(g_t.pid, SIGKILL);
        syscall(SYS_AUDIO_STOP);
        waitpid(g_t.pid, NULL, 0);
        g_t.pid = 0;
    }
    s_child = 0; s_stopping = 0;
    g_t.status = ST_STOPPED;
    do_play();
}

/* Play/stop toggle on the big transport button. */
static void do_toggle(void)
{
    if (g_t.status == ST_PLAYING) do_stop();
    else                          do_play();
}

static int in_circle(int mx, int my, int cx, int cy, int r)
{
    int dx = mx - cx, dy = my - cy;
    return dx * dx + dy * dy <= r * r;
}

/* Returns 0 to quit, 1 to keep running. */
static int handle_key(uint8_t k)
{
    switch (k) {
    case 'q': case 'Q':            return 0;
    case 0x1B:                     do_stop();    return 1;   /* Esc */
    case ' ': case '\r': case '\n': do_toggle(); return 1;
    case 's': case 'S':            do_stop();    return 1;
    case 'r': case 'R':            do_restart(); return 1;
    default:                       return 1;
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0]) {
        snprintf(g_t.path, sizeof(g_t.path), "%s", argv[1]);
        basename_of(g_t.path, g_t.name, sizeof(g_t.name));
        g_t.has_file = 1;
        g_t.dur_ms = audio_duration_ms(g_t.path);   /* show total before play */
        g_t.status = ST_READY;
    } else {
        g_t.status = ST_NOFILE;
    }

    g_t.lfd = lumen_connect_retry();
    if (g_t.lfd < 0) { dprintf(2, "[TUNES] lumen_connect failed\n"); return 1; }

    g_t.lwin = lumen_window_create(g_t.lfd, "Tunes", WIN_W, WIN_H);
    if (!g_t.lwin) { dprintf(2, "[TUNES] window_create failed\n"); close(g_t.lfd); return 1; }
    g_t.w = g_t.lwin->w; g_t.h = g_t.lwin->h;
    g_t.surf = (surface_t){ .buf = (uint32_t *)g_t.lwin->backbuf,
                            .w = g_t.w, .h = g_t.h, .pitch = g_t.lwin->stride };

    font_init();
    dprintf(2, "[TUNES] connected %dx%d file=%s\n",
            g_t.w, g_t.h, g_t.has_file ? g_t.name : "(none)");

    struct sigaction sa = {0};
    sa.sa_handler = on_term; sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_chld; sigaction(SIGCHLD, &sa, NULL);

    /* Auto-play when launched on a file (the file-manager open path). */
    if (g_t.has_file) do_play();

    g_t.dirty = 1;
    render();

    while (!s_term) {
        lumen_event_t ev;
        int r = lumen_wait_event(g_t.lfd, &ev, 300);

        if (s_child) {                 /* playback child exited */
            s_child = 0;
            while (waitpid(-1, NULL, WNOHANG) > 0) { }
            if (g_t.status == ST_PLAYING) {
                g_t.status = s_stopping ? ST_STOPPED : ST_DONE;
                g_t.pid = 0;
                s_stopping = 0;
                g_t.dirty = 1;
            }
        }

        if (r < 0) break;
        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) break;
            else if (ev.type == LUMEN_EV_KEY && ev.key.pressed) {
                if (!handle_key((uint8_t)ev.key.keycode)) break;
            } else if (ev.type == LUMEN_EV_MOUSE &&
                       ev.mouse.evtype == LUMEN_MOUSE_DOWN && (ev.mouse.buttons & 1)) {
                if (in_circle(ev.mouse.x, ev.mouse.y, PLAY_CX, TB_CY, PLAY_R))
                    do_toggle();
                else if (in_circle(ev.mouse.x, ev.mouse.y, RST_CX, TB_CY, RST_R))
                    do_restart();
            }
        }
        if (g_t.status == ST_PLAYING)
            g_t.dirty = 1;           /* advance the progress bar each tick */
        render();
    }

    do_stop();                         /* don't leave audio playing after quit */
    lumen_window_destroy(g_t.lwin);
    close(g_t.lfd);
    dprintf(2, "[TUNES] exit\n");
    return 0;
}
