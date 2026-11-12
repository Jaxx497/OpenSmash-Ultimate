# Ultrawide: left before the first PR

Everything that must be done before branch `ultrawide` becomes a pull request. When this list is
empty, the file is deleted and the PR opened. Words as in [ULTRAWIDE-CONTEXT.md](ULTRAWIDE-CONTEXT.md);
how things work and what was learned in [ULTRAWIDE.md](ULTRAWIDE.md); HUD rulings in its
"HUD decisions" table.

Order agreed with the owner (2026-11-11): HUD work first, then the Vulkan port.

## Menu and settings

- [x] **"16:9 (original)"**: the 16:9 preset's label; help text "The game as made for 16:9. The View setting has no effect."
- [x] **View greyed at 16:9**: shown, with "only with a picture wider than 16:9".
- [x] **Presets snap to the monitor** (43:18 dropped from the menu: on a 21:9 monitor it was the same as 21:9): 21:9 and 43:18 take the monitor's exact shape when it is within
      2.22-2.45 (2560x1080, 3440x1440, 3840x1600), 32:9 within 3.38-3.73; otherwise the preset's own
      shape. No bars in fullscreen inside those ranges; bars outside them are expected.
- [x] **Window size and render scale change without a restart**: at once offline; in an online match,
      after it (as the picture shape). Window size: any renderer on Windows (Linux and macOS: next start).
      Render scale: Direct3D 11 (Vulkan and Metal: next start); only the targets whose size changes are
      made again.
- [x] **Window size options fit the picture shape and the monitor**: the menu offers heights 720 to 2160
      whose width (height x shape) fits the monitor's work area; display.ini keeps width and height.
- [x] **Render scale auto follows the shape**: chosen again whenever the window's picture changes size
      (ponytail: also while a window is dragged, every frame of the drag).
- [x] **Windowed: the window follows the shape**: same height, new width, around the same centre; shrunk to
      fit the monitor if wider.
- [ ] **The online hold ends at the main menu** (now: menus can stay 16:9 after an online match until the
      next offline match).
- [ ] **Every display setting persists**: a check, row by row, that each one survives a restart.

## Experimental tab (the maintainer agreed to a new tab)

Rule for every row: once turned on it stays on; any change takes effect at the next start
([ADR 0001](adr/0001-visual-features-never-change-the-fight.md)).

- [ ] **Ultrawide online**: the picture shape kept in online matches. Disclaimer: "Experimental. A mistake
      in this feature changes the fight for you AND your opponent, and can make the match go out of sync."
- [ ] **Open log folder**: Explorer on `build\` with `play.log` selected (rollback's desync reports too).
- [ ] **`play.prev.log`**: the previous session's log kept at each start.
- [ ] **The log names the shape and view** at each online match's start, and the rollback desync report
      carries that line ([ADR 0002](adr/0002-online-held-at-16-9.md)).

## HUD (see ULTRAWIDE.md, "HUD decisions")

- [ ] Pause menu elements
- [ ] Training mode elements
- [ ] Timer placement
- [ ] Minimap
- [ ] Triple arrows (the blue arrows) follow the bubbles out, in all three views
- [ ] Wide view: keep drawing the fighter instead of the bubble while inside the picture
- [ ] Menus and loading screens: fill the side areas

## Bugs to reproduce

- [ ] **Training mode camera stretches** in a wider picture (owner's test).

## Renderers

- [ ] **Vulkan port**: the squeeze, 2D clip, bubble shift and shape-change surface rebuild in shared code
      both back ends call; Vulkan proven by the replay fight hash (renderer-independent) and by captures of
      the same frames from both back ends compared. Then the lock becomes "Direct3D 11 or Vulkan".
- [ ] **Linux** (Direct3D 11 through DXVK): if making `host.c`'s Windows-only parts portable is small;
      else after the first PR.

## Clean-up

- [ ] Research switches: keep as `SSBU_DEBUG_*` the camera hunt, the F9 picker, the camera trace, the
      framing dump and the reference view; `SSBU_TEST_ASPECT_CYCLE`, `SSBU_TEST_VIEW_CYCLE`; remove the
      rest (cull hunt if it costs anything, `RCAM_WATCH`, `ASPECT_HYBRID`, `ASPECT_SCREEN`, `NO_2D_CLAMP`,
      `WIDE_STYLE=shadow`, `SHADOW_LOG`, `WIDE_FRAMING_STORED`). Docs and `docs/TESTING-RUNS.md` follow.
- [ ] `docs/PLAYING.md`: the final settings text.

## After the first PR (not blocking)

- Metal.
- Online unlocked by default, by [ADR 0002](adr/0002-online-held-at-16-9.md)'s evidence.
