# Custom music: build plan

Findings are in [CUSTOM-MUSIC.md](CUSTOM-MUSIC.md). Owner's decisions (2026-11-15): the settings tab is
"Audio"; music folders can be changed later and there can be several; formats mp3, m4a, flac, wav, ogg, opus.

## Step 2: silence the game's music (done; checked in the game)

The owner checked this in the game on 2026-11-15 and all checks passed: a fight with game music off, switching
it on mid-match, the options slider, and a restart with it off silent from the title.

What it does now: `runtime/hle_music.c` hooks the group volume function (installed from `main` in
`host.c`, after the modules load, before the game runs) and passes 0 for group 4 while `game_music` is
off; `audio_game_music()` / `audio_set_game_music()` (`runtime/audio.h`) hold the setting
(`SSBU_GAME_MUSIC`, else `display.ini` `game_music`, 1 default). A change from the menu is applied by a
`FRAME_HOOK_END` handler on the game thread, once the game has called the function for group 4 once.
The settings menu has an "Audio" tab (after Display) with the moved Volume row and "Game music".
Adding it changes no game-code fingerprint (`tools/recomp.py` hashes `HLE(...)` names, `HLE_WRAP`
targets and `tools/hooks.txt`; this uses a run-time `rt_hook_function` and none of those), and the
build globs `runtime/hle*.c`, so no list needed the new file.

Linux and macOS: `rt_hook_function` patches direct calls on Windows only (`runtime/hle_detour.c`), so
there a direct call of the function (the boot call, if it is one) bypasses the hook and the music stays
on at boot; the menu's change still works. No workaround built.

Not yet checked in the game:
- a fight with "Game music" off: stage music silent, effects and voices play;
- switching it back on in a match brings the music back (at the options menu's volume);
- the options menu's music slider still works when on, and is silent when off;
- starting with `game_music=0` in `display.ini` is silent from the title screen (the boot call is hooked).

The original plan:

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

## Getting FFmpeg

Decision: the player decodes with FFmpeg 7.1.x (avformat-61, avcodec-61, avutil-59, swresample-5), loaded at
run time (`LoadLibrary` / `dlopen`), not linked: the game builds and runs without it, and music is simply
unavailable when the DLLs are missing. LGPL-2.1-or-later build, never `--enable-gpl` or `--enable-nonfree`.

- Headers: `third_party/ffmpeg/include` (FFmpeg 7.1.5, `third_party/ffmpeg/README.md`).
- The project's build: `tools/ffmpeg_build.sh` (mingw-w64 on Ubuntu, audio demuxers and decoders only:
  mp3, m4a with AAC or ALAC, flac, wav PCM, Ogg Vorbis and Opus, raw AAC; `--disable-x86asm`, so no nasm).
  `.github/workflows/ffmpeg-release.yml` runs it and publishes the zip, the FFmpeg source tarball and
  `SHA256SUMS` as a release.
- The build fetches it with `python tools/ffmpeg_fetch.py` into `deps/ffmpeg/bin` (pinned tag, asset and
  SHA-256 at the top of that script; skipped when the DLLs are there with the recorded hashes).
- Development fallback, used by default while the project's release is still the placeholder, or with
  `--dev`: BtbN's FFmpeg-Builds `autobuild-2026-07-31-14-10`, `ffmpeg-n7.1.5-12-g1fdbca85aa-win64-lgpl-shared-7.1.zip`
  (pinned by SHA-256). It has every codec (avcodec is 66 MB); only the four DLLs and the license are kept.

Release steps for a maintainer: run the "FFmpeg release" workflow (Actions, Run workflow) with the tag
`ffmpeg-audio-v1`; when it finishes, copy the SHA-256 of `opensmash-ffmpeg-audio-7.1.5-win64.zip` from the
release's `SHA256SUMS` into `RELEASE_SHA256` in `tools/ffmpeg_fetch.py` (replacing the placeholder), and check
that `python tools/ffmpeg_fetch.py` (no `--dev`) fetches it. The script has never run against the real
build: the first run may show a decoder or demuxer missing from the configure line.
