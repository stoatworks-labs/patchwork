# patchwork

A faulty LED wall for Resolume Arena/Avenue, as one FFGL effect: `SW Patchwork`
(`PW01`). The clip is cut into cabinets on a daisy chain; every stage of that chain
— the processor's map, the cable, the receiving cards, the calibration tables, the
scan-driven modules, the LEDs, the supplies — can be made to fail. C++/GLSL, CMake
MODULE → a universal `.bundle` (macOS) + Windows `.dll`. MIT. Bundle id
`com.stoatworks.ffgl.patchwork`.

Read `AGENTS.md` before touching the cable (`Wall.{h,cpp}` and `kCommon`), the
repeat (`kRouteFragment`), the zebra (`kPanelFragment`), the stats pass or the
parameter table (`Controls.cpp`).

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (into `~/Documents/Resolume Arena/Extra Effects`)
- Render the harness's card: `./build/pwtest --out /tmp/wall.png --size 1920x1080`
- A frame of a real clip: `ffmpeg -ss 2 -i clip.mov -frames:v 1 -s 1920x1080 -f rawvideo -pix_fmt rgba /tmp/c.rgba && ./build/pwtest --clip /tmp/c.rgba --size 1920x1080 --out /tmp/w.png`
- List parameters: `./build/pwtest --list`
- Set anything by name: `./build/pwtest --set "Repeat Tiles=3" --set "Lost Signal=Test Pattern"`
  (an option by its element's name or index; anything that is neither is refused, exit 2)
- The card panning 3 px a frame (delays, lag and the hold need motion): add `--moving`
- A clip through the effect: `ffmpeg -i in.mov -vf fps=60 -f rawvideo -pix_fmt rgba -s WxH - | ./build/pwtest --pipe --size WxH --script cues.txt | ffmpeg -f rawvideo -pix_fmt rgba -s WxH -r 60 -i - out.mp4`
  (Resolume's demo clips are 30 fps; the harness clocks every frame at 1/60 s.)
- The card as video: `./build/pwtest --film 600 --moving --script cues.txt | ffmpeg …`
  A cue line is `frame  Parameter Name  value` (`#` starts a comment), in the units
  of `--set`. Held before the first key and after the last; a standard control is
  LINEAR between keys (so give a change a hold key the frame before), and an
  option or integer STEPS. An unknown name or a value that is not a number exits 2
  before any frame; a partial frame at EOF ends the stream with exit 0; a reader
  that hangs up ends `--pipe`/`--film` with exit 1 (SIGPIPE is ignored).

## Verify
- Everything: `tools/verify.sh` (~8 min: reserved words, no unbounded trig in the
  GLSL, glslc, fresh universal build, lipo/plist/codesign/oxbow probe+selftest, every
  check at 1280x720 and 320x180, the same on Apple's software renderer at 320x180,
  the offline set, the `--pipe` contract, the negative controls, the mutants, the
  sweep, the bench)
- **The wall**: `--identity`, `--tiles`, `--batches`, `--zebra`, `--leds`.
- **The cable**: `--chain` (a cut arriving one hop per frame; breaks), `--repeat`,
  `--hold`; no GL: `--chain-law`, `--motion-law`.
- **The supply and the heat**: `--psu`, `--heat`.
- **The machinery**: `--resize`, `--state`; no GL: `--cues`, `--names`.
- One raster only: add `--size WxH`. The software renderer:
  `PWTEST_RENDERER=software ./build/pwtest --zebra --size 320x180`.
- **The checks can fail**: `--negative` (15 wrong models), `tools/mutate.sh`
  (7 one-character mutants of the GLSL and Wall.cpp).
- What CI runs: `--offline` and `tools/glslc.sh`.
- No dead controls: `python3 tools/sweep.py` (48 parameters); `--bare` lists the 12
  that only act with another control set.
- Cost: `--bench` (720p/1080p/4K, GPU time by GL_TIME_ELAPSED).

## Notes
- **Everything at LED resolution is stored top row first**; only the wall pass (in)
  and the display pass (out) flip. `pwtest`'s `Panel()` reads LEDs top-down;
  `Output()` is GL's bottom-up.
- **The GLSL and `Wall.cpp` both define the cable**; `--chain` measures the GPU's
  order out of the picture and `--chain-law` the C++'s properties. Change both.
- **The repeat is a map offset, turned back with the destination's direction**:
  never mirrored. See AGENTS.md before "simplifying" it.
- **The shader gets the repeat offset reduced twice** (full chain, last chain).
- **Probabilities are integer thresholds** (`ThresholdU32`): a PCG hash below
  p × 2³² fires, never a float compare. No `fract(sin())`.
- **Nothing absolute crosses into GLSL**: time is a slot base and a fraction from
  `wall::SlotOf`, the repeat an integrated phase reduced in double.
- Every host parameter is 0..1 except the integers (LED Pitch, Tile W/H, Modules X/Y,
  Tiles Per Port, Batches, Repeat Tiles, Lag Frames, Fault Seed). An option reads
  back 0..1 whatever its count: map by element index.
- Negative-control hooks are the `Hooks` uniform (`shaders::Hook`), zero in every
  shipped frame; the CPU's are `SetHeatEulerForTest`, `SetResizeKeepsStateForTest`.
- No sin/cos/atan in the GLSL (verify.sh greps). Reserved words (`packed`, `smooth`,
  `noise1..4` …) must not be identifiers. No `M_PI`, no `far`/`near`.
- `patchwork_core` is an OBJECT library: the registration is a file-scope
  constructor nothing references.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are GENERATED by stoatworks-backend's
  `sync-about.py` / `sync-attributions.py`: edit the backend's tables, never these.
- The user guide is `docs/USER-GUIDE.md`; `docs/USER-GUIDE.pdf` and the site's copy are
  `stoatworks-website/scripts/build_guides.py patchwork`'s output, never edited by hand.

## Not done yet
- Never loaded into Resolume. Never built on Windows. Not a fleet repo: no GitHub
  repo, no `~/Projects/resolume/patchwork`, no registration, no tag.
- No user guide, no browser demo, no OpenFX port.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/patchwork/patchwork.YYYY-MM-DD.log
    %LOCALAPPDATA%\patchwork\logs\patchwork.YYYY-MM-DD.log
