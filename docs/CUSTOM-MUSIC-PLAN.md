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

## Step 1: the player engine (built; no menu yet)

`runtime/music.c` and `runtime/music.h` play the player's own files through the game's own audio output, decoded
with FFmpeg. Nothing in the game changes while `music` is off, and without the FFmpeg DLLs the log says so once and
the game's music is left alone.

How it works:
- FFmpeg is loaded at run time through a table of only the functions used: `SSBU_FFMPEG_DIR`, then
  `<game folder>\deps\ffmpeg\bin`, then beside `ssbu.exe` (`dlopen` of `libavutil.so.59` and the others elsewhere).
  The four major versions (59, 5, 61, 61) are checked; others are refused.
- Three threads. The scanner lists the libraries. The player decodes ahead into a ring of 5.5 s of 48 kHz stereo
  floats: one `SwrContext` per track (any rate, channel count and sample format, downmixed with
  `rematrix_maxval` 1.0 so a 5.1 mix does not clip). The output thread calls `music_mix`, which `ipc_audio.c`'s
  `feed()` calls for every output path (WASAPI, waveOut, SDL) after the game's samples and the silence fill, so the
  music goes on through loading screens. `music_mix` takes no lock and never waits; `append()`, the rollback
  save/load and `test_tap` are untouched.
- Gapless: at the end of a track's packets the decoder and the resampler are drained into the ring and the next
  track's samples follow in the same ring. The next track is already open (its first packets read) a second into
  the current one, so a slow disk costs nothing. FFmpeg trims the encoder delay and padding it knows about (LAME tag,
  m4a edit list, opus pre-skip); nothing is trimmed here.
- No clicks: skip, previous, pick, pause, stop and every volume change ramp the gain over 5 ms where the samples
  are mixed. A skip fades out, the player drops the rest of the ring and says where its new sound starts
  (`req`/`ack`/`flush_to`), and the new track fades in. A ring that runs empty plays silence for the missing part and is counted
  (the first five are logged).
- Skip and previous go by the track being heard, not by the one the player has decoded to (the ring holds seconds of
  later tracks); previous goes back through the tracks played, and plays the same track again when there is none.

Settings (read once at start, `display.ini` like `volume`, the variables win; the setters in `music.h` change them
while playing and do not write the file, the menu does):

| key | default | variable |
|---|---|---|
| `music` | 0 | `SSBU_MUSIC` |
| `music_volume` (0..100) | 70 | `SSBU_MUSIC_VOLUME` |
| `music_shuffle` | 1 | `SSBU_MUSIC_SHUFFLE` |
| `music_source`: empty = every track in a library folder, else a folder (its subfolders too) or a playlist file | empty | `SSBU_MUSIC_SOURCE` |

Turning music on calls `audio_set_game_music(0)` (only when FFmpeg loaded), off calls `audio_set_game_music(1)`.
The music is mixed at `music_volume / 100 * master volume / 100`.

Libraries: the `music\` folder of the game (subfolders too) and the paths in the `[libraries]` section of
`config\music.ini` (beside `display.ini`, one path per line). Formats by extension: mp3, m4a, mp4, aac, flac, wav, ogg,
oga, opus. Playlists: every `.m3u` / `.m3u8` found while scanning, and the files in `[playlists]` of `music.ini`.
One entry a line, `#` lines ignored, absolute paths or paths relative to the playlist's folder, `.m3u8` as UTF-8
(a BOM is stripped), `.m3u` as UTF-8 if it is valid UTF-8, else in the Windows ANSI code page. Web addresses and
entries that do not exist or are not music are left out. A playlist's tracks may be outside the libraries: they play
(and are indexed), but are not part of "every track" or the folder list. A playlist is a source like a folder (its
own order; shuffle applies when on). Playlists are read at every scan, not indexed.

