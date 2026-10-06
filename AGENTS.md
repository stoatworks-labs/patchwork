# AGENTS.md — patchwork

Read this before touching the cable (`Wall.{h,cpp}`, `linearIndex`/`tileAt`/
`travelOf` in `kCommon`), the repeat (`kRouteFragment`), the zebra
(`kPanelFragment`), the stats pass or the parameter table (`Controls.cpp`).
`CLAUDE.md` is the command reference; this is the why.

## The one idea

An LED wall is not one display. It is a few hundred small ones on a daisy chain.
A processor cuts the picture into cabinet rectangles and sends them down a cable
that snakes from cabinet to cabinet; each cabinet's receiving card takes its
rectangle by its place on the chain, applies its own calibration table, and drives
its modules one scan line at a time through an address decoder and a row of
shift-register driver chips. **Every failure an operator sees is one stage of that
chain lying**, and the plugin is that chain with a switch on every stage.

The neighbour is `pitch`, which is the same wall seen *through a camera* (moiré,
scan bands against a shutter) and already has simple dead/dim cabinets. Patchwork
owns everything upstream of the LEDs and has no camera at all.

## How a frame goes

Seven passes (`Shaders.h`), all at LED resolution (`LED Pitch` pixels per LED,
`ceil( W / p ) x ceil( H / p )`) except the last. Every LED-resolution texture is
stored **top row first** — LED row 0 is the top of the picture, as a processor
counts — and only the wall pass (in) and the display pass (out) know GL's rows run
the other way.

1. **wall** (RGBA16F): each LED is the box mean of its p x p pixels, by
   `texelFetch`, so the host's padding never enters and at `LED Pitch 1` it is a
   copy. An edge LED averages only the pixels it covers.
2. **copy** (only while `Hop Delay` or `Lag Tiles` is up): the wall into layer
   `RingWrite` of a `GL_TEXTURE_2D_ARRAY`, RGBA8, depth `min( 32, 256 MB / layer )`.
