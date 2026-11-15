/* The game's own music on or off (docs/CUSTOM-MUSIC.md, docs/CUSTOM-MUSIC-PLAN.md).
 *
 * The game sets a mixer group's volume through one function, 0x71037375f0 (w0 the group, s0 the
 * linear volume). Group 4 is the options menu's music (Sound_Option_Bgm): at 0 the game's music is
 * silent and effects and voices play on. The hook passes 0 for group 4 while the music is off.
 * The game calls it at boot for groups 4, 2 and 3 with the saved option value, and the options
 * menu calls it when its music slider moves; the hook remembers what the game asked for, so that
 * switching the music back on can restore it.
 *
 * Linux and macOS: rt_hook_function patches direct calls on Windows only (hle_detour.c), so there
 * the boot call (a direct call, if it is one) bypasses the hook and the music stays on at boot;
 * calls through the dispatch table, and the change made from the menu, still work. */
#include "audio.h"
#include "display.h"
#include "frame_hooks.h"
#include "host.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUSIC_GROUP_VOLUME 0x71037375f0ull
#define GROUP_OPTION_BGM 4

static volatile LONG game_music = -1;   /* 1 on, 0 off, -1 not read yet */
static volatile LONG pending;           /* a change the game has not been told about yet */
static volatile LONG asked_seen;        /* the game has called the function for group 4 */
static volatile LONG asked_bits;        /* the volume it asked for last (a float's bits) */
static u64 original;

int audio_game_music(void) {
    LONG m = game_music;
    if (m >= 0) return (int)m;
    char v[16] = "";
    const char *env = getenv("SSBU_GAME_MUSIC");
    if (env && *env) snprintf(v, sizeof v, "%s", env);
    else settings_ini_get(display_ini_path(), NULL, "game_music", v, sizeof v);
    m = v[0] == '0' && !v[1] ? 0 : 1;
    InterlockedCompareExchange(&game_music, m, -1);
    if (!m) fprintf(stderr, "[audio] the game's music is off\n");
    return (int)game_music;
}

/* (any thread: the menu's. The game is told from its own thread, at its next frame end.) */
void audio_set_game_music(int on) {
    InterlockedExchange(&game_music, on ? 1 : 0);
    InterlockedExchange(&pending, 1);
}

static float asked_volume(void) {
    LONG b = asked_bits;
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static void music_group_volume(Cpu *c) {
    if ((u32)c->x[0] == GROUP_OPTION_BGM) {
        LONG bits;
        memcpy(&bits, &c->v[0], 4);
        InterlockedExchange(&asked_bits, bits);
        InterlockedExchange(&asked_seen, 1);
        if (!audio_game_music()) { float zero = 0.0f; memcpy(&c->v[0], &zero, 4); }
    }
    rt_call(c, original);
}

/* (the game's thread, after the frame end) Tells the game the menu's change. Group 4 has to have
 * been set once by the game before this, or there is no volume to restore. The registers the call
 * changes are the frame end's results, so all of them are put back. */
static void apply_pending(Cpu *c, const u64 args[8], void *ctx) {
    (void)args; (void)ctx;
    if (!pending || !asked_seen || !original) return;
    InterlockedExchange(&pending, 0);
    Cpu saved = *c;
    float v = audio_game_music() ? asked_volume() : 0.0f;
    c->x[0] = GROUP_OPTION_BGM;
    memcpy(&c->v[0], &v, 4);
    rt_call(c, original);   /* (the game's function itself, not through the hook) */
    *c = saved;
}

/* (main, before the game runs) */
void music_start(void) {
    original = rt_hook_function(MUSIC_GROUP_VOLUME, rt_native(music_group_volume, "music group volume"));
    if (!original) { fprintf(stderr, "[audio] music group volume hook missed: the game's music cannot be switched off\n"); return; }
    frame_hook_add(FRAME_HOOK_END, 400, NULL, apply_pending, NULL);
}
