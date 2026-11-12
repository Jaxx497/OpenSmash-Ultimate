# Ultrawide

Playing on a screen wider than 16:9 without stretching anything, while every fight stays exactly the
fight a 16:9 screen would have. The working notes and the map of the game's code are in
[ULTRAWIDE.md](ULTRAWIDE.md).

## Language

### The picture

**Picture shape**:
The width over the height of what the player sees, e.g. 32:9 or 43:18; from 16:9 up to 32:9, the widest supported. It changes only between fights, and is 16:9 for online matches.
_Avoid_: aspect, resolution, ratio

**16:9 view**:
What a 16:9 screen would show at the same moment: the middle of the picture, framed by the game.
_Avoid_: original view, game view, core

**Side areas**:
The parts of the picture left and right of the 16:9 view.
_Avoid_: margins, extra width, bars

**Stretched**:
Drawn wider than its proportions, as when a 16:9 image is shown on a wider screen. A defect.

### Views

**View**:
How the camera frames a fight for the picture shape. The player picks one; the fight is the same in every view.
_Avoid_: camera mode, camera style

**Wide view**:
The 16:9 view with the side areas added at the same zoom: everything the fight could involve is in the picture, even past the stage.
_Avoid_: full view, full

**Zoomed view**:
The game's own framing done for the picture shape: it zooms in until the fighters fill the wider picture, keeping it on the stage.
_Avoid_: liked view, shadow view

**Hybrid view**:
The wide view, slid along the stage and zoomed in only as far as needed to keep the picture on the stage; never less than the 16:9 view.
_Avoid_: stage view, stage style

**Reference view**:
The zoomed view as first made, by telling the game itself the picture shape: it changes fights, so it is kept for comparison only and never used online.
_Avoid_: liked mode, bad mode

### The stage

**Stage range**:
The area of a stage the 16:9 camera keeps its view inside; nothing past it was meant to be seen.
_Avoid_: camera range, camera bounds, play area

**Abyss**:
Whatever lies past the stage range: unfinished scenery, empty space.
_Avoid_: void, out of bounds

**Blast zone**:
The boundary past which a fighter is KO'd.
_Avoid_: dead zone, kill zone

### Fights

**Fight-identical**:
A fight that is the same, frame for frame, as on a 16:9 screen, shown by its fight hash up to GAME!.
_Avoid_: online-safe, deterministic, in sync

**Off-screen**:
Outside the 16:9 view, as the game's rules see it: a fighter there is shown in a magnifying bubble and takes off-screen damage, whatever the picture shows.
_Avoid_: out of view, hidden

**Magnifying bubble**:
The circle the game draws for an off-screen fighter, showing that fighter and where they are.
_Avoid_: magnifying glass, offscreen bubble, shield (the shield is a different bubble)

### The HUD

**HUD**:
Everything the game draws over the fight: damage meters, timer, tags, magnifying bubbles. It stays in the 16:9 view, unstretched.
_Avoid_: UI, overlay (the overlay is the runtime's own frame-rate display)

**Tag**:
The marker over a fighter naming its player: P1, P2, CPU.
_Avoid_: player tag, label, nameplate
