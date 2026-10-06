# patchwork

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The behaviour is not asserted
> but measured: an offline harness drives the real plugin class in a headless GL
> context and checks each claim — a wall with nothing wrong is the clip to the byte,
> a stuck address line blacks half the rows and doubles the other half while the
> module's mean light stays put, a cut in the clip arrives one hop per frame in the
> exact order of the cable for every route, corner and port size, a broken cable loses
> a suffix of its chain, the repeated tiles are the right way round byte for byte in
> every motion, a lost card holds the last frame it received, the heat follows its
> one-pole to 1e-7 with red drooping first, and a tripped supply blinks on the frame
> it should — with fifteen negative controls and seven one-character mutants that
> prove the checks can fail. It has **never been loaded into Resolume on macOS**.
> On Windows, a build of v0.1.0 loads, registers and renders in Resolume Arena 7.27.1 with all 54 host controls as declared, on software rendering. See [Status](#status).

A faulty LED wall, as an FFGL effect for [Resolume](https://resolume.com) Arena and
Avenue.

![A kaleidoscope clip on a failing LED wall: cabinets a shade off in brightness and colour, dark seams, zebra-striped modules, dead cabinets, three bright tiles from the centre repeated along the cable's snake, and the bottom rows downstream of a cable break showing the receiving cards' red test pattern](docs/hero.png)

<sub>One frame, rendered by `pwtest`, the offline harness — not captured from
Resolume. Resolume's bundled `IntoTheGlow_02` on the defaults (60 x 60 LED cabinets at
two pixels an LED), with three cabinets from the middle of the cable repeated along the
next 14, more zebra, colour spread and seams, and the cable broken near its end (Lost
Signal: Test Pattern).</sub>

## The one idea

An LED wall is not one display. It is a few hundred small ones on a daisy chain.

A processor cuts the picture into cabinet-sized rectangles and sends them down a
cable that snakes from cabinet to cabinet. Each cabinet's receiving card takes its
rectangle by its place on the chain, applies its own calibration table, and drives
its modules one scan line at a time through an address decoder and a row of
shift-register driver chips.

**Every failure an operator sees is one stage of that chain lying.** The plugin is
the chain, with every stage able to fail:

| stage | the lie | what you see |
| --- | --- | --- |
| processor map | the map repeats every N cabinets | N tiles' content repeated along the cable — and moving along it |
| cable | broken after cabinet b | every cabinet downstream black, frozen, a test pattern or garbage |
| cable | crossed; a cabinet hung upside down | two tiles swapped; a tile turned 180° |
| receiving card | buffers late | tiles lag; a cut in the clip ripples down the chain |
| receiving card | loses its signal | its own fallback: black, the last frame, a test pattern, garbage |
| calibration | tables from another batch | the patchwork: cabinets and modules a shade off |
| mechanics | gaps between cabinets | dark (or bright) seams |
| supply | over current | bright cabinets dim, or trip and blink |
| LEDs | hot | red droops first, so a long-held bright cabinet drifts cyan |
| address decoder | one line stuck | **zebra stripes** |
| row driver | one address dead | pinstripes every S rows |
| column driver | data chain broken at a chip | one colour gone from a 16-column boundary on |
| shift clock | a missed clock | a module's content slipped sideways |
| LED | dead; stuck | black dots; one colour stuck full on |

## What falls out

None of these is drawn. Each is one stage misbehaving on the picture it is given:

- **The patchwork.** `Batches` K draws K calibration tables and deals them to the
  cabinets; each module adds its own. A full-white clip shows it worst — which is
  why every gain is at most 1, so nothing clips away the evidence.
- **The repeat, along a virtual line.** The line is the cable. `Repeat Tiles` N
  takes N cabinets' content from `Repeat From` along the chain and repeats it over
  `Repeat Reach` of the rest. `Repeat Motion` moves it: **Step** a whole cabinet at a
  time, **Scroll** an LED at a time, **Ping-Pong**, **Random**. Because the line is a
  snake, alternate rows run opposite ways and the content turns the corner at the end
  of every row. It is a map fault, so a card always shows a rectangle of the source —
  never mirrored.
- **Breaks follow the cable.** A break loses everything downstream of it, which on a
  snake is a ragged-edged block from the middle of a row onwards. `Tiles Per Port`
  splits the wall into several chains, each with its own fate; `Intermittent` makes a
  bad connector flicker in and out.
- **A cut ripples.** `Hop Delay` buffers a fraction of a frame per hop, so a hard
  cut in the clip sweeps down the snake; `Lag Tiles` makes a few cards late at random.
- **Zebra stripes are light moved, not removed.** A stuck address line leaves half
  the rows unselected and selects the other half twice, so the lit rows show the sum
  of two rows' data: the module's mean is unchanged. `Zebra Width` picks the line
  (stripes of 1–16 rows); a 1/1 static-drive module has no address lines and cannot
  zebra; `Floating` flickers.
- **The supply listens to the content.** `PSU Limit` caps each cabinet's mean drive:
  `Dim` sags a bright cabinet down to the cap, `Hiccup` trips it off for 0.27 s and
  lets it blink back — so a white flash knocks out exactly the bright cabinets.
- **Heat is slow and red.** With `Heat` up, each cabinet's temperature follows its own
  drive with time constant `Heat Time`; red loses light about three times faster than
  green and six times faster than blue, so a bright logo held for a minute leaves a
  cyan ghost of its cabinets after it has gone.

`Fill 1` with every fault, spread and process at zero is the clip, byte for byte.

## Controls

| Group | |
| --- | --- |
| **Wall** | LED Pitch (1–16 pixels per LED), Fill (the lit fraction of each LED), Tile W, Tile H (LEDs per cabinet), Modules X, Modules Y (per cabinet), Scan (1/1 … 1/32). |
| **Chain** | Route (Row Snake, Column Snake, Rows, Columns), Start Corner, Tiles Per Port (0 = one chain). |
| **Calibration** | Tile Spread, Module Spread, Colour Spread, Batches (0 = every cabinet its own), Seams, Heat, Heat Time (1–600 s). |
| **Repeat** | Repeat Tiles (0 = off), Repeat From, Repeat Reach, Repeat Motion (Hold, Step, Scroll, Ping-Pong, Random), Repeat Speed (±8 cabinets/s). |
| **Mapping** | Swapped Tiles, Flipped Tiles, Hop Delay (0–2 frames per hop), Lag Tiles, Lag Frames. |
| **Signal** | Chain Break, Intermittent, Dropouts, Dropout Time (0.05–5 s), Lost Signal (Black, Hold, Test Pattern, Garbage). |
| **Power** | Dead Tiles, Flicker Tiles, Flicker Rate (0.5–30 Hz), PSU Limit, PSU Mode (Dim, Hiccup). |
| **Modules** | Dead Modules, Zebra, Zebra Width (Random, 1–16 rows), Zebra Mode (Stuck, Floating), Dead Rows, Colour Loss, Shifted. |
| **LEDs** | Dead LEDs, Stuck LEDs. |
| **Output** | Fault Seed, Mix. |

The rates are probabilities per cabinet, module, chain or LED. A seed fails the same
cabinets every time; only Dropouts, Flicker, Intermittent and Floating zebra are
processes in time, and the heat and the hiccup have memory. The defaults are a rental
wall after a hard season — every cabinet a little off, a couple dead, a few modules
striped, pinstriped, missing a colour or slipped, the odd cabinet dropping out. The
repeat is off by default: it is a choice, not a fault you would leave in.

## Status

**v0.1.0, and honestly early — 6 October 2026.**

### Measured offline, on macOS

`tools/verify.sh` passes on this machine (Apple Silicon, macOS 26) against a fresh
universal Release build, running every check at **two rasters**, 1280×720 and 320×180,
and again on **Apple's software renderer** at 320×180. What it establishes:

| check | result |
| --- | --- |
| `--identity` | no faults: worst \|out − clip\| **0.000486**, under one half-float ULP (0.000489), **0 of 3,686,400 bytes** differ at 720p; at LED Pitch 2 and 3 every pixel is its LED's box mean to **0.000488** (pitch 3 divides neither raster, so the edge LEDs' partial boxes are covered) |
| `--tiles` | Tile Spread and Module Spread are constant inside every cabinet and module (**0 changes**) and change across **100%** of boundaries (12,880 and 34,960 pairs at 720p), with modules at uneven `floor( m T / M )` boundaries |
| `--batches` | Batches 1, 3, 5 give exactly **1, 3, 5** cabinet colours over 60 cabinets; Batches 0 gives **60** |
| `--zebra` | every stuck bit (stripes of 1, 2, 4, 8 rows, at 1/16 and 1/4 scan): dark rows black and lit rows the sum of a row and its partner to **0.000586**, the module mean kept to **0.000195**; a bit past the scan ratio clamps; 1/1 shows no stripes |
| `--chain` | for all 4 routes × 4 corners × 1 or 4 chains, a cut reaches chain position *i* on frame cut + *i* exactly (**0 of 20** cabinets off); a snake's measured order has **0** non-adjacent steps; a break loses a suffix of every chain (**0** live cabinets downstream of a lost one, 3 seeds) |
| `--repeat` | Hold, Step, Scroll (both signs), Ping-Pong and Random, on row and column snakes and rasters from every corner, with uneven chains: **0 bytes wrong** against the map-offset model; on a row snake scrolling at 1 cabinet/s, row 0 moves **+13** LEDs and row 1 **−13** in 6 frames |
| `--hold` | over 90 frames each a different level, **1,730** lost cabinet-frames (runs up to 30 frames), **0** showing anything but the last frame the card received — which cabinets are lost is measured from a twin rig under Black, not re-derived |
| `--heat` | heating on white: \|H − (1 − e^(−t/τ))\| **1.0e-7** over 241 frames; cooling from H 0.8647: **1.2e-7**; red, green, blue droop 1 : 0.3 : 0.15 to a half-float ULP |
| `--psu` | Dim at 0.5: every cabinet over it lands on 0.5 to **0.000195**, those under it untouched; Hiccup on at frames 17, 35, 53, 71 at 60 fps (every **18** = ⌈0.27 × 60⌉ + 1) and every **15** at 50 fps |
| `--leds` | Dead and Stuck at 5%: **0.0500** and **0.0503** of 921,600 LEDs (4σ = 0.0009) |
| `--resize` | 1280×720 for 30 frames then 640×360 for 3, against a fresh 640×360: **0 bytes** differ (a break under Hold, the ring live) |
| `--state` | the host's viewport, vertex array, buffer, program, unit, framebuffer, blend, scissor, clear colour, colour mask and ten texture units come back unchanged |
| `--chain-law` | 4,032 layouts: every route a bijection with its inverse, every snake step to a neighbour along its travel direction or round a line end |
| `--motion-law` | the repeat's phase is integrated: a speed change mid-run never jumps it by more than one LED |
| `--negative` | **15** deliberately wrong models, **15** caught |
| mutation | **7** one-character mutants of the shipped GLSL and `Wall.cpp`, **7** caught (the zebra's partner one bit high, the repeat against the cable, a break losing the tiles before it, the heat chasing the wrong channel, the LED box not divided, Dim multiplying, the snake starting backwards) |
| `tools/sweep.py` | all **48** controls measurably change the picture |
| shaders | all 8 compile through `glslc`, not merely through Apple's driver |
| the bundle | universal (`x86_64 arm64`), exports `plugMain`, ad-hoc signs; `oxbow` reports `SW Patchwork` / `PW01` / `effect` and renders 120 frames through `plugMain` |

Render cost, GPU time by `GL_TIME_ELAPSED`, median of 60 frames after a warm-up, on a
GPU shared with other work, at the defaults (LED Pitch 2)
**0.27 ms** at 720p, **0.46 ms** at 1080p, **1.40 ms** at 4K (8% of a 60 fps frame); at LED
Pitch 1 with the repeat, the delay ring and the hold all live, **0.43**, **0.92** and **3.63 ms**.
The ring of past frames is RGBA8 at LED resolution, up to 32 frames in 256 MB, and is
allocated only while Hop Delay or Lag Tiles is up. macOS figures only.

### Not established

It has **never been loaded into Resolume on macOS**. Everything above was compiled,
rendered and measured offline against the real plugin class in a headless CGL context,
plus an `oxbow` load. What the host's clock does to the processes over a long session,
and how the faults read to someone who runs real walls, are untested.

**Windows, in Resolume Arena 7.27.1** (win-lab, Mesa llvmpipe, no GPU, 2026-10-06): a
build of this source loads from Extra Effects, registers as `SW Patchwork` / `PW01` /
effect, all 54 host controls match the declaration in name, order, type, range and
default, it renders, and Arena's log stays clean: 9 of 9 of the fleet gate's checks.
45 controls moved the picture (48 under a precondition); Heat Time, Hop Delay, Lag Tiles
and Lag Frames were inconclusive, because the gate compares single frames of a still
picture, and a delay or a time constant of a still picture is the same picture;
`tools/sweep.py` proves all four on a moving card. Software rendering says nothing about
a GPU or about speed.
The mechanisms are real; the rates, the test pattern and the garbage are not any
vendor's. No OpenFX port (not in scope for 0.1.0).

The [user guide](docs/USER-GUIDE.md) covers every control, what it does and why.

## Browser demo

**<https://patchwork-demo.stoatworks-labs.com/>** — every control, with the plugin's own
names, groups and defaults. The seven passes are the plugin's own GLSL, spliced unedited
into `demo/shaders.js` with the order `Assemble()` joins them in
(`demo/tools/check_shaders.py`, run by `tools/verify.sh`, fails on a changed character or
a changed order). The CPU half — the controls, the wall's layout, the repeat's motion, the
time slots, the ring of past frames, the hold and the heat — is a JavaScript port in
`demo/plugin.js` that **nothing checks but a reader**; compared once with `pwtest --pipe`
on the same frames, nine settings of ten agreed byte for byte over 40 frames and the
tenth in all but 2 pixels of 9.2 million, by one level. It runs on the page's generated clips
at the display's frame rate, so Hop Delay counts the browser's frames, not a
composition's; the integer controls are dropdowns. It is a demo, not the plugin, and the
page says what it does not reproduce.

## Build

Needs CMake 3.15+, a C++17 compiler, and the FFGL SDK submodule.

```bash
git clone --recursive https://github.com/stoatworks-labs/patchwork
cd patchwork
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build     # into ~/Documents/Resolume Arena/Extra Effects
```

macOS builds are universal (Apple Silicon + Intel) by default; add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster dev build. Windows needs GLEW via vcpkg.

## Building and testing

The offline harness renders the real plugin class headlessly, on a synthetic 60 fps
clock:

```bash
./build/pwtest --out /tmp/wall.png --size 1920x1080      # the test card
./build/pwtest --list                                    # every control, kind and default
./build/pwtest --identity --tiles --zebra                # each claim, measured
./build/pwtest --chain --repeat --hold --heat --psu
./build/pwtest --negative                                # and the checks can fail
./build/pwtest --bench                                   # 720p through 4K
python3 tools/sweep.py                                   # no control is silently dead
tools/verify.sh                                          # all of it, on a fresh universal build
```

Every check takes `--size`; run it at 320×180 as well as the raster you care about.
Footage goes through the real shaders with `--pipe`, in the fleet's frame format:

```bash
ffmpeg -i in.mov -vf fps=60 -f rawvideo -pix_fmt rgba -s 1920x1080 - \
  | ./build/pwtest --pipe --size 1920x1080 --script cues.txt \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 60 -i - out.mp4
```

See [`CLAUDE.md`](CLAUDE.md) for the full command reference and
[`AGENTS.md`](AGENTS.md) for the model and the traps.

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT — see [LICENSE](LICENSE).
