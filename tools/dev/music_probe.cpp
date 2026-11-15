// Throwaway probe: which of the game's mixer-group calls silences the music and leaves the effects?
// SSBU_MUSIC_PROBE = log | vol0 | mute1 | mute4 | vol1 (group 1 volume 0)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hook.h"

static const HookApi *api;
static HookOrig orig_vol, orig_mute, orig_pause;
static const char *mode = "log";

static float getf(Ctx *c) { float f; memcpy(&f, &c->v[0].d[0], 4); return f; }
static void setf(Ctx *c, float f) { uint32_t u; memcpy(&u, &f, 4); c->v[0].d[0] = u; c->v[0].d[1] = 0; }

static void my_vol(Ctx *c, void *) {
    uint32_t group = (uint32_t)c->x[0];
    char line[128];
    snprintf(line, sizeof line, "[probe] volume(group %u, %.3f) from %llx", group, getf(c), (unsigned long long)c->x[30]);
    api->log(line);
    if (group == 4 && !strcmp(mode, "vol0")) setf(c, 0.0f);
    orig_vol(c);
    if (group == 4 && !strncmp(mode, "mute", 4)) {
        c->x[0] = (uint64_t)(mode[4] - '0'); c->x[1] = 1;
        api->call(c, 0x7103737a20ull);
        api->log("[probe] mute sent");
    }
    if (group == 4 && !strcmp(mode, "vol1")) {
        c->x[0] = 1; setf(c, 0.0f);
        orig_vol(c);
        api->log("[probe] group 1 volume 0 sent");
    }
}
static void my_mute(Ctx *c, void *) {
    char line[128];
    snprintf(line, sizeof line, "[probe] mute(group %u, %u) from %llx", (unsigned)c->x[0], (unsigned)c->x[1], (unsigned long long)c->x[30]);
    api->log(line);
    orig_mute(c);
}
static void my_pause(Ctx *c, void *) {
    char line[128];
    snprintf(line, sizeof line, "[probe] pause(group %u, %u, %u) from %llx", (unsigned)c->x[0], (unsigned)c->x[1], (unsigned)c->x[2], (unsigned long long)c->x[30]);
    api->log(line);
    orig_pause(c);
}

extern "C" MOD_EXPORT void mod_init(const HookApi *a) {
    api = a;
    if (getenv("SSBU_MUSIC_PROBE")) mode = getenv("SSBU_MUSIC_PROBE");
    int a1 = api->install(0x71037375f0ull, my_vol, nullptr, &orig_vol);
    int a2 = api->install(0x7103737a20ull, my_mute, nullptr, &orig_mute);
    int a3 = api->install(0x7103737820ull, my_pause, nullptr, &orig_pause);
    char line[96];
    snprintf(line, sizeof line, "[probe] mode %s, hooks %d %d %d", mode, a1, a2, a3);
    api->log(line);
}