3. **route** (RGBA16F): per destination tile, in order — a crossed cable swaps it
   with its chain neighbour (positions 2k ↔ 2k+1); the processor's map repeat picks
   which chain slot's content it is sent and where along it; a cabinet hung upside
   down turns that 180°; the card's buffering picks how old it is (`round( Hop
   Delay x pos ) + lag`, clamped to the frames the ring really holds).
4. **hold** (ping-pong RGBA16F): `lost ? previous hold : route`, so the moment a card
   loses its signal its hold already holds the frame before. Invalid on the first
   frame and after a reallocation, when it takes the route outright.
5. **stats** (ping-pong RGBA32F, one texel per TILE): P = the tile's mean drive,
   `(r + g + b) / 3` of what its card puts out (16 x 16 stratified taps: exact on a
   flat field, an estimate on a picture); the heat one-pole `H += (P − H) α`,
   `α = 1 − e^(−dt/τ)` computed in double on the CPU; the hiccup timer.
6. **panel** (RGBA16F): the card's output — or its fallback (Black, Hold, Test
   Pattern, Garbage) — through the module's faults (shift, zebra, dead row, colour
   loss, dead module), the LED's (dead), the calibration table (batch, module), the
   seams, the heat droop, a stuck LED, and last the supply (dead, flicker, Dim,
   Hiccup). Clamped to 0..1.
7. **display** (to the host): each pixel's LED times the share of the pixel the
   emitter covers (a centred square of area `Fill`; exactly 1 at `Fill 1`), mixed
   with the clip; alpha `mix( src.a, 1, Mix )`.

### The cable

`Route` (Row Snake, Column Snake, Rows, Columns) from `Start Corner`, cut into
chains of `Tiles Per Port` (0 = one chain). `linearIndex` is the order the cable
visits the tiles with every port end to end; `port = linear / PerPort`, `pos` the
remainder. `travelOf` is the direction the cable runs through a tile in screen
terms. The GLSL and `Wall.cpp` are the same functions; `--chain-law` checks the C++
(bijection, inverse, a snake steps to a neighbour along its travel direction or
turns at a line end) over 4032 layouts, and `--chain` measures the GPU's order out
of the picture (below), so neither copy is taken on trust.

### The repeat

`Repeat Tiles` N, the window starting at chain position `s0 = ⌊From·n⌋` (clamped so
the window fits), the region the window plus `⌊Reach·(n − s0 − N) + ½⌋` tiles. With
the motion offset m in whole LEDs (`0 ≤ m < N·L`, L the LEDs along the cable through
one tile) and e an LED's index along the cable through its own tile:

    R = (pos − s0)·L + e − m + N·L      source tile = s0 + (R div L) mod N,   e' = R mod L

and **e' becomes a column with the DESTINATION's direction of travel**. This is the
"map offset" model: every card is always sent a (wrapped) rectangle of the source,
never mirrored. At rest every tile shows its window tile the right way round; in
motion each tile's content translates along its own run of the cable, so on a snake
alternate rows run opposite ways. The alternative — shifting the at-rest picture
along the cable like a conveyor ("model B") — agrees inside every row but makes a
duplicated, mirrored joint at every corner of the snake, because the at-rest
picture is not continuous there. A map fault is a map fault; model A was chosen,
and `--repeat` states it independently of the formula: tile p shows window tile
`s0 + ((p − s0 − j) mod N)` translated f LEDs along its own direction (m = jL + f),
the overhang taken from the window tile before.

The phase is one double, integrated every frame (`RepeatMotion::Advance`), so a
change of `Repeat Speed` never jumps the content; Step takes whole tiles of it,
Scroll whole LEDs, Ping-Pong a triangle out to N tiles and back, Random a new
whole-tile offset each time the phase crosses a tile. The shader receives m
already reduced — twice, once for a full chain's window and once for the last,
shorter chain's (`RepeatShiftFull`/`RepeatShiftLast`), because `m mod (N·L)` is not
`(m mod (N'·L)) mod (N·L)` for a smaller window.

### Zebra

An address line stuck at v: a row whose address `r mod S` has bit k ≠ v is never
selected (black); a row with bit k = v is selected in its own slot and in its
partner's, `r XOR 2^k`, so it shows the SUM of both rows' data. **Light is moved,
not removed** — `--zebra` holds every module's mean to the source's within two
half-float ULPs. Stripes of 2^k rows. `Zebra Width` asks for a bit; a bit the scan
ratio has no line for is clamped (`min( k, log2 S − 1 )`), and 1/1 static drive has
no address lines and no zebra at all. `Floating` redraws v every frame.

### The time

pitch's (readout's) host-clock unit vote, unchanged. `dt` is bounded at 0.25 s and is
0 on the first frame. Nothing absolute crosses into GLSL: each random process —
dropouts, flicker, the intermittent connector — takes an integer slot base and a
float fraction reduced in double (`wall::SlotOf`), and adds a per-tile (per-port)
phase in the shader, so slots do not flip in lockstep. The test pattern's colour is
`floor( t ) & 3`; garbage and floating lines use a frame counter.

## Decisions taken without asking

- **The name** is patchwork (`SW Patchwork`, `PW01`, free across the fleet and
  `~/dev` on 2026-10-06): a decalibrated wall is what the industry calls a patchwork
  or checkerboard, and the wall is literally sewn from tiles.
- **Tile size is in LEDs, with `LED Pitch` pixels per LED** (pitch's convention,
  a cabinet's real spec). Defaults 60 x 60 LEDs at two pixels: 16 x 9 cabinets at
  1080p. The wall is anchored top-left and partial cabinets at the right and bottom
  are real cabinets on the chain.
- **Every calibration gain is at most 1.** A decalibrated wall is really brighter
  than a calibrated one (calibration dims every LED to the weakest), but an 8-bit
  output cannot show it, and gains above 1 would clip on white and hide the
  patchwork exactly where it shows most. Seams are the exception: an overlap
  brightens, and clips on white.
- **Module colour belongs to Module Spread**, not Colour Spread. Found by
  `--batches` (see the traps): with module tint on Colour Spread, Batches could never
  give K looks.
- **Faults are static per seed, except the four that are processes** (Dropouts,
  Flicker, Intermittent, the zebra's Floating mode) and the two with state (heat,
  hiccup). A seeded wall fails the same cabinets on every render.
- **The supply's restart is 0.27 s**, fixed, chosen not to be a whole number of
  frames at any common rate (16.2 at 60, 13.5 at 50, 6.48 at 24) so a float sum of
  frame times never lands on the boundary. Not a control.
- **The heat droop ratios** red : green : blue = 1 : 0.3 : 0.15 are typical
  AlInGaP-against-InGaN datasheet ratios, not one part's. `Heat` sets red's.
- **The wall is opaque**: alpha `mix( src.a, 1, Mix )`. A dead cabinet is black,
  not a hole. Resolume's demo clips are DXV with alpha (toner's trap), and an LED
  wall shows transparent content as black.
- **The ring is RGBA8**, to fit 32 frames in 256 MB: a delayed tile is quantised to
  8 bits where an undelayed one is half float. Depth `min( 32, 256 MB / layer )`, so
  at 4K and `LED Pitch 1` it holds 7 frames and delays clamp there.
- **Hold and the heat start again on a reallocation** (a new raster, a new LED
  Pitch, a new tile count): what a card last received at another raster means
  nothing at this one. `--resize` holds the plugin to "exactly a fresh instance".
- **Per-chain repeat.** Each port repeats its own window; with one chain (the
  default) that is the wall.
- **No audio input** for v0.1.0. Resolume can drive any control from audio or BPM
  itself; a beat-locked Step is the obvious addition (Open questions).
- **StoatworksAbout.h and ATTRIBUTIONS.md are generated** by the backend's
  `sync-about.py` and `sync-attributions.py` since registration (2026-10-06). The guide URL
  added a "User guide" button inside the About block, which is last, so no control moved.

## The traps

Ordered by how much time they cost.

**The chain check sorted by arrival before port, and was vacuous.** The first
`--chain` sorted (arrival, port) and compared consecutive entries for adjacency,
skipping pairs from different ports — but with several ports every consecutive pair
IS from a different port, so the multi-port runs compared nothing and printed "0
non-adjacent steps". Rows-with-ports reading 0 (it must be at least 2) gave it away.
Sorted by port, then arrival. A check that compares nothing passes; read the
numbers, not the ok.

**Apple's GPU truncates float to half.** The identity error was 0.000486 against a
half-float half-ULP of 0.000244 — a conversion rounding toward zero loses a whole
ULP. The bound is one half-float ULP at the top of the range (2^-11) plus float32
slack, and says why. On a GPU that rounds, the check has twice the room it needs.

**Colour Spread leaked into the modules.** The first panel tinted every module by
Colour Spread, so `--batches` found 60 distinct colours where 3 batches were asked
for. The physics was wrong, not the test: a batch is a calibration table, and a
module's own deviation is Module Spread's.

**A card with no horizontal structure cannot be seen to scroll.** The sweep first
found Route, Repeat Motion and Repeat Speed dead: the repeat's window was the
card's top-left, a sky that varies only vertically, and a horizontal scroll of it
is the same picture. The sweep puts the window mid-card (`Repeat From 0.45`).
Intermittent at 0.5 was dead for the same reason in time: one frame is compared,
and at 0.5 it may be one where the connector is open; it is swept at 1.

**A sampler2DArray on an empty unit is "unloadable".** Apple's driver logs it every
run even though no fragment reads the ring when it is off; the spec calls it
undefined. A 1 x 1 x 1 array is bound there instead.

**`mutate.sh` splits on `|`** (conway's trap, walked into again): the first zebra
mutant was XOR → OR. It is now `1 << k` → `2 << k`, and the table says so.

**Members named like namespaces.** `wall` the buffer hid `wall::` the namespace and
`route` the local hid `Route` the enum, inside `ProcessOpenGL`; the buffers are
`wallPass`, `routePass`, … now.

**Cue sheets ramp standard controls.** The demo's first take ramped Zebra from frame
0 to its key at 8.5 s; every change needs a hold key the frame before.

**Apple's software renderer takes ~9 s per chain configuration** at 320x180 (290 s
for the first `--chain`). The software pass runs two opposite corners of the four,
prints the skip, and the cut comes after 22 frames instead of 40.

## Would this hold on another rasteriser, at another raster?

Every check runs at 1280x720 and 320x180 on this Mac's GPU, and at 320x180 on
Apple's software renderer (`tools/verify.sh`).

- `--identity`: one half-float ULP at the top of the range (2^-11) + 1e-6. Holds
  whether the conversion rounds or truncates; pitch 3 does not divide either raster,
  so the edge LED's partial box is covered.
- `--tiles`, `--batches`: exact equality and inequality of stored values; tile
  sizes are derived from the raster and deliberately do not divide it.
- `--zebra`: two half-float ULPs (a doubled row is a sum of two stored values).
  The ramp stays under 0.45 so no sum clips.
- `--chain`: integer frame counts and tile adjacency; 5 x 4 tiles at any raster, so
  20 hops sit inside any ring depth this check can meet (32 at both rasters).
- `--repeat`: 8-bit equality of a hash card's bytes, which survive the half-float
  round trip exactly; tile sizes derived from the raster.
- `--hold`: 8-bit equality; which tiles are lost is measured from a twin rig, not
  re-derived, so a different rasteriser cannot desynchronise model and plugin.
- `--heat`: 1e-4 on an RGBA32F state (600 float32 updates); the panel's droop to a
  half-float ULP on top.
- `--psu`: two half-float ULPs for Dim (a quotient of stored values); Hiccup is a
  frame count, at 60 and 50 fps, with the restart chosen off every frame boundary.
- `--leds`: 4 sigma of a binomial, so the bound shrinks honestly with the raster
  (0.0009 at 720p, 0.0036 at 180p).
- `--resize`: byte equality between two instances in ONE context (cross-context
  float equality is GPU-only — honeydew's lesson).

## A check that cannot fail is not a check

`pwtest --negative` runs 15 wrong models, one at a time, each through a hook the
shipped shaders carry at zero (`shaders::Hook`) or a CPU switch, and requires the
check to fail: the wall's box a pixel right (`--identity`), calibration read a tile
one LED off (`--tiles`), a batch id ignoring Batches (`--batches`), a doubled row
with only its own data (`--zebra`), a break losing the tiles before it and a hop
delay one hop long (`--chain`), the repeat turned back with the source's direction —
model B's mirror (`--repeat`), a hold that follows the route (`--hold`), forward
Euler for the heat (`--heat`), Dim by the square root (`--psu`), dead LEDs at twice
the rate (`--leds`), the hold surviving a reallocation (`--resize`), the snake's
claims made of the raster routes (`--chain-law`), phase from speed x elapsed
(`--motion-law`), and every cue ramping (`--cues`). All 15 fail.

`tools/mutate.sh` changes one character of the shipped code and requires the named
check to fail, on a copy of the tree built from scratch:

| mutant | caught by |
| --- | --- |
| GLSL `r ^ ( 1 << k )` → `r ^ ( 2 << k )`: the zebra's partner one bit too high | `--zebra` (ramp, worst row 0.038) |
| GLSL `dir > 0` → `dir < 0`: the repeat's along-index against the cable | `--repeat` (167,006 bytes wrong at rest) |
| GLSL `ln.pos >= at` → `ln.pos <= at`: a break loses the tiles before it | `--chain` (7 live tiles downstream of a lost one) |
| GLSL `( P - prev.r )` → `( P - prev.g )`: heat relaxes towards the hiccup timer | `--heat` (H off by 3.12) |
| GLSL `sum / float( n )` → `sum * float( n )`: the box sum not divided | `--identity` (LED Pitch 2 off by 0.507) |
| GLSL `PsuLimit / state.b` → `PsuLimit * state.b`: Dim multiplies | `--psu` (0.32) |
| C++ `( y & 1 ) == 0` → `!= 0`: the row snake starts backwards | `--chain-law` |

7 of 7 caught (2026-10-06). The GLSL mutants prove the harness drives the shaders
the plugin ships, not a copy.

## Shape of the code

    source/
      Patchwork.{h,cpp}   the FFGL plugin: clock, buffers, the ring, the seven passes
      Shaders.{h,cpp}     every pass, assembled kVersion + kCommon (+ kSignal) + pass
      Controls.{h,cpp}    the parameter table (name, group, kind, default) and units
      Wall.{h,cpp}        the cable and the repeat's motion, in integers, no GL
      Hash.h              PCG, the same integer arithmetic as the GLSL
      PassBuffer.{h,cpp}  pitch's FFGLFBO with the colour-texture leak fixed
      GLState.h           conway's capture/restore of the host's GL state
      Diag.{h,cpp}        the log file, nothing else
      PluginEntry.cpp     the bundle's own TU and build stamp
      StoatworksAbout*.h  the About block (StoatworksAbout.h generated by sync-about.py)
    tools/
      pwtest/main.cpp     the harness: render, --pipe/--film/--script, every check
      verify.sh           everything; mutate.sh, sweep.py, glslc.sh
    demo/                 the browser demo: plugin.js (the port), shaders.js (generated), vendor/ (the kit)

## What is genuinely verified, and what is assumed

**Measured** (numbers in README Status): a perfect wall is the clip to a half-float
ULP and to the byte; LEDs are box means; calibration is piecewise constant on tiles
and modules with every change on a boundary; K batches are K looks; a stuck address
line blacks one parity of rows and doubles the other with the module's mean kept;
the cable's order, measured as a cut arriving one hop per frame, is the documented
route for every route, corner and port size, and a snake's consecutive tiles are
adjacent; a break loses a suffix of each chain; the repeat is the map-offset model,
byte for byte, for every motion; rows of a snake scroll opposite ways at the same
speed; a lost card holds the last frame it received; heat is the one-pole to 1e-7
heating and cooling, with red, green and blue drooping 1 : 0.3 : 0.15; Dim caps a
tile's mean at the limit; Hiccup blinks with period ceil(R/dt) + 1 at 60 and 50 fps;
LED fault rates are binomial at their rates; a resize is a fresh start; the host's
GL state comes back; every control moves the picture.

**Assumed, not measured**: that real walls fail this way in these proportions (the
mechanisms are real; the rates and the look of the test pattern and the garbage are
not a vendor's); the heat droop ratios and the 0.27 s restart; that 16 x 16 taps are
a good enough tile mean on real pictures; that the host's clock behaves as pitch's
vote expects. **Never loaded into Resolume on macOS.** On Windows the fleet's Arena gate
passed 9 of 9 on Arena 7.27.1 (llvmpipe, 2026-10-06): 45 controls moved a still carrier,
48 under a precondition, and the four that act on motion or over seconds (Heat Time, Hop
Delay, Lag Tiles, Lag Frames) were inconclusive there, as the expectation's notes say.
No OpenFX port. The browser demo is a port, below.

## The browser demo (2026-10-06)

`demo/` is <https://patchwork-demo.stoatworks-labs.com>, built to the fleet's
`resolume-demo` kit rules (`~/Projects/resolume/specs/DEMO-BRIEF.md`) by a sub-agent of
the release session. What a reader of it must know:

- **The shaders are the plugin's**: all ten pieces plus kVersion, spliced by
  `demo/tools/check_shaders.py --write` into `demo/shaders.js`, AND the order the
  plugin joins them in, read out of `Assemble()`'s switch, the `Pass` enum and
  InitGL's vertex line into `ASSEMBLY`; the page builds every program from that
  table. The script compares every piece character for character, refuses a piece
  of Shaders.cpp it does not copy, compares each stage's assembly, and finally
  requires the file to be exactly what `--write` produces; `tools/verify.sh` runs
  it. Negative-controlled once, 8 of 8 caught: the zebra's partner one bit high in
  shaders.js, the repeat against the cable in Shaders.cpp only, stats assembled
  without kSignal, panel's pieces reordered in ASSEMBLY, a new piece in
  Shaders.cpp, an extra export typed into shaders.js, a Pass with no case, and a
  different vertex assembly. **All seven passes compile in WebGL2 as spliced**
  (the kit's `port()` adds the ES precision lines, `sampler2DArray`'s included)
  now that the panel counts the scan bits by a loop instead of `findMSB`.
- **The CPU half is a port that only a reader checks**: Controls.cpp's table (as
  `TABLE`, in ParamId order) and every conversion, `IntegerOf`, `OptionIndex`,
  `ScanGroups`, `ThresholdU32`; `wall::MakeLayout` (only what ProcessOpenGL uses:
  the cable's order is the GLSL's); `RepeatMotion` (`Advance` per frame,
  `OffsetLeds` with Random's PCG from Hash.h via `Math.imul`), the full and last
  chains' shifts; `wall::SlotOf` (`whole >>> 0` is the uint32 reduction, the float
  carry kept); the ring (`TEXTURE_2D_ARRAY` RGBA8, depth `clamp(256 MB / layer, 2,
  32)`, write and fill, delays off if `getError` says the allocation failed, a 1 x 1
  x 1 array on the unit when there is none); the hold and state ping-pongs and
  their validity flags; `1 - exp(-dt/tau)` in double; the 0.27 s restart; the
  frame counter; the test pattern's phase. Uniforms are set by the type the linked
  program declares (`getActiveUniform`), since JavaScript has one number type; a
  name no pass declares is shown in the status line rather than left as a dead
  control.
- **Compared once with the plugin, and it agreed byte for byte.** A scratch script
  gated the page's animation frames headlessly (Chrome, ANGLE on Metal, this Mac's
  M4 Max): the first frame at t = 0, then Pause and one Step (1/60 s) per frame,
  the canvas read back with `preserveDrawingBuffer` forced on; the clip frames came
  from the same page at Mix 0 (which returns its input exactly) and went through
  `pwtest --pipe` with the same `--set`s, at 640 x 360, 40 frames each. Nine
  settings identical in every byte: the defaults; Repeat Tiles 3 scrolling at 1 and
  at 2.88 cabinets/s; Random on a column snake from the bottom right; Ping-Pong at
  -8/s over chains of 11 with a last chain of 4; Chain Break 1 under Hold with
  Dropouts 0.2 in 0.13 s slots; a break under Garbage with Floating zebra over
  chains of 9; Hop Delay 1 frame/hop with Lag; Hiccup at limit 0.19 with Heat 1,
  tau 1 s. The tenth,
  27 controls moved at once (Mix 0.8, Fill 0.4), differed in 2 of 9.2 million
  pixels by one level. The identity preset returns the page's own clip byte for
  byte. The comparer can fail: the plugin on Fault Seed 2 differs in 98% of pixels,
  one frame out of step in 30%, and seven one-line mutants of the PORT were each
  caught (restart 0.30 s, Scroll rounding instead of flooring, RingFilled one high,
  Random's salt one bit off, HoldValid always 1, the frame counter counting two,
  the last chain's shift 7 LEDs off). One survived and is recorded rather than
  explained away: the last chain's shift left unreduced (m, not m mod N'L) differs
  from the right one only where R goes negative, in a few LED columns at the start
  of the last chain on frames where m >= N'L, and on this GPU it made no
  difference there. On SwiftShader the same comparison is within 2 levels
  (another rasteriser's half-float rounding). Nothing repeats any of this; a change
  to the C++ half needs the port changed by hand.
- **Decisions taken without asking.** The clock is the page's seconds (the unit
  vote not ported); dt is the plugin's (bounded at 0.25 s, 0 backwards), though the
  kit already caps a frame at 0.1 s. **A redraw with the clock stopped (a control
  moved under Pause) does not write the ring or count a frame**, readout's page's
  choice, so a paused ripple and a paused Garbage hold still; the hold, the state
  and everything else run as a host frame with no time passing, which the
  comparison above shows changes nothing they hold. The ten integer controls are
  dropdowns of every value in their range (the page's value is the index, the
  plugin's integer the minimum plus it). The presets are the page's own, after the
  user guide's "Start here"; the plugin ships none. The clips, moving ones first:
  scene, spot, grid, bars, ramp, detail, alpha.
- **Differences, all said on the page:** the page's frames are the display's, so
  Hop Delay and Lag count browser frames (a 120 Hz display ripples twice as fast in
  seconds); Restart replays the slots from 0 while heat, hiccup, hold, ring and
  phase carry on; the paused-redraw rule; integer dropdowns and what a shared link
  carries; a 265 MB ring at LED Pitch 1 and 1080p may be refused by a browser (the
  plugin's own fallback, delays off, said in the status line); still clips show no
  delay or Hold; no About block; no audio in the plugin to miss.
- **Seen, not a fault:** headless Chrome on SwiftShader logs "GPU stall due to
  ReadPixels" four times; a clear-only WebGL2 page logs the same four, so it is the
  headless compositor, not this page (on Metal there are none). The live page logs
  one console error that is not the page's: the zone's injected
  `/cdn-cgi/challenge-platform` script blocked by `script-src 'self'`, as on every
  `*-demo` host. Locally there are no errors.

## Open design questions

- **A beat-locked repeat.** Step on the host's beat (Resolume's transport, as
  macroblock reads it) would be the VJ's first ask.
- **Redundant cabling.** Real walls loop a backup cable back from the last cabinet,
  so a single break loses nothing; a `Loop Backup` switch would make a break lose
  only the cabinets between two breaks.
- **Receiving cards that drive more than one cabinet**, and cards whose map is
  shifted by a fraction of a cabinet (a wrong X/Y offset in the config file).
- **Gamma before the drive.** The wall drives with the clip's code values; a real
  processor applies a gamma and a real low-grey looks worse (pitch's honest limit
  too).
- **Content-driven heat under motion.** The stats taps are 16 x 16 per tile; on
  fast-moving bright content the heat reads an estimate.
- Whether the repeat should be per chain (now) or across the whole wall when there
  are several ports.
