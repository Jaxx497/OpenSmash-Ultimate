# Ultrawide: what is known, what was changed

Working notes for the ultrawide mode (branch `ultrawide`). Read this before touching the camera, the
HUD or the renderer's squeeze for it. Addresses are game version 13.0.5, `main` loaded at `0x7100000000`
(`main+0x1234` = `0x7100001234`). Every claim says how it was found; **measured** means a run showed it.

Goals agreed with the owner: the fight must stay exactly what 16:9 computes (online play); the HUD
stays in the middle 16:9, unstretched; the P1/CPU tags and the magnifying glasses sit on their fighters;
the camera should feel like 16:9's (centred, pulling in, not showing much past the stage).

## Switches (environment, Direct3D 11 only)

| Variable | What it does | State |
| :--- | :--- | :--- |
| `SSBU_ASPECT=W:H` | The picture's shape (`43:18` = 3440x1440, `32:9`). Unset: 16:9, nothing changes. | kept |
| `SSBU_ASPECT_NATIVE=1` | The game draws its 3D wide itself (render camera widened); the renderer squeezes only 2D. | kept, the current mode |
| `SSBU_ASPECT_HYBRID=1` | Test: render camera width only, renderer squeezes everything. Squashed fights. | to remove |
| `SSBU_ASPECT_SCREEN=1` | Test: the render camera's second width (+0x170) too. No effect seen. | to remove |
| `SSBU_NO_2D_CLAMP=1` | The 2D clip to the middle 16:9 off. Menus and the CPU-level pop-up break, yellow bars show. | to remove |
| `SSBU_CAMERA_HUNT`, `SSBU_CULL_HUNT`, `SSBU_CAM_DUMP` | Research: camera matrix and frustum searches, the render camera dumped every 5 s. | research, to remove |
| F9 (key) | Research: steps through the kinds of squeezed 2D draw, leaving one unsqueezed. | research, to remove |
| `SSBU_GFX_LOG_CB=all` | F4's draw log dumps every constant buffer bank in full. | small, may keep |

## What the runtime changes (by file)

- `runtime/display.h`, `runtime/gpu_display.c`: `display_aspect()` (from `SSBU_ASPECT`); `display_fit`
  fits the picture at that shape; a windowed frame keeps it while resized.
- `runtime/gpu_d3d11.c`:
  - `scaled_size`: a 16:9 render target is made `aspect / (16/9)` times wider than the render scale
    makes it (other shapes, e.g. square shadow maps, are not); it returns that factor, kept as
    `Surface.wide`.
  - The squeeze (in `d3d11_draw`, after the stages are bound): on a widened target a draw's clip-space
    x is divided by the widening (`params.fix[0]`, which every vertex program already multiplies by).
    Not squeezed: full-screen passes (they read a widened target and test no depth, or test depth with
    ALWAYS: tone mapping, bloom, the half-size depth copy). In `SSBU_ASPECT_NATIVE` only 2D is squeezed
    (no depth test), the game's 3D being wide already.
  - 2D clip: a squeezed 2D draw is scissored to the middle 16:9, and the game's own scissor (in its
    pixels) is squeezed towards the middle with it.
  - Research: `cam_hunt_cb` (camera matrices in vertex constant buffers), F9's picker, the `aspect:`
    line in F4's draw log.
- `runtime/gpu_window.c`: F9 passed to the renderer like F1-F6.
- `runtime/host.c`:
  - `aspect_native()`; `wide_camera` (frame end): the render camera's width `+0x164` set to
    `1080 * aspect` each frame (the game then builds a wide projection and culls with it).
  - `hud_call` and its 12 wrappers: while the HUD's world-to-screen functions run, the render camera's
    projection x scale (`+0x290`) is multiplied by the widening and its inverse (`+0x2d0`) divided, i.e.
    16:9's, so what they place lands on its fighter after the squeeze.
  - Research: the hunts and the camera dump (behind their switches).

## The game: render camera

