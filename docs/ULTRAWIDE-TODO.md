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
      (not while the window or its edge is being dragged: at the end of the drag).
- [x] **Windowed: the window follows the shape**: same height, new width, around the same centre; shrunk to
      fit the monitor if wider.
- [x] **The online hold ends at the main menu**: the match block's online melee mode (Online 14, Battle
      Arena 20) counts only while the game talks to other players (15 s), so the menus go back to the
      picture shape once they are gone. Not yet seen in a real online session.
- [x] **Every display setting persists**: `tests/test_renderer_option.py` reads back every value the Display
      page writes. Found: the launcher before upstream #318 wrote its old copy of the settings back when the
      game closed, undoing the in-game menu's changes; fixed by the current launcher (not yet installed here).

## Experimental tab (the maintainer agreed to a new tab)

Rule for every row: once turned on it stays on; any change takes effect at the next start
([ADR 0001](adr/0001-visual-features-never-change-the-fight.md)).

- [x] **Ultrawide online** (display.ini `experimental_ultrawide_online`; the shape still never changes inside an online fight): the picture shape kept in online matches. Disclaimer: "Experimental. A mistake
      in this feature changes the fight for you AND your opponent, and can make the match go out of sync."
- [x] **Open log folder** (Windows): Explorer on `build\` with `play.log` selected (rollback's desync reports too).
- [x] **`play.prev.log`**: the previous session's log kept at each start (the launcher, `Program.cs`; reaches players with the next launcher build).
- [x] **The log names the shape and view** at each online match's start, and the rollback desync report
      carries that line ([ADR 0002](adr/0002-online-held-at-16-9.md)).

## HUD (see ULTRAWIDE.md, "HUD decisions")

- [x] Pause menu elements: panels to the side their layout puts them against (the game's own layouts; see ULTRAWIDE.md, `pane_place`)
- [x] Training mode elements (the + menu too)
- [x] Timer placement: the "Timer position" setting (right, center, left, hidden), two-digit minutes checked
- [x] Radar (the minimap square): seen in a test (2026-11-12, a fighter off the bottom): the picture's top-right corner, under the timer
- [ ] Battle Camera frame (camera controls' frame overlays) marks the whole picture
- [ ] A shape changed mid-fight reaches a HUD panel that never changes only at the next fight (making the layouts
      place themselves again was left out: see `pane_place`); fine unless players switch shapes mid-fight
- [ ] Triple arrows (the blue arrows) follow the bubbles out, in all three views
- [ ] Wide view: keep drawing the fighter instead of the bubble while inside the picture
- [x] Menus and loading screens: black side areas for the first PR (owner); the blurred fill after it

## Bugs to reproduce

- [x] **Training mode camera stretches** in a wider picture (owner's test): Training's camera is
      orthographic (`+0x150` = 1), its projection built from a 16:9 box (`+0x140`), not from the width
      the fight's camera takes, so the 3D was drawn 2x wide (grid cells 61 x 31 px; 16:9: square). Its
      drawing projection is widened after the copy now (cells square, side areas drawn, tags on their
      fighters). Wide and zoomed look the same there: an orthographic camera has no distance to zoom by.
      Scripted route: title A, RIGHT, UP, A (Games & More), DOWN, LEFT, A (Training), wait, A, SUP x3, A,
      PLUS, A (`build/aspect_test/auto/train_*.bat`).
- [x] **A menu's selection frame strays into the side area** (owner, 2026-11-12, 32:9 zoomed or hybrid,
      Vault > Replays > Replay Data): the magnifying bubble mover took the menu's replay thumbnails and
      selection frame for a bubble (the same vertex programs as a bubble's portrait and ring; the left tile
      sits where a left bubble would) and moved the frame to the picture's edge. Bubbles move only in a
      fight now (the game's ready-go flag, on in replays too).
- [x] **Pause camera jumps** (owner, 2026-11-12, 32:9 zoomed, worst near the stage's edge): a pause stops
      the fighters' frame line, and a quarter second later the views took that for the fight's end and
      dropped their offset, so the picture jumped to the game's own camera (and the zoomed view's shadow
      camera started afresh on resume). A pause is the fight now (the ready-go flag stays on); the pause
      shot starts from the picture's view, and the offset fades as the player moves the pause camera
      away. The pause menu box at the 16:9 view's right edge is the pause menu's HUD decision.
- [x] **Wide view at 12% of its framing** (found 2026-11-12): the wide view re-ran the game's framing
      for the picture's shape, but the renderer's copies for other cameras cleared that offset every
      frame, so it sat at 12% of it (100% zooms in until the fighters fill the picture, like the zoomed
      view). Owner: the game's own framing, no shift; matches the glossary's wide view.
- [x] **Zoomed view tighter than 16:9?** Checked 2026-11-12 on the owner's Ganondorf vs Pichu replay
      (Bridge of Eldin, 32:9 zoomed), every frame to GAME!: the picture is always shorter than the 16:9
      view (zooming in is the view), and it stops at the stage's edge, so a fighter standing on the ledge
      has its centre up to 1.3 units past the picture's edge (half the body shown); once, for 2 frames,
      a fighter high up was 1.4 units above the picture's top. No fighter is hidden without a bubble.

- [x] **Zoomed pause zooms out** (owner, 2026-11-12, Bridge of Eldin): the renderer's copies for other
      cameras cleared the views' zoom each frame, and the zoomed view's shadow camera, which set it again
      every fight frame, stops in a pause, so the zoom fell to 1 the moment the game paused. The other
      cameras' copies leave the views alone now.

## Renderers

- [ ] **Vulkan port**: the squeeze, 2D clip, bubble shift and shape-change surface rebuild in shared code
      both back ends call; Vulkan proven by the replay fight hash (renderer-independent) and by captures of
      the same frames from both back ends compared. Then the lock becomes "Direct3D 11 or Vulkan".
- [ ] **Linux** (Direct3D 11 through DXVK): if making `host.c`'s Windows-only parts portable is small;
      else after the first PR.

## Clean-up

- [x] Research switches: kept as `SSBU_DEBUG_CAMERA_HUNT`, `SSBU_DEBUG_CAM_TRACE` (`_OBJ`), `SSBU_DEBUG_FRAMING_DUMP`,
      `SSBU_DEBUG_REFERENCE_VIEW`, `SSBU_DEBUG_PANES`, F9; `SSBU_TEST_ASPECT_CYCLE`, `SSBU_TEST_VIEW_CYCLE`; removed: the cull
      hunt, `RCAM_WATCH`, `ASPECT_HYBRID`, `ASPECT_SCREEN`, `NO_2D_CLAMP`, `SHADOW_LOG`, `WIDE_FRAMING_STORED`,
      `FRAMING_SHADOW` (the wide view's old framing re-run).
- [x] `docs/PLAYING.md`: the settings text (aspect, view, timer) matches what ships.

## After the first PR (not blocking)

- Menus and loading screens: a blurred fill of the side areas instead of black.

- Metal.
- Online unlocked by default, by [ADR 0002](adr/0002-online-held-at-16-9.md)'s evidence.
