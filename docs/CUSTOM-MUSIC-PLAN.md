# Custom music: build plan

Findings are in [CUSTOM-MUSIC.md](CUSTOM-MUSIC.md). Owner's decisions (2026-11-15): the settings tab is
"Audio"; music folders can be changed later and there can be several; formats mp3, m4a, flac, wav, ogg, opus.

## Step 2: silence the game's music (hand to a Sonnet subagent)

1. New `runtime/hle_music.c`. At boot, hook the game's mixer-group volume function
   `0x71037375f0 (w0 = group, s0 = volume)` with `rt_hook_function(addr, rt_native(fn, "music group volume"))`
   (as `runtime/hle_paramconfig.c:322` does). In the hook, for group 4 (`Sound_Option_Bgm`), store the
   volume the game asked for, and pass 0.0 instead while game music is off. Call the original for every group.
2. Applying a change while playing: never call the game from the menu thread. Set a pending flag, and
   apply it from a frame hook (`frame_hook_add`, `runtime/frame_hooks.h`) on the game thread: save the
   Cpu registers it touches, put group 4 in x0 and the volume (stored value, or 0) in s0, call the
   original through `rt_call`, then restore the registers.
3. Setting: `display.ini` key `game_music` (1 default, 0 off), read and written the same way as
   `volume` (`audio_master_volume`, `runtime/ipc_audio.c:63`). Expose
   `int audio_game_music(void)` and `void audio_set_game_music(int on)` in `runtime/audio.h`.
4. Settings tab: add "Audio" to `PAGE_NAMES` (`runtime/gpu_menu.c:139`), move the Volume row
   (`R_VOLUME`, around line 1250) there, and add a "Game music" on/off row with help text.
5. Tests: update the menu harness (`tools/dev/settingstest.c`) and `tests/test_settings.py` for the new
   page and the moved row; run the tests.
6. Check in the game: a fight with "Game music" off: stage music silent, effects and voices play;
   switching it back on in a match brings the music back; the options menu's music slider still works
   when on.

## Step 1: play the player's music (open: library choice)

- Folders: `display.ini` key `music_folders`, paths separated by `;` (`E:\Albums;E:\OST`), scanned
  recursively. The Audio tab shows the count; editing the list goes in the launcher later.
- Library, recommended: **libmpv**, loaded at run time (`LoadLibrary` / `dlopen`, as
  `runtime/gpu_vulkan.c:66` loads Vulkan) so the game runs without it. It covers all six formats, the
  playlist, shuffle, gapless playback and resampling, and plays on its own threads, so it continues
  through loading screens. About 100 lines against FFmpeg's few hundred (decode thread, resampler,
  ring, mixing in `feed()`). Cost: libmpv-2.dll is large (tens of MB) and is a prebuilt binary that the
  launcher has to download at a pinned version and check by SHA256, as `tools/runtime_fetch.py` does.
  It plays as a second audio stream next to the game's, mixed by Windows.
- Alternative: vendored decoders built from source like libopus and libvpx (`tools/codec_libs.py`):
  dr_mp3, dr_flac, dr_wav, stb_vorbis, opusfile + libogg, faad2 + minimp4 for m4a (AAC only). No
  download, more code and six libraries to maintain.
- Audio tab rows: Music on/off (turns game music off with it), Music volume, Shuffle, Next track,
  Now playing.