The index file, `cache\music_index` (the game's cache folder, `SSBU_DATA_ROOT` in tests): UTF-8 text, first line
`opensmash-music-index 1`, then a line a track, tab separated: path, size, modified time (seconds), length (seconds),
title, artist, album (tabs, CR and LF in tags become spaces). The scan lists the folders, reuses the entry of a file
with the same path, size and time, and opens only the others (`avformat_open_input` and `find_stream_info`, no
decoding) for length and tags (the audio stream's tags if the file has none); files that are gone are dropped. A file
that does not open keeps an entry (title = file name, length -1) so it is not opened at every start, and is left out
of the library. The index is written only when something changed, to `music_index.tmp` and then renamed over. It is read
before the scan: the player plays from the indexed list at once, and a missing file, another first line or any bad
line means a full read (it is a cache, never an error). `music_rescan()` reads every file again. Now-playing comes from
the index entry; no second open for tags.

Errors are logged, never shown, with one exception. Per scan, one line for each file or playlist entry left out
(`[music] left out: <path>: doesn't open | no audio stream | missing playlist entry in <list> | unsupported
extension (playlist entry) in <list> | web address in <list>`; a file the index already knows does not open is not
logged again) and one summary line (`[music] 1284 tracks, 3 playlists; left out: 7 files that don't open, 2 missing
playlist entries, 1 web addresses (n read, m from the index, s s)`). A track that fails while playing is skipped to
the next with `[music] skipped while playing: <path>: <why>`. The one error the user sees is an explicit pick:
`music_play_track(path, why, size)` opens the track at once and, when it cannot play, returns 0 and a sentence for the
menu (`Couldn't play "<title>": the file doesn't open` / `no audio in it`) and the music playing goes on untouched.

API for the menu (`runtime/music.h`): on/off, volume, shuffle, source, next, previous, pause, rescan,
`music_folders` (the folders that hold tracks, sorted, with their track counts, then the playlists, with a `playlist`
flag and their name), `music_now` (state, title, artist, position and length in seconds), `music_play_track`.

Tests:
- `tools/dev/musictest.c` includes `music.c` and drives it without the game: it plays the `music\` folder of `--game`,
  pulls samples as `feed()` does and writes a 48 kHz stereo WAV, with `--skip`, `--prev`, `--pause`, `--resume`,
  `--pick`, `--source` and `--now` at given times, `--slow MS` (the player sleeps after every packet),
  `--realtime`, `--rescan`.
- `tests/test_music.py` (skipped without clang-cl, the DLLs and `ffmpeg.exe`; get them with
  `python tools/ffmpeg_fetch.py --tool`, which keeps the BtbN build's `ffmpeg.exe` and its other DLLs in
  `deps/ffmpeg/bin`; the game never uses them). Run from the capitalised path:
  `cd /d C:\Users\J\Desktop\OpenSmash-Ultimate && deps\venv\Scripts\python.exe -m unittest tests.test_music -v`.
  It checks: gapless 440 Hz sine in 3 files cut at odd samples, in mp3 (LAME), m4a AAC, m4a ALAC, flac, Vorbis, Opus
  and wav: no near-silence over 0.5 ms, no click at the seams, and the same samples (within 4 of 32768) as `ffmpeg.exe`
  decoding the three files one after the other; the same checks must fail on made-up faults (1 ms gap, dropped
  sample, click); skip, previous, pause, resume and picks with no step beyond the sine and the 5 ms fade; a slow
  player (underruns counted, run completes); mono, 8 and 24 bit, 96 kHz, 5.1, cover art (mp3, m4a), Japanese
  names, a text file named `.mp3`, a truncated flac, nothing playable; the index (second run opens nothing,
  touched file read again, deleted file gone, bad first line or garbled line rebuilt, rescan, tags with tabs and
  Japanese round-trip); playlists (absolute, relative, UTF-8 with BOM and Japanese names, ANSI, missing entries,
  a web address); shuffle (no immediate repeat, every track once a round), a folder as source, previous.
- What they showed: every format is gapless and equal to the reference decode (largest difference 1 of 32768).
  The resampler's start-up at a seam (44.1 kHz files played at 48 kHz) shows as a second difference of about 6% of
  the peak, the same in the reference. ffmpeg's own AAC encoder leaves its end padding (about 20 ms of silence) in
  the file with no information to trim it, so those files have it between tracks in every decoder; the test allows what the reference has.
  Mono is played at 0.707 on both sides (swresample's mix). Faults put in by hand: 1 ms of silence between tracks and
  one sample dropped at each track's end both make the gapless test fail in all seven formats.

Not done yet: the menu (Audio tab rows for Music on/off, volume, shuffle, next/previous/pause, now playing, the
"Playing from" picker built on `music_folders`, the now-playing toast, the explicit-pick error message), writing the
new settings back to `display.ini`, a bigger-than-memory index (the whole library is held in memory), checking the
Linux and macOS builds (the file is in their lists and include paths but nothing was compiled there).

Build notes: `music.c` is in the runtime lists of `tools/recomp.py` (Windows), `tools/linux/build_linux.py` and
`tools/mac/build_mac.py`, whose patterns did not match the name; `third_party/ffmpeg/include` is added to
`cc.CFLAGS` (runtime files only) and to the Linux and macOS include lists. `GAME_CFLAGS` and
`tools_fingerprint()` (which hashes `HLE(...)` names, `HLE_WRAP` targets, hooks and the register cache, never the
runtime's compile flags) are unchanged, so no generated C is built again; the runtime's objects are (their keys include the
compile command).

## Step 1, the options considered: library choice (decided: FFmpeg)

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
