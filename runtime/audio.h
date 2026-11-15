/* The game's sound, as the rest of the runtime sees it (ipc_audio.c). */
#pragma once

/* The master volume, 0 to 100 (%), over what the game sets itself: SSBU_VOLUME, else display.ini's
 * volume, else 100. The settings menu changes it while playing (and writes display.ini). */
int audio_master_volume(void);
void audio_set_master_volume(int percent);
/* The game's own music, on or off (hle_music.c): SSBU_GAME_MUSIC, else display.ini's game_music
 * (1 or absent on, 0 off). Off silences the music and keeps effects and voices. The setter may be
 * called from any thread; the game is told at its next frame end. */
int audio_game_music(void);
void audio_set_game_music(int on);