The camera the picture is drawn with. Found: `f_7103747820` (scene set-up, calls `tanf`) reads it and
copies its matrices to the scene globals; dumped live with `SSBU_CAM_DUMP` (**measured**).

Address: `*(*(main+0x5337ce8) + 0x490)` is a manager `m`; the camera is `*(m + 0xe0)` when
`*(m + 0xd8)` is set, else the default at `main+0x5334060`.

| Offset | Value at 16:9 | Meaning |
| :--- | :--- | :--- |
| +0x150 | 0 | projection kind (0 perspective) |
| +0x15c | 30, 41 | field of view, degrees (41: the entry; 30: the fight) |
| +0x160 | 0.5236 | field of view, radians |
| +0x164 | 1920 | width: **the projection is built from this** (widening it alone widened the projection, measured) |
| +0x168 | 1080 | height |
| +0x16c | 1.77778 | width / height, computed as 1920 x (1/1080) - not bit-equal to 16/9, which is why searches for 16/9 missed it |
| +0x170, +0x174 | 1920, 1080 | a second size; widening it changed nothing seen |
| +0x178, +0x17c | 1/1920, 1/1080 | reciprocals of the second size |
| +0x1d0 | 4x4 | view, row-vector (`p * V`), translation row at +0x200 |
| +0x210 | 4x4 | identity seen |
| +0x250 | 4x4 | inverse view; camera position at +0x280 |
| +0x290 | 4x4 | projection: +0x290 x scale (cot(fov/2) / aspect), +0x2a4 y scale (cot(fov/2)) |
| +0x2d0 | 4x4 | inverse projection |
| +0x310 | 1920 1080 1/1920 1/1080 | another size |

Widening the width at +0x164 each frame fixed the culling of floors and background pieces at the
picture's sides (**measured**) and does not change the fight (**measured**, below).

Copies on the way to the GPU: the scene constant buffer (`cb3`, field of view 41) is written by
`f_7103553980` (a loop that flushes CPU-side buffers to GPU ones with memcpy); per-draw `cb8` copies
(field of view 30) by `f_710355a980` / `f_710355ad30` and `main+0xa5c80`. Found with `SSBU_CAMERA_HUNT`
(page write-watch), **measured**.

## The game: game camera (`app::`) - the one that frames the fight

The camera logic: where the camera goes, what is off screen. Its state feeds the fight.

- Camera manager: `*(*(main+0x52b8f00))`, call it `M` (`app::camera::*` and `sv_camera_manager::*` read it):
  - `M+0xc20` .. `+0xc2c`: the stage's camera range, left, right, top, bottom (`sv_camera_manager::camera_range`,
    `main+0x2282ea0`; left/right order from the framing code's min/max).
  - `M+0xc30` .. `+0xc3c`: the dead area, the blast zones (`app::camera::get_dead_area`, `main+0x15cb9e0`).
  - `M+0xeb0`: the visible range's centre; `M+0xf38` x `M+0xf68`: its half height; `M+0xf58`: its
    width / height. `app::camera::get_camera_range` (`main+0x15cb960`) returns centre +- (half height x
    aspect, half height). Read from code, not yet dumped live.
- The framing: `f_71004fe4f0(camera, out, ..., s0 = aspect)` (calls `tanf`) fits the fighters' camera
  ranges, clamped to the stage's (`+0x224..+0x230` against `M+0xc20..`). Its caller copies the aspect
  from `*(camera + 0x8) + 0x128` to `camera + 0x134` every update; `app::item::is_clip_in_camera`
  (`main+0x15c46a0`) reads `+0x134..+0x140`.
