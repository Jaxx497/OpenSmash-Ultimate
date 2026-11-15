/* The custom-music player (hle_music_player.c, docs/CUSTOM-MUSIC-PLAN.md): the player's own music files, decoded
 * with FFmpeg (loaded at run time: without its DLLs the feature says so and the game is unaffected),
 * mixed into the game's own audio output (ipc_audio.c's feed).
 *
 * Settings, read once at start (the menu writes display.ini itself, the setters below do not):
 *   music (0/1, off)  music_volume (0..100, 70)  music_shuffle (0/1, 1)  music_source (a folder, empty = all)
 * with SSBU_MUSIC, SSBU_MUSIC_VOLUME, SSBU_MUSIC_SHUFFLE and SSBU_MUSIC_SOURCE over them. Libraries: the
 * music\ folder of the game and the [libraries] section of music.ini beside display.ini (one path per line).
 * Every function but music_mix may be called from any thread. */
#pragma once
#include <stdint.h>

void music_player_start(void);   /* (host.c's main) reads the settings; with music on, loads FFmpeg and starts */
int music_available(void);       /* FFmpeg is there (known after the first try: music on, or music_set_enabled) */

int music_enabled(void);
/* On: loads FFmpeg (once), starts the scan and the player, and turns the game's own music off
 * (audio_set_game_music(0)). Off: fades out, turns the game's music back on. Returns whether it is on
 * afterwards (0 when FFmpeg is missing: the game's music then stays as it was). */
int music_set_enabled(int on);
int music_volume(void);
void music_set_volume(int percent);   /* 0..100, ramped over 5 ms */
int music_shuffle(void);
void music_set_shuffle(int on);       /* from the next track on */
void music_get_source(char *out, int size);
void music_set_source(const char *folder);   /* "" = everything; plays another track at once */
void music_next(void);
void music_previous(void);            /* the track played before this one, else this one again */
void music_pause(int paused);
int music_paused(void);
void music_rescan(void);              /* in the background; the playing track goes on */
int music_scanning(void);
int music_track_count(void);

typedef struct { char path[520], name[256]; int tracks, playlist; } MusicFolder;
/* The "Playing from" picker's choices (utf-8): the folders that hold tracks, sorted, each with the number of
 * tracks directly in it, then the playlists (.m3u/.m3u8, path = the file, name = its name without the
 * extension, playlist = 1, tracks = the ones that exist). Either path goes to music_set_source. Fills at
 * most max; returns how many there are. */
int music_folders(MusicFolder *out, int max);

typedef struct {
    int state;               /* 0 nothing playing, 1 playing, 2 paused */
    char title[256], artist[256];   /* tags, else the file's name; artist may be empty */
    double position, duration;      /* seconds; duration 0 when the file does not say */
} MusicNow;
void music_now(MusicNow *now);

/* The user picked this very track (its path): it is opened here and now. 1: it plays (after a 5 ms fade of
 * what played, the order goes on from it). 0: it cannot, `why` says so in words to show ("Couldn't play
 * \"<title>\": the file doesn't open"), and what played goes on, uninterrupted. This is the only error
 * the player shows: a broken track reached by shuffle, order, next, previous or a folder or playlist is
 * skipped, with a log line, and nothing else. */
int music_play_track(const char *path, char *why, int size);

/* (ipc_audio.c's feed, the output thread, never blocks) Adds the music to `frames` stereo 16-bit frames
 * of game sound, times its volume and master (0..100, the game's master volume). */
void music_mix(int16_t *out, uint32_t frames, int master);
