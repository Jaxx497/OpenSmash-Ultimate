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

- [ ] Pause menu elements
- [ ] Training mode elements
- [ ] Timer placement
- [ ] Minimap
- [ ] Triple arrows (the blue arrows) follow the bubbles out, in all three views
- [ ] Wide view: keep drawing the fighter instead of the bubble while inside the picture
- [ ] Menus and loading screens: fill the side areas

## Bugs to reproduce

- [ ] **Training mode camera stretches** in a wider picture (owner's test).
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
- [ ] **Wide view at 12% of its framing** (found 2026-11-12): the wide view re-runs the game's framing
      for the picture's shape and eases toward it, but the renderer's copy for other cameras (two more
      each frame) clears that offset, so it sits at 12% of its goal (-11.8 of -98.4 at Final
      Destination's edge). The glossary's wide view is the game's own camera with the side areas added,
      which is closer to no offset at all. Owner: keep, full framing, or no offset?
- [ ] **Zoomed view tighter than 16:9?** If the zoomed picture can be narrower (in the fight's space) than
      the 16:9 view, a fighter can leave the picture without being off-screen by the game's rule: no
      bubble, not seen. The owner records a replay for this edge case; check it in the zoomed view.

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