- `main+0x4472c5c`: the only 16:9 float in `main` (rodata). Read by the framing code's other branch and
  the main menu's 3D camera, not by the fight's projection (**measured**: patching it widened the main
  menu's panels only).

**Widening this camera's aspect (both places) changes the fight** (**measured**: a scripted 32:9 fight
with it differs from 16:9 at frame 578, in damage - fighters past the old 16:9 edge took the
magnifying glass's off-screen damage). With it widened, though, the camera framed the wide picture
the way the owner wanted: it pulled in, stayed inside the stage, and fighters past the old 16:9 edge
were drawn (the game hides a fighter in the magnifying glass) with the glass at the real edge.

### How the 16:9 camera behaves (measured, 2026-11-11)

A 4-player replay (frames 10-4170; camera range x -170..170, y -50..130; blast zones x +-230) watched at
16:9 with `SSBU_CAM_TRACE=10` and `SSBU_FIGHT_HASH` (`build\aspect_test\replay169.bat`):

- The game camera is a 16:9 rectangle on the fighters' plane: centre `M+0xeb0`, half height
  `M+0xf38 x M+0xf68`, half width = half height x 16/9.
- The render camera follows it: same x, a little above it (pitched ~5 degrees down), at the distance
  that shows that half height (half height = distance x tan(fov / 2)); the field of view itself moves too
  (26-30 degrees seen).
- The rectangle stays inside the stage's camera range: its left edge stopped at -169, -166, -167 against
  the range's -170 (frames 2110, 3010, 3310). That is the 16:9 feel: it slides with the fighters and stops
  at the range.
- Off screen = outside that rectangle: at frame 2110 two fighters at x -197 and -211 (past the range,
  inside the blast zone).
- At 32:9 the same height is twice as wide: frame 2110's rectangle would run -261..109, 91 past the range
  and 31 past the blast zone. Zoomed out (half height ~75 and more) a 32:9 view is wider than the whole
  camera range, so no sideways slide can keep it inside: it has to zoom in, as the game's own framing did
  when its aspect was widened.

The framing camera object (x0 of `f_71004fe4f0`; dumped with `SSBU_FRAMING_DUMP`, a hook that only
reads; same replay):

| Offset | Meaning (measured against the trace and the fighters' positions) |
| :--- | :--- |
| +0x70 (x, y) | where the camera looks now |
| +0x90 (x, y, z) | where it is now (z: its distance) |
| +0x80, +0xa0 | the same two as its goal: it eases from now to goal |
| +0xb0 | 30, 30, 0.6 (field of view, ...) |
| +0x1f8 (left, right, top, bottom) | the goal rectangle, already 16:9 (fighters' box plus margins, stretched to 16:9) |
| +0x208 (left, right, top, bottom) | the visible rectangle now (16:9, inside the camera range) |
| +0x224..+0x230 | the slot the framing clamps against the range: +-FLT_MAX here (empty) |

The fighters' own box is not kept: the game works it out and stores only the 16:9 result. A wide
framing needs it (spread sideways: zoom in; spread up and down: cannot).

### The liked mode against the visual-only framing (measured 2026-11-11)

`SSBU_GAME_FRAMING_WIDE=1` (with `SSBU_ASPECT_NATIVE`) is the mode the owner liked: the game's own camera
framing told the wide aspect. It changes the fight (kept for reference: `build\aspect_test\liked.bat`,
offline only). A live 1v1 on Bridge of Eldin in it, against the owner's 16:9 replay of the same match-up
and the visual-only framing (`SSBU_WIDE_FRAMING`), view half height by the fighters' spread:

| spread | 16:9 | liked | visual-only |
| :--- | :--- | :--- | :--- |
| 0-49 | 42 | 41 | 42 |
| 150-199 | 64 | 61 | 59 |
| 300+ | 95 | 48 | 84 |

The liked mode keeps a close shot when the fighters spread out (the wide picture covers them), and
slides off their middle to stay inside the stage's range; past the range at spread 300+: 0. The
visual-only framing zooms out as 16:9 does: 259 past. So the zoom is not decided by `f_71004fe4f0`.

The camera update (`f_71004fdc10`) then calls `f_71004fec80` twice: with s0 = the stored aspect
(+0x134), x2 = +0x70 (the camera's now), x3 = +0x110; and with s0 = main+0x4472c5c (16:9), x2 = +0xc0,
x3 = +0x118. These move the camera's state; the zoom is likely there. Setting the stored aspect only during the
second `f_71004fe4f0` call did not change the picture; the hash difference seen then (frame 2474) was
the results screen's random victory pose, not the fight. Compare hash logs up to the "GAME!" frame (the
fighters jump to x 1100 / 0 for the results).

### Decisions (owner, 2026-11-11)

- Two camera styles, both kept as options: **full** (the visual-only framing as it is: everything the
  fight could need is in view, past the stage if need be) and **stage** (the liked mode's look: the view
  clamped to the stage's camera range at the full width, zoom fitted to the wide picture), the latter
  to be made fight-identical with a shadow camera (the game's own camera update run on a copy).
- The magnifying glass stays where the 16:9 rules put it: it marks a fighter nearing the blast zone,
  which is when it shows anyway. Drawing the fighter instead is for later.
- Stages whose floor runs to the blast zones (Bridge of Eldin) show the oddities most; competitive
  rulesets ban them.

## Driving the game without a person

`build\aspect_test\auto\*.bat` (made by `mk.py` there): `SSBU_SEQ` walks the menus - title (A at tick
600), main menu (RIGHT is Vault, A), Vault (RIGHT is Replays, A), Replay Data (A), the newest replay (A),
Play (A) - with screenshots every few seconds (`SSBU_CAPTURE`, `sheet.py` makes a contact sheet). Each
run takes a copy of the save (`SSBU_DATA_ROOT`). A saved replay plays the same fight every time:
`framing169` (scripted) and a watch by hand hashed the same over 16536 lines.

The same replay at 32:9 (`replay329.bat`, the native mode) hashed the same as at 16:9 over 4039 fight
frames (16153 lines): the second proof that the mode does not change a fight, this time on real play.

## The game: HUD

Drawn by a few vertex programs, all with depth testing off, on the final 1920x1080 target. Found from an
F4 draw log of a fight (`SSBU_GFX_LOG_CB=all`), **measured**:

- Placed elements, `vs_e0758c3ac3507774` (text `ps_340268462afc028d`, images `ps_547eb1f52eda5794`):
  vertex bank 3, first 0x40 bytes, is the element's transform with the screen's: x row `(2/1920, 0, 0, x)`,
  y row `(0, -2/1080, 0, y)`; the corners come in the element's pixels (a storage buffer).
- Shapes, `vs_1669b7d183575986` and 7 others: bank 3 is the screen's mapping (no position) and, at +0x40,
  the element's transform (x row `(scale, 0, 0, x in layout pixels from the middle)`); the vertex buffer
  is a unit square.
- The P1/CPU tags and the magnifying glasses use the same programs as the rest of the HUD: no program
  tells them apart. Their positions come from the render camera's projection through the HUD's
  world-to-screen code. Narrowing each element around its own anchor (instead of squeezing all to the
  middle) put tags right but broke widgets made of pieces (clock and damage digits spread apart) and
  pulled a full-screen element's edges (the yellow bars) into view.
- The 12 functions given 16:9's projection (`main+0x1341670`, `+0x134c690`, `+0x135ccb0`, `+0x13955d0`,
  `+0x13a3f50`, `+0x13a53d0`, `+0x13a67c0`, `+0x13adb10`, `+0x13c8460`, `+0x13ca580`, `+0x13e29d0`,
  `+0x142aed0`): the ones that load the render camera and read +0x290, in that part of `main`. Tags land
  on their fighters with all 12 (**measured**); which of them are needed is not known.
- `sv_camera_manager::world_to_screen` (`main+0x22832c0` -> `f_7101341280`): layout pixels, centre
  (960, 540); in a fight only ever returned the centre (**measured**). Not what places the tags.
- `kozukatasight::screen_width` (`main+0x1646f00`) reads a global at `main+0x744701c` (1920 when 0).
- Yellow bars: an element just past the 16:9 edges; the 2D clip hides it.

## The fight stays the same: how to check

`build\aspect_test\verify\run169.bat` and `run329b.bat` (and `run329.bat`): the same scripted fight
(`SSBU_SEQ`, `SSBU_DETERMINISTIC`, `SSBU_FIXED_SEED`, `SSBU_FIGHT_HASH`; Mario against a level 1 CPU
Donkey Kong, `stage=end`), each with a fresh copy of the save. The menus are walked with the keyboard
(`SSBU_SEQ_KEYBOARD=1`); the fight plays itself and the run ends after 30 s. Then:

```
python tools/dev/fight_hash_diff.py build/aspect_test/verify/run169.fh.txt build/aspect_test/verify/run329b.fh.txt
```

Results (2026-11-10): 16:9 vs 32:9 native with the 16:9 framing: the same over 4036 lines. 16:9 vs 32:9
with the game camera widened: first difference frame 578, damage. A stage not named in
`SSBU_DEBUG_MATCH` comes from the menus, so two runs can differ in stage: name it.

## Tried and dropped

- Patching `main+0x4472c5c`: only the main menu's camera and the framing's other branch.
- Searching memory for 16:9 floats, for perspective matrices of 16:9, for frustum planes (6 or 4,
  either layout): the fight's 16:9 is computed each frame; culling keeps no planes.
- Telling the game the screen is wider (`vi`): ignored, the game's sizes are its own.
- A vertex-shader squeeze (perspective vertices only): right picture, but every vertex program's cache
  is new (18,222 programs, ~1 h of compiling); short effects were skipped while compiling.
- `f_710364b620`'s visible rectangle (+0x950 = height x 16 / 9): 0 x 0 in fights.
- Panning the render camera to keep the stage's range in view, with the rest dimmed (2026-11-11):
  no dimming seen; reverted.

## To do (agreed with the owner)

1. **"Stage" camera style, fight-identical**: a shadow camera - the game's own camera update
   (`f_71004fdc10`, `f_71004fec80`) run on a copy of the camera with the wide aspect; only the picture
   follows it. Checked by replay hash.
2. **Magnifying glass at the wide edge**: the game clamps the bubble to its 16:9 screen, and the HUD is
   squeezed into the middle 16:9, so the bubble sits on the old 16:9 edge. Clamp it to the picture's
   edge instead and let its draws through the 2D clip box. When it shows stays the 16:9 rule's (a
   useful cue near the blast zone).
3. **Keep drawing the fighter in the bubble**: the game hides the fighter's model when it bubbles it;
   keep drawing it while it is inside the wide picture (visual only), bubble shown too.
4. **Settings**: a `display.ini` key for the aspect (`auto` from the monitor, `16:9`, `21:9`, `32:9`, ...)
   and for the camera style (`full`, `stage`), and both in the in-game settings menu.
5. **Menus and loading screens**: fill the sides (black, or a blur of the middle) where nothing draws.
6. **HUD layout options**: damage meters and the minimap placed for wide screens.
7. **Vulkan and Metal**: the same changes in the other renderers (Direct3D 11 only now).
8. **Clean-up before shipping**: remove the research switches and hooks (hunts, dumps, F9 picker,
   `SSBU_ASPECT_HYBRID`, `SSBU_ASPECT_SCREEN`, `SSBU_NO_2D_CLAMP`); keep `SSBU_GAME_FRAMING_WIDE` as the
   reference while it is useful; online stays 16:9 until a fight-hash test of each camera style passes.

## Open

- Camera feel: with the 16:9 framing the wide picture shows past the stage's range. The widened
  framing looked right but changes the fight. To look at next: how the 16:9 camera behaves (dump the
  game camera through a fight at 16:9), and whether the framing can be computed twice - 16:9's for the
  fight, the wide one for the render camera only.
- Tags and glasses with a visual-only wide framing (they follow the render camera).
- A `display.ini` setting and the settings menu; Vulkan and Metal; removing the research code.
