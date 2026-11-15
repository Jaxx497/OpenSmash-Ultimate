# Custom music: how the game's own music is silenced

Notes from reverse-engineering the game's sound volumes (2026-11-15), for a mod that plays the player's
own music through the runtime's audio output. Addresses are for game version 13.0.5 (main at
`0x7100000000`).

## Result

Muting the game's `Sound_Option_Bgm` mixer group silences the music and leaves sound effects and voices.
Either of the first two calls does it:

| Call | What it does |
| :--- | :--- |
| `0x71037375f0 (w0 = group, s0 = volume)` | Sets a group's volume. The volume is linear (the options menu sends 0.0 to about 1.0); the function turns it into decibels and posts a `volu` command to the sound thread. |
| `0x7103737a20 (w0 = group, w1 = flag)` | Mutes (flag 1) or unmutes (0) a group: posts a `mute` command. |
| `0x7103737820 (w0 = group, ...)` | Posts a `paus` command (not tested). |

The group numbers index a table of names at `0x710523cec8`:

| Group | Name | |
| :--- | :--- | :--- |
| 0 | `nus3_GamePause` | |
| 1 | `nus3_BgmMute` | muting it changed nothing (tested) |
| 2 | `Sound_Option_Se` | the options menu's sound effects slider |
| 3 | `Sound_Option_Voice` | the options menu's voices slider |
| 4 | `Sound_Option_Bgm` | the options menu's music slider |
| 5 | `Stage_SE` | |
| 6 | `Volume_Offset_Group` | |
| 7 | `BGM_Stage` | |
| 8 | `Fighter_SE` | |

Callers also pass 9 and 10; those names were not read.

## How it was tested

A native mod (`tools/dev/music_probe.cpp`, built with `ssbu-recomp/fighter_sdk/tools/build_mod.py`) hooks the
three functions, logs their calls, and with `SSBU_MUSIC_PROBE` changes the game's first group 4 volume call
at boot. Each run was headless (`SSBU_HEADLESS=1`, `SSBU_VOLUME=0`) with `SSBU_AUDIO_HASH` logging every
buffer's peak, and `SSBU_INPUT` skipping the opening movie, entering the main menu and moving the cursor
every 4 seconds.

| Run | Menu music | Cursor and menu sounds |
| :--- | :--- | :--- |
| baseline | plays | play |
| `vol0`: group 4 volume 0 | silent | play |
| `mute4`: mute group 4 | silent | play |
| `mute1`: mute group 1 | plays | play |

One call at boot held from the title screen into the main menu.

Not tested: a fight (stage music, victory fanfare), and whether anything un-mutes group 4 later. The
opening movie's sound is not in the music group: it played in every run.

## Where the game calls these

- At boot the saved options are applied: volume 0.700 for groups 4, 2 and 3 on this save (from
  `0x7101976d40`).
- The options menu registers the callbacks `update_edited_bgm_volume` (`0x7101d53de0`),
  `update_edited_se_volume` (`0x7101d53f10`) and `update_edited_voice_volume` (`0x7101d54020`). Each
  calls the volume function with group 4, 2 or 3 and stores the slider's value (0 to 100) in the
  settings block at `0x71052c5180 + index * 16 + 0x4314c` (music), `+ 0x43150` (effects), `+ 0x43154` (voices).
- The game itself mutes groups 2, 3 and 4 together from `0x710233a980`, and groups 0, 8 and 9 from fight code.

## The other music volume

Stages and menus fade the music through a second path that a mod does not need, but which explains
volume changes during play: `0x71023eea60 (x0 = manager, w1 = slot, w2 = fade in ms, s0 = volume)`, with
the manager pointer at `0x7105329f38`. It keeps four factors at `[manager + 0x28] + 0x1bc + slot * 4`,
multiplies them and applies the product to every music player. Slots 0 (menus), 1 and 2 (stages, about
120 call sites) are used; nothing sets slot 3 through this function.
`app::kozukatasight::set_bgm_volume(float, uint)` (`0x7101647180`) is a wrapper for slot 1.

## For the mod

The simplest form is a hook on `0x71037375f0` that sends 0 for group 4 while custom music is on (and
remembers the value the game asked for), plus one call of the function when the setting changes. That
also covers the player moving the music slider in the options menu.

## Seen on the way

`SSBU_SKIP_MOVIES=1` with a scripted A press at the title stopped the game with a guest trap in
ARCropolis (`arcropolis+0x128e68`). Without `SSBU_SKIP_MOVIES` the same run was fine.
