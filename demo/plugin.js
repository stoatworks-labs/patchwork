/**
 * Patchwork — browser demo.
 *
 * A faulty LED wall, as the signal chain it is. The one idea, from
 * `AGENTS.md`: **an LED wall is not one display, it is a few hundred small ones
 * on a daisy chain.** A processor cuts the picture into cabinet rectangles and
 * sends them down a cable that snakes from cabinet to cabinet; each cabinet's
 * receiving card takes its rectangle by its place on the chain, applies its own
 * calibration table, and drives its modules one scan line at a time. Every
 * failure an operator sees is one stage of that chain lying, and nothing on the
 * wall is drawn: the patchwork, the repeated tiles, the dead runs of cabinets,
 * the zebra stripes and the rest are those stages misbehaving on the picture.
 *
 * Two halves, and they are not equally faithful here:
 *
 *   **The GPU half is the plugin's own GLSL.** `shaders.js` is the ten
 *   `R"( ... )"` pieces of `source/Shaders.cpp` plus kVersion, spliced across
 *   by `demo/tools/check_shaders.py --write` and never typed, with the order
 *   the plugin joins them in (`Assemble()`, and InitGL's vertex stage) as
 *   `ASSEMBLY`. The same script compares all of it character for character and
 *   `tools/verify.sh` runs it. The seven passes — wall, copy, route, hold,
 *   stats, panel, display — run in the plugin's order into buffers of the
 *   plugin's formats (RGBA16F at LED resolution, an RGBA32F ping-pong at one
 *   texel per cabinet, an RGBA8 TEXTURE_2D_ARRAY ring of past walls), with the
 *   uniforms `Patchwork::ProcessOpenGL` sets.
 *
 *   **The CPU half is a port** (`createRenderer` and everything above it), and
 *   nothing checks it but a reader: Controls.cpp's table and conversions
 *   (`IntegerOf`, `OptionIndex`, `ScanGroups`, `ThresholdU32` and every
 *   `...FromParam`), `wall::MakeLayout`, `RepeatMotion` (the phase integrated
 *   per frame, `OffsetLeds` with Random's PCG from Hash.h via `Math.imul`),
 *   `wall::SlotOf`, the ring (its depth, write layer, fill count and what
 *   happens when it cannot be allocated), the hold and state ping-pongs and
 *   their validity flags, the heat's alpha, the 0.27 s restart, the frame
 *   counter and the test pattern's phase. `pwtest --chain-law` and
 *   `--motion-law` check the C++ and have never heard of this page. It was
 *   compared ONCE with the plugin through `pwtest --pipe` (2026-10-06, see
 *   AGENTS.md): ten settings, 40 frames each, the page on the same clip
 *   frames and the same 1/60 s clock -- nine byte for byte identical, the
 *   tenth 2 pixels of 9.2 million off by one. Nothing repeats that comparison.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **The clock is the page's.** The kit's clock is seconds accumulated from
 * frame deltas (at most 0.1 s a frame), paused by Pause, and handed to the
 * port as the host's time in seconds: the plugin's seconds-or-milliseconds
 * vote is not ported, because this host's unit is known. Everything after it
 * is the plugin's: dt is the step since the last frame, bounded at 0.25 s and
 * 0 for a step backwards (Restart) or the first frame.
 *
 * **A redraw with the clock stopped is not a new frame for the ring or the
 * frame counter.** The plugin is called once per host frame, so its ring of
 * past walls and its frame counter move every call. This page also redraws
 * when a control changes under Pause, with its clock standing still; if those
 * redraws wrote the ring, a paused Hop Delay ripple would wash out after a few
 * slider moves and Garbage and Floating zebra would change on every one. So the
 * ring takes a frame, and the counter counts one, only when the clock has moved
 * since the last frame drawn (Play, Step, Restart) — or when the ring has just
 * been allocated and holds nothing. Readout's page made the same choice for its
 * ring. Everything else runs exactly as it would on a host frame with no time
 * passing: the hold and the state still ping-pong, the heat's alpha is 0 and
 * the hiccup timer stands.
 *
 * **The integer controls are dropdowns.** LED Pitch, Tile W, Tile H, Modules X,
 * Modules Y, Tiles Per Port, Batches, Repeat Tiles, Lag Frames and Fault Seed
 * are FF_TYPE_INTEGER in the plugin, with real ranges. The kit has no integer
 * control, so each is a dropdown of every value in the plugin's range; the
 * page's value is the element index, and the plugin's integer is the range's
 * minimum plus it.
 *
 * **Nothing audio.** The plugin has no audio input, so nothing is missing.
 *
 * **The About block is not a parameter here**, as on every page in the suite.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import * as S from './shaders.js';

const f32 = Math.fround;

//===========================================================================
// Constants (Controls.h, Patchwork.cpp)
//===========================================================================

const kScanCount = 6;
const kZebraWidthCount = 6;
/** How long a tripped supply stays off before it restarts. */
const kRestartSeconds = 0.27;
/** How often an intermittent connection decides whether it is connected. */
const kIntermittentSeconds = 0.1;
/** The deepest ring of past frames, and the memory it may take. */
const kMaxRing = 32;
const kRingBudgetBytes = 256.0 * 1024.0 * 1024.0;
/** Patchwork.cpp: seconds of host time one frame may advance the clock by. */
const kMaxFrameDelta = 0.25;

//===========================================================================
// Controls.cpp, ported: every host value to the plugin's units.
//
// The plugin's params[] is float, so a value from the page goes through f32
// on the way in (`host()` in the renderer) before any of these. They then run
// in double, as the C++ does.
//===========================================================================

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const clamp01 = (v) => clamp(v, 0.0, 1.0);
const geometric = (v, low, high) => low * Math.pow(high / low, clamp01(v));
function inverseGeometric(value, low, high) {
  const lo = Math.min(low, high);
  const hi = Math.max(low, high);
  return f32(Math.log(clamp(value, lo, hi) / low) / Math.log(high / low));
}

const kHeatTimeLow = 1.0;
const kHeatTimeHigh = 600.0;
const kDropLow = 0.05;
const kDropHigh = 5.0;
const kFlickerLow = 0.5;
const kFlickerHigh = 30.0;
const kMaxRepeatSpeed = 8.0;
const kMaxHop = 2.0;

/** std::lround: half away from zero. */
const lround = (v) => (v < 0 ? -Math.round(-v) : Math.round(v));

const FillFromParam = (v) => 0.05 + 0.95 * clamp01(v);
const SpreadFromParam = (v) => 0.6 * clamp01(v);
const ColourSpreadFromParam = (v) => 0.5 * clamp01(v);
const SeamFromParam = (v) => 0.6 * clamp01(v);
const HeatFromParam = (v) => 0.5 * clamp01(v);
const HeatTimeFromParam = (v) => geometric(v, kHeatTimeLow, kHeatTimeHigh);
const ParamFromHeatTime = (seconds) => inverseGeometric(seconds, kHeatTimeLow, kHeatTimeHigh);
function RepeatSpeedFromParam(v) {
  const centred = 2.0 * clamp01(v) - 1.0;
  return (centred < 0.0 ? -1.0 : 1.0) * kMaxRepeatSpeed * centred * centred;
}
function ParamFromRepeatSpeed(tilesPerSecond) {
  const magnitude = Math.sqrt(Math.min(Math.abs(tilesPerSecond), kMaxRepeatSpeed) / kMaxRepeatSpeed);
  return f32(0.5 + 0.5 * (tilesPerSecond < 0.0 ? -magnitude : magnitude));
}
const HopDelayFromParam = (v) => kMaxHop * clamp01(v);
const DropoutTimeFromParam = (v) => geometric(v, kDropLow, kDropHigh);
const ParamFromDropoutTime = (seconds) => inverseGeometric(seconds, kDropLow, kDropHigh);
const FlickerRateFromParam = (v) => geometric(v, kFlickerLow, kFlickerHigh);
const ParamFromFlickerRate = (hertz) => inverseGeometric(hertz, kFlickerLow, kFlickerHigh);
const PsuLimitFromParam = (v) => 0.05 + 0.95 * clamp01(v);
function LedRateFromParam(v) {
  const x = clamp01(v);
  return 0.2 * x * x;
}

/** A probability as the integer threshold a 32-bit hash is compared with. */
function ThresholdU32(probability) {
  if (!(probability > 0.0)) return 0;
  const scaled = Math.round(probability * 4294967296.0);
  return scaled >= 4294967295.0 ? 0xffffffff : scaled >>> 0;
}

const OptionIndex = (value, count) => clamp(lround(value), 0, count - 1);
const ScanGroups = (option) => 1 << OptionIndex(option, kScanCount);

const SCAN_NAMES = ['1/1', '1/2', '1/4', '1/8', '1/16', '1/32'];
const ROUTE_NAMES = ['Row Snake', 'Column Snake', 'Rows', 'Columns'];
const CORNER_NAMES = ['Top Left', 'Top Right', 'Bottom Left', 'Bottom Right'];
const MOTION_NAMES = ['Hold', 'Step', 'Scroll', 'Ping-Pong', 'Random'];
const LOST_NAMES = ['Black', 'Hold', 'Test Pattern', 'Garbage'];
const PSU_NAMES = ['Dim', 'Hiccup'];
const WIDTH_NAMES = ['Random', '1 Row', '2 Rows', '4 Rows', '8 Rows', '16 Rows'];
const ZEBRA_NAMES = ['Stuck', 'Floating'];

const Route = { RowSnake: 0, ColumnSnake: 1, Rows: 2, Columns: 3, Count: 4 };
const Corner = { Count: 4 };
const Motion = { Hold: 0, Step: 1, Scroll: 2, PingPong: 3, Random: 4, Count: 5 };
const LostSignal = { Count: 4 };
const PsuMode = { Count: 2 };

//===========================================================================
// The table: Controls.cpp's InfoOf(), in ParamId order -- name, group, kind,
// default, and for an integer its range, for an option its elements. The
// key is this page's id for the control (the URL carries it).
//===========================================================================

const standard = (key, name, group, value) => ({ key, name, group, kind: 'standard', value });
const integer = (key, name, group, value, min, max) => ({ key, name, group, kind: 'integer', value, min, max });
const option = (key, name, group, value, options) => ({ key, name, group, kind: 'option', value, options });

const TABLE = [
  integer('ledPitch', 'LED Pitch', 'Wall', 2, 1, 16),
  standard('fill', 'Fill', 'Wall', 1.0),
  integer('tileW', 'Tile W', 'Wall', 60, 4, 512),
  integer('tileH', 'Tile H', 'Wall', 60, 4, 512),
  integer('modulesX', 'Modules X', 'Wall', 2, 1, 8),
  integer('modulesY', 'Modules Y', 'Wall', 2, 1, 8),
  option('scan', 'Scan', 'Wall', 4, SCAN_NAMES),

  option('route', 'Route', 'Chain', 0, ROUTE_NAMES),
  option('corner', 'Start Corner', 'Chain', 0, CORNER_NAMES),
  integer('perPort', 'Tiles Per Port', 'Chain', 0, 0, 256),

  standard('tileSpread', 'Tile Spread', 'Calibration', 0.3),
  standard('moduleSpread', 'Module Spread', 'Calibration', 0.15),
  standard('colourSpread', 'Colour Spread', 'Calibration', 0.3),
  integer('batches', 'Batches', 'Calibration', 0, 0, 16),
  standard('seams', 'Seams', 'Calibration', 0.3),
  standard('heat', 'Heat', 'Calibration', 0.0),
  standard('heatTime', 'Heat Time', 'Calibration', ParamFromHeatTime(30.0)),

  integer('repeatTiles', 'Repeat Tiles', 'Repeat', 0, 0, 64),
  standard('repeatFrom', 'Repeat From', 'Repeat', 0.0),
  standard('repeatReach', 'Repeat Reach', 'Repeat', 1.0),
  option('repeatMotion', 'Repeat Motion', 'Repeat', Motion.Scroll, MOTION_NAMES),
  standard('repeatSpeed', 'Repeat Speed', 'Repeat', ParamFromRepeatSpeed(1.0)),

  standard('swapped', 'Swapped Tiles', 'Mapping', 0.0),
  standard('flipped', 'Flipped Tiles', 'Mapping', 0.0),
  standard('hopDelay', 'Hop Delay', 'Mapping', 0.0),
  standard('lagTiles', 'Lag Tiles', 'Mapping', 0.0),
  integer('lagFrames', 'Lag Frames', 'Mapping', 4, 1, kMaxRing - 1),

  standard('chainBreak', 'Chain Break', 'Signal', 0.0),
  standard('intermittent', 'Intermittent', 'Signal', 0.0),
  standard('dropouts', 'Dropouts', 'Signal', 0.02),
  standard('dropoutTime', 'Dropout Time', 'Signal', ParamFromDropoutTime(0.5)),
  option('lostSignal', 'Lost Signal', 'Signal', 0, LOST_NAMES),

  standard('deadTiles', 'Dead Tiles', 'Power', 0.02),
  standard('flickerTiles', 'Flicker Tiles', 'Power', 0.0),
  standard('flickerRate', 'Flicker Rate', 'Power', ParamFromFlickerRate(8.0)),
  standard('psuLimit', 'PSU Limit', 'Power', 1.0),
  option('psuMode', 'PSU Mode', 'Power', 0, PSU_NAMES),

  standard('deadModules', 'Dead Modules', 'Modules', 0.0),
  standard('zebra', 'Zebra', 'Modules', 0.02),
  option('zebraWidth', 'Zebra Width', 'Modules', 0, WIDTH_NAMES),
  option('zebraMode', 'Zebra Mode', 'Modules', 0, ZEBRA_NAMES),
  standard('deadRows', 'Dead Rows', 'Modules', 0.02),
  standard('colourLoss', 'Colour Loss', 'Modules', 0.02),
  standard('shifted', 'Shifted', 'Modules', 0.01),

  standard('deadLeds', 'Dead LEDs', 'LEDs', 0.05),
  standard('stuckLeds', 'Stuck LEDs', 'LEDs', 0.03),

  integer('faultSeed', 'Fault Seed', 'Output', 1, 0, 999),
  standard('mix', 'Mix', 'Output', 1.0),
];
const INFO = Object.fromEntries(TABLE.map((row) => [row.key, row]));

/** Controls.cpp's IntegerOf: the host's float, rounded and clamped to the range. */
function IntegerOf(key, value) {
  const info = INFO[key];
  return clamp(lround(value), info.min, info.max);
}

/**
 * What the plugin's params[] would hold for this control: the page's value as
 * a float, and for an integer dropdown the range's minimum plus the index.
 */
function hostValue(params, key) {
  const info = INFO[key];
  const v = params.get(key);
  return info.kind === 'integer' ? f32(info.min + v) : f32(v);
}

//===========================================================================
// Hash.h, ported: PCG in uint32 throughout (= the GLSL's pcg()).
//===========================================================================

function Pcg(v) {
  const state = (Math.imul(v >>> 0, 747796405) + 2891336453) >>> 0;
  const word = Math.imul(((state >>> ((state >>> 28) + 4)) ^ state) >>> 0, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

//===========================================================================
// Wall.h / Wall.cpp, ported: what the plugin's CPU uses of them. (The cable's
// order, LinearIndex, TileAt, LinkOf and TravelOf, is the GLSL's on the GPU;
// the C++ copies are the harness's, and ProcessOpenGL never calls them.)
//===========================================================================

const ceilDiv = (a, b) => (b > 0 ? Math.trunc((a + b - 1) / b) : 0);

function MakeLayout(width, height, pitch, tileW, tileH, perPort, route, corner) {
  pitch = Math.max(pitch, 1);
  const layout = {
    ledsW: ceilDiv(Math.max(width, 1), pitch),
    ledsH: ceilDiv(Math.max(height, 1), pitch),
    tileW: Math.max(tileW, 1),
    tileH: Math.max(tileH, 1),
    cols: 0,
    rows: 0,
    perPort: 1,
    route,
    corner,
  };
  layout.cols = ceilDiv(layout.ledsW, layout.tileW);
  layout.rows = ceilDiv(layout.ledsH, layout.tileH);
  const tiles = layout.cols * layout.rows;
  layout.perPort = perPort > 0 ? Math.min(perPort, tiles) : tiles;
  return layout;
}

const EMPTY_LAYOUT = { ledsW: 0, ledsH: 0, tileW: 1, tileH: 1, cols: 0, rows: 0, perPort: 1, route: 0, corner: 0 };
const TilesOf = (l) => l.cols * l.rows;
const AlongX = (l) => l.route === Route.RowSnake || l.route === Route.Rows;
const AlongLength = (l) => (AlongX(l) ? l.tileW : l.tileH);

/** The repeat's motion: one phase in tiles, integrated in double every frame. */
class RepeatMotion {
  constructor() { this.phase = 0.0; }

  Advance(tilesPerSecond, dt) { this.phase += tilesPerSecond * dt; }

  /** The offset m in LEDs, 0 <= m < window x along. */
  OffsetLeds(motion, window, along, seed) {
    if (window <= 0 || along <= 0) return 0;
    const span = window * along;
    const phase = this.phase;
    let m = 0;
    switch (motion) {
      case Motion.Hold: m = 0; break;
      case Motion.Step: m = Math.floor(phase) * along; break;
      case Motion.Scroll: m = Math.floor(phase * along); break;
      case Motion.PingPong: {
        // Out the length of the window and back: a triangle in tiles.
        const period = 2.0 * window;
        const u = phase - period * Math.floor(phase / period);
        const tiles = u < window ? u : period - u;
        m = Math.floor(tiles * along);
        break;
      }
      case Motion.Random: {
        // A new whole-tile offset each time the phase crosses a whole tile.
        const slot = Math.floor(Math.abs(phase)) >>> 0;
        m = (Pcg((slot ^ Pcg((seed ^ 0x52e9a1c3) >>> 0)) >>> 0) % (window >>> 0)) * along;
        break;
      }
      default: break;
    }
    // C++'s % truncates towards zero, as JavaScript's does.
    m %= span;
    if (m < 0) m += span;
    return m;
  }
}

/**
 * wall::SlotOf: floor( t / period ) split into a uint32 base and a float
 * fraction, so the shader adds a per-tile phase without a large float.
 */
function SlotOf(seconds, period) {
  const slot = { base: 0, fraction: 0.0 };
  if (!(period > 0.0)) return slot;
  const u = seconds / period;
  const whole = Math.floor(u);
  // static_cast< uint32_t >( ( long long )whole & 0xFFFFFFFF ): ToUint32 is
  // exactly that reduction for any integer a double holds.
  slot.base = whole >>> 0;
  slot.fraction = f32(u - whole);
  // A fraction that rounds up to 1.0 in float is a carry.
  if (slot.fraction >= 1.0) {
    slot.fraction = 0.0;
    slot.base = (slot.base + 1) >>> 0;
  }
  return slot;
}

//===========================================================================
// The ring of past walls: Patchwork.cpp's ensureRing / releaseRing. The kit's
// PassBuffer is 2D only, so the array texture is here.
//===========================================================================

class Ring {
  constructor(gl) {
    this.gl = gl;
    this.texture = null;
    this.fbo = null;
    this.width = 0;
    this.height = 0;
    this.depth = 0;
    this.write = 0;
    this.filled = 0;
    this.failed = null;
  }

  /** Allocate at this size and depth, or keep the one that matches. */
  ensure(width, height, depth) {
    const gl = this.gl;
    if (this.texture && this.width === width && this.height === height && this.depth === depth) return true;
    this.release();

    // The plugin reads glGetError straight after the allocation. Drain what
    // is already queued here first, so an older error is not taken for this.
    for (let i = 0; i < 16 && gl.getError() !== gl.NO_ERROR; i += 1);

    this.texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.texture);
    gl.texImage3D(gl.TEXTURE_2D_ARRAY, 0, gl.RGBA8, width, height, depth, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, null);
    const allocated = gl.getError() === gl.NO_ERROR;
    this.fbo = gl.createFramebuffer();
    if (!allocated) {
      this.release();
      this.failed = { width, height, depth };
      return false;
    }
    this.failed = null;
    this.width = width;
    this.height = height;
    this.depth = depth;
    this.write = 0;
    this.filled = 0;
    return true;
  }

  release() {
    const gl = this.gl;
    if (this.fbo) gl.deleteFramebuffer(this.fbo);
    if (this.texture) gl.deleteTexture(this.texture);
    this.fbo = null;
    this.texture = null;
    this.width = 0;
    this.height = 0;
    this.depth = 0;
    this.write = 0;
    this.filled = 0;
  }
}

/**
 * A 1 x 1 x 1 array bound to the ring's unit while there is no ring (InitGL's
 * `emptyRing`): a sampler2DArray on an empty unit is undefined.
 */
function emptyArray(gl) {
  const t = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D_ARRAY, t);
  gl.texImage3D(gl.TEXTURE_2D_ARRAY, 0, gl.RGBA8, 1, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));
  gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.bindTexture(gl.TEXTURE_2D_ARRAY, null);
  return t;
}

//===========================================================================
// Uniforms by the type the shader declares.
//
// FFGLShader::Set picks glUniform1i or glUniform1f by the C++ argument's type,
// and the plugin's calls match the shaders' declarations (tools/sweep.py would
// find a control that went dead because they did not). JavaScript has one
// number type, so here the declared type, read back from the linked program,
// picks the call. A name the program does not have, or has optimised away, is
// a no-op as glUniform( -1 ) is in GL -- but it is remembered, so a name that
// no pass has at all (a typo, a uniform renamed in the shader) is reported
// under the picture rather than silently leaving a control dead.
//===========================================================================

function typed(gl, program) {
  const types = new Map();
  const count = gl.getProgramParameter(program.program, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < count; i += 1) {
    const info = gl.getActiveUniform(program.program, i);
    types.set(info.name, info.type);
  }
  return {
    program,
    types,
    unset: new Set(),
    use() {
      program.use();
      return this;
    },
    set(name, x, y) {
      const type = types.get(name);
      if (type === undefined) {
        this.unset.add(name);
        return this;
      }
      const loc = program.location(name);
      switch (type) {
        case gl.FLOAT: gl.uniform1f(loc, x); break;
        case gl.INT:
        case gl.SAMPLER_2D:
        case gl.SAMPLER_2D_ARRAY: gl.uniform1i(loc, x | 0); break;
        case gl.UNSIGNED_INT: gl.uniform1ui(loc, x >>> 0); break;
        case gl.INT_VEC2: gl.uniform2i(loc, x | 0, y | 0); break;
        default: throw new Error(`the ${program.label} pass declares ${name} with a type this page does not set`);
      }
      return this;
    },
  };
}

//===========================================================================
// The plugin's per-instance state and ProcessOpenGL, ported.
//===========================================================================

function createRenderer(gl, quad) {
  // InitGL: the vertex stage is kVersion + kQuadVertex; every fragment shader
  // is Assemble()'d. Both orders come from shaders.js's ASSEMBLY, which
  // check_shaders.py reads out of the C++.
  const join = (stage) => S.ASSEMBLY[stage].map((piece) => S[piece]).join('');
  const vertex = join('vertex');
  const STAGES = ['wall', 'copy', 'route', 'hold', 'stats', 'panel', 'display'];
  const programs = {};
  for (const stage of STAGES) programs[stage] = typed(gl, new Program(gl, vertex, join(stage), stage));

  const st = {
    wallPass: new PassBuffer(gl, { filter: 'nearest' }),
    routePass: new PassBuffer(gl, { filter: 'nearest' }),
    holdPass: [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })],
    statePass: [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })],
    panelPass: new PassBuffer(gl, { filter: 'nearest' }),
    holdIndex: 0,
    stateIndex: 0,
    holdValid: false,
    stateValid: false,
    ring: new Ring(gl),
    emptyRing: emptyArray(gl),
    layout: EMPTY_LAYOUT,
    motion: new RepeatMotion(),
    frameCounter: 0,
    lastNow: -1.0,
    stats: null,
    deadUniforms: null,
  };

  const unbindTextureUnits = (count) => {
    for (let unit = count - 1; unit >= 0; unit -= 1) bindTexture(gl, unit, null);
    gl.activeTexture(gl.TEXTURE0);
  };
  const into = (buffer) => {
    gl.bindFramebuffer(gl.FRAMEBUFFER, buffer.fbo);
    gl.viewport(0, 0, buffer.width, buffer.height);
  };

  /** Names set on some pass and declared by none: a dead uniform. Once. */
  function findDeadUniforms() {
    const everSet = new Set();
    const live = new Set();
    for (const p of Object.values(programs)) {
      for (const name of p.unset) everSet.add(name);
      for (const name of p.types.keys()) live.add(name);
    }
    return [...everSet].filter((name) => !live.has(name)).sort();
  }

  return {
    get stats() { return st.stats; },

    render({ input, params, width, height, time }) {
      const host = (key) => hostValue(params, key);
      const w = input.width;
      const h = input.height;

      //-------------------------------------------------------------------
      // The clock. dt is bounded so a stall does not dump seconds of heat or
      // tiles of repeat motion into one frame; a step backwards is no time.
      //-------------------------------------------------------------------
      const now = time;
      const dt = st.lastNow >= 0.0 && now > st.lastNow ? Math.min(now - st.lastNow, kMaxFrameDelta) : 0.0;
      // See the header: the ring and the counter move with the clock only.
      const clockMoved = st.lastNow < 0.0 || now !== st.lastNow;
      st.lastNow = now;
      if (clockMoved) st.frameCounter = (st.frameCounter + 1) >>> 0;

      //-------------------------------------------------------------------
      // What the controls say.
      //-------------------------------------------------------------------
      const pitch = IntegerOf('ledPitch', host('ledPitch'));
      const tileW = IntegerOf('tileW', host('tileW'));
      const tileH = IntegerOf('tileH', host('tileH'));
      const modulesX = Math.min(IntegerOf('modulesX', host('modulesX')), tileW);
      const modulesY = Math.min(IntegerOf('modulesY', host('modulesY')), tileH);
      const scan = ScanGroups(host('scan'));
      const routeKind = OptionIndex(host('route'), Route.Count);
      const cornerKind = OptionIndex(host('corner'), Corner.Count);
      const perPort = IntegerOf('perPort', host('perPort'));
      const seed = IntegerOf('faultSeed', host('faultSeed')) >>> 0;

      const previous = st.layout;
      const layout = MakeLayout(w, h, pitch, tileW, tileH, perPort, routeKind, cornerKind);
      st.layout = layout;
      const { ledsW, ledsH } = layout;

      const repeatTiles = IntegerOf('repeatTiles', host('repeatTiles'));
      const repeatMotion = OptionIndex(host('repeatMotion'), Motion.Count);
      const hopDelay = HopDelayFromParam(host('hopDelay'));
      const lagRate = clamp(host('lagTiles'), 0.0, 1.0);
      const wantRing = hopDelay > 0.0 || lagRate > 0.0;

      //-------------------------------------------------------------------
      // Buffers. A reallocation of the hold or the state starts it again.
      //-------------------------------------------------------------------
      const ledsChanged = ledsW !== previous.ledsW || ledsH !== previous.ledsH;
      const tilesChanged = layout.cols !== previous.cols || layout.rows !== previous.rows;
      st.wallPass.ensure(ledsW, ledsH, gl.RGBA16F);
      st.routePass.ensure(ledsW, ledsH, gl.RGBA16F);
      st.holdPass[0].ensure(ledsW, ledsH, gl.RGBA16F);
      st.holdPass[1].ensure(ledsW, ledsH, gl.RGBA16F);
      st.panelPass.ensure(ledsW, ledsH, gl.RGBA16F);
      st.statePass[0].ensure(layout.cols, layout.rows, gl.RGBA32F);
      st.statePass[1].ensure(layout.cols, layout.rows, gl.RGBA32F);
      if (ledsChanged) st.holdValid = false;
      if (tilesChanged) st.stateValid = false;

      let ringDepthWanted = 0;
      if (wantRing) {
        const layerBytes = 4.0 * ledsW * ledsH;
        ringDepthWanted = clamp(Math.trunc(kRingBudgetBytes / layerBytes), 2, kMaxRing);
        if (!st.ring.ensure(ledsW, ledsH, ringDepthWanted)) ringDepthWanted = 0;
      } else if (st.ring.texture) {
        st.ring.release();
      }
      const ringActive = ringDepthWanted > 0;

      //-------------------------------------------------------------------
      // The time, reduced in double: the repeat's phase integrated, the
      // random processes as a slot and a fraction.
      //-------------------------------------------------------------------
      st.motion.Advance(RepeatSpeedFromParam(host('repeatSpeed')), dt);
      const along = AlongLength(layout);
      const tiles = TilesOf(layout);
      const lastLength = tiles - Math.trunc((tiles - 1) / layout.perPort) * layout.perPort;
      const windowFull = Math.min(repeatTiles, layout.perPort);
      const windowLast = Math.min(repeatTiles, lastLength);
      let shiftFull = 0;
      let shiftLast = 0;
      if (repeatTiles > 0) {
        const m = st.motion.OffsetLeds(repeatMotion, windowFull, along, seed);
        shiftFull = m;
        shiftLast = windowLast > 0 ? m % (windowLast * along) : 0;
      }

      const dropoutSlot = SlotOf(now, DropoutTimeFromParam(host('dropoutTime')));
      const flickerSlot = SlotOf(now, 1.0 / FlickerRateFromParam(host('flickerRate')));
      const contactSlot = SlotOf(now, kIntermittentSeconds);
      const patternPhase = ((Math.floor(now) % 4) + 4) % 4;

      const tau = HeatTimeFromParam(host('heatTime'));
      const heatAlpha = 1.0 - Math.exp(-dt / tau);

      //-------------------------------------------------------------------
      // The uniforms every pass shares.
      //-------------------------------------------------------------------
      const setCommon = (s) => {
        s.set('Leds', ledsW, ledsH);
        s.set('Tile', layout.tileW, layout.tileH);
        s.set('Tiles', layout.cols, layout.rows);
        s.set('Modules', modulesX, modulesY);
        s.set('Scan', scan);
        s.set('Route', routeKind);
        s.set('Corner', cornerKind);
        s.set('PerPort', layout.perPort);
        s.set('Seed', seed);
        s.set('Hooks', 0);
        s.set('TBreak', ThresholdU32(host('chainBreak')));
        s.set('TConnected', ThresholdU32(host('intermittent')));
        s.set('IntermittentBase', contactSlot.base);
        s.set('IntermittentFrac', contactSlot.fraction);
        s.set('TDropout', ThresholdU32(host('dropouts')));
        s.set('DropoutBase', dropoutSlot.base);
        s.set('DropoutFrac', dropoutSlot.fraction);
      };
      const setSignal = (s) => {
        s.set('RouteTex', 0);
        s.set('HoldTex', 1);
        s.set('Lost', OptionIndex(host('lostSignal'), LostSignal.Count));
        s.set('PatternPhase', patternPhase);
        s.set('Frame', st.frameCounter);
      };

      const psuLimit = f32(PsuLimitFromParam(host('psuLimit')));
      const psuMode = OptionIndex(host('psuMode'), PsuMode.Count);

      gl.disable(gl.BLEND);

      //-------------------------------------------------------------------
      // 1. The wall.
      //-------------------------------------------------------------------
      {
        into(st.wallPass);
        const s = programs.wall.use();
        bindTexture(gl, 0, input.texture);
        setCommon(s);
        s.set('InputTexture', 0);
        s.set('Size', w, h);
        s.set('Pitch', pitch);
        quad.draw();
        unbindTextureUnits(1);
      }

      //-------------------------------------------------------------------
      // 2. The ring: this frame's wall into the next layer -- when the clock
      // has moved, or the ring is new and holds nothing (see the header).
      //-------------------------------------------------------------------
      const ring = st.ring;
      if (ringActive && (clockMoved || ring.filled === 0)) {
        ring.write = (ring.write + 1) % ring.depth;
        ring.filled = Math.min(ring.filled + 1, ring.depth);
        gl.bindFramebuffer(gl.FRAMEBUFFER, ring.fbo);
        gl.framebufferTextureLayer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, ring.texture, 0, ring.write);
        gl.viewport(0, 0, ledsW, ledsH);
        const s = programs.copy.use();
        bindTexture(gl, 0, st.wallPass.texture);
        s.set('WallTex', 0);
        quad.draw();
        unbindTextureUnits(1);
      }

      //-------------------------------------------------------------------
      // 3. The route.
      //-------------------------------------------------------------------
      {
        into(st.routePass);
        const s = programs.route.use();
        bindTexture(gl, 0, st.wallPass.texture);
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D_ARRAY, ringActive ? ring.texture : st.emptyRing);
        gl.activeTexture(gl.TEXTURE0);
        setCommon(s);
        s.set('WallTex', 0);
        s.set('RingTex', 1);
        s.set('RingActive', ringActive ? 1 : 0);
        s.set('RingDepth', Math.max(ring.depth, 1));
        s.set('RingWrite', ring.write);
        s.set('RingFilled', Math.max(ring.filled, 1));
        s.set('TSwap', ThresholdU32(host('swapped')));
        s.set('TFlip', ThresholdU32(host('flipped')));
        s.set('TLag', ThresholdU32(lagRate));
        s.set('LagFrames', IntegerOf('lagFrames', host('lagFrames')));
        s.set('HopDelay', hopDelay);
        s.set('RepeatN', repeatTiles);
        s.set('RepeatFrom', clamp(host('repeatFrom'), 0.0, 1.0));
        s.set('RepeatReach', clamp(host('repeatReach'), 0.0, 1.0));
        s.set('RepeatShiftFull', shiftFull);
        s.set('RepeatShiftLast', shiftLast);
        quad.draw();
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D_ARRAY, null);
        unbindTextureUnits(1);
      }

      //-------------------------------------------------------------------
      // 4. The hold.
      //-------------------------------------------------------------------
      const holdPrev = st.holdIndex;
      st.holdIndex = 1 - st.holdIndex;
      {
        into(st.holdPass[st.holdIndex]);
        const s = programs.hold.use();
        bindTexture(gl, 0, st.routePass.texture);
        bindTexture(gl, 1, st.holdPass[holdPrev].texture);
        setCommon(s);
        s.set('RouteTex', 0);
        s.set('HoldPrev', 1);
        s.set('HoldValid', st.holdValid ? 1 : 0);
        quad.draw();
        unbindTextureUnits(2);
      }
      st.holdValid = true;

      //-------------------------------------------------------------------
      // 5. The stats, one texel per tile.
      //-------------------------------------------------------------------
      const statePrev = st.stateIndex;
      st.stateIndex = 1 - st.stateIndex;
      {
        into(st.statePass[st.stateIndex]);
        const s = programs.stats.use();
        bindTexture(gl, 0, st.routePass.texture);
        bindTexture(gl, 1, st.holdPass[st.holdIndex].texture);
        bindTexture(gl, 2, st.statePass[statePrev].texture);
        setCommon(s);
        setSignal(s);
        s.set('StatePrev', 2);
        s.set('StateValid', st.stateValid ? 1 : 0);
        s.set('HeatAlpha', heatAlpha);
        s.set('Dt', dt);
        s.set('PsuLimit', psuLimit);
        s.set('PsuMode', psuMode);
        s.set('Restart', kRestartSeconds);
        quad.draw();
        unbindTextureUnits(3);
      }
      st.stateValid = true;

      //-------------------------------------------------------------------
      // 6. The panel.
      //-------------------------------------------------------------------
      {
        into(st.panelPass);
        const s = programs.panel.use();
        bindTexture(gl, 0, st.routePass.texture);
        bindTexture(gl, 1, st.holdPass[st.holdIndex].texture);
        bindTexture(gl, 2, st.statePass[st.stateIndex].texture);
        setCommon(s);
        setSignal(s);
        s.set('StateTex', 2);
        s.set('TDeadModule', ThresholdU32(host('deadModules')));
        s.set('TZebra', ThresholdU32(host('zebra')));
        s.set('ZebraWidth', OptionIndex(host('zebraWidth'), kZebraWidthCount));
        s.set('ZebraFloating', OptionIndex(host('zebraMode'), 2));
        s.set('TDeadRow', ThresholdU32(host('deadRows')));
        s.set('TColourLoss', ThresholdU32(host('colourLoss')));
        s.set('TShift', ThresholdU32(host('shifted')));
        s.set('TDeadLed', ThresholdU32(LedRateFromParam(host('deadLeds'))));
        s.set('TStuckLed', ThresholdU32(LedRateFromParam(host('stuckLeds'))));
        s.set('TDeadTile', ThresholdU32(host('deadTiles')));
        s.set('TFlicker', ThresholdU32(host('flickerTiles')));
        s.set('FlickerBase', flickerSlot.base);
        s.set('FlickerFrac', flickerSlot.fraction);
        s.set('TileSpread', SpreadFromParam(host('tileSpread')));
        s.set('ModuleSpread', SpreadFromParam(host('moduleSpread')));
        s.set('ColourSpread', ColourSpreadFromParam(host('colourSpread')));
        s.set('Batches', IntegerOf('batches', host('batches')));
        s.set('Seams', SeamFromParam(host('seams')));
        s.set('Heat', HeatFromParam(host('heat')));
        s.set('PsuLimit', psuLimit);
        s.set('PsuMode', psuMode);
        quad.draw();
        unbindTextureUnits(3);
      }

      //-------------------------------------------------------------------
      // 7. The display, to the host: here the canvas, the composition's size.
      //-------------------------------------------------------------------
      {
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, width, height);
        const s = programs.display.use();
        bindTexture(gl, 0, st.panelPass.texture);
        bindTexture(gl, 1, input.texture);
        s.set('PanelTex', 0);
        s.set('InputTexture', 1);
        s.set('Size', w, h);
        s.set('Pitch', pitch);
        s.set('EmitHalf', 0.5 * Math.sqrt(FillFromParam(host('fill'))) * pitch);
        s.set('MixAmount', clamp(host('mix'), 0.0, 1.0));
        quad.draw();
        unbindTextureUnits(2);
      }

      if (st.deadUniforms === null) st.deadUniforms = findDeadUniforms();

      st.stats = {
        layout,
        modules: [modulesX, modulesY],
        scan,
        chains: Math.ceil(tiles / layout.perPort),
        ring: ringActive ? { depth: ring.depth, filled: ring.filled } : null,
        ringFailed: wantRing && !ringActive ? ring.failed : null,
        repeat: repeatTiles > 0 ? { window: windowFull, along, offset: shiftFull, phase: st.motion.phase } : null,
        frame: st.frameCounter,
        deadUniforms: st.deadUniforms,
      };
    },
  };
}

//===========================================================================
// The parameters: Controls.cpp's table, with the plugin's names, groups and
// defaults, in ParamId order. The hints are the user guide's, shortened.
//===========================================================================

const fmt = (n, digits = 0) => n.toFixed(digits);
/** Three significant figures, for the geometric controls. */
const sig = (n) => (n >= 100 ? n.toFixed(0) : n >= 10 ? n.toFixed(1) : n >= 1 ? n.toFixed(2) : n.toFixed(3));
const pct = (p, digits = 1) => `${(100 * p).toFixed(digits)}%`;

/** The readout beside each slider, in the plugin's units. Short: the column is narrow. */
const DISPLAY = {
  fill: (v) => `${pct(FillFromParam(f32(v)), 0)} lit`,
  tileSpread: (v) => `≤ ${pct(SpreadFromParam(f32(v)), 0)} darker`,
  moduleSpread: (v) => `≤ ${pct(SpreadFromParam(f32(v)), 0)} darker`,
  colourSpread: (v) => `≤ ${pct(ColourSpreadFromParam(f32(v)), 0)} colour`,
  seams: (v) => `±${fmt(SeamFromParam(f32(v)), 2)} pitch`,
  heat: (v) => (HeatFromParam(f32(v)) === 0 ? 'off' : `red −${pct(HeatFromParam(f32(v)), 0)}`),
  heatTime: (v) => `τ ${sig(HeatTimeFromParam(f32(v)))} s`,
  repeatFrom: (v) => `${fmt(clamp01(f32(v)), 2)} along`,
  repeatReach: (v) => `${fmt(clamp01(f32(v)), 2)} of rest`,
  repeatSpeed: (v) => {
    const s = RepeatSpeedFromParam(f32(v));
    return `${s > 0 ? '+' : s < 0 ? '−' : ''}${sig(Math.abs(s))} cab/s`;
  },
  swapped: (v) => `${pct(f32(v))} of pairs`,
  flipped: (v) => `${pct(f32(v))} of tiles`,
  hopDelay: (v) => (HopDelayFromParam(f32(v)) === 0 ? 'off' : `${fmt(HopDelayFromParam(f32(v)), 2)} fr/hop`),
  lagTiles: (v) => `${pct(clamp01(f32(v)))} of cards`,
  chainBreak: (v) => `${pct(f32(v))} of chains`,
  intermittent: (v) => `contact ${pct(f32(v))}`,
  dropouts: (v) => `${pct(f32(v))} per slot`,
  dropoutTime: (v) => `${sig(DropoutTimeFromParam(f32(v)))} s`,
  deadTiles: (v) => `${pct(f32(v))} of tiles`,
  flickerTiles: (v) => `${pct(f32(v))} of tiles`,
  flickerRate: (v) => `${sig(FlickerRateFromParam(f32(v)))} Hz`,
  psuLimit: (v) => (PsuLimitFromParam(f32(v)) >= 1 ? 'no limit' : `mean ≤ ${pct(PsuLimitFromParam(f32(v)), 0)}`),
  deadModules: (v) => `${pct(f32(v))} modules`,
  zebra: (v) => `${pct(f32(v))} modules`,
  deadRows: (v) => `${pct(f32(v))} modules`,
  colourLoss: (v) => `${pct(f32(v))} modules`,
  shifted: (v) => `${pct(f32(v))} modules`,
  deadLeds: (v) => `${pct(LedRateFromParam(f32(v)), 2)} of LEDs`,
  stuckLeds: (v) => `${pct(LedRateFromParam(f32(v)), 2)} of LEDs`,
  mix: (v) => fmt(clamp01(f32(v)), 2),
};

const INTEGER_NOTE = (info) => ` FF_TYPE_INTEGER ${info.min}–${info.max} in the plugin; a dropdown of every value here.`;

const HINTS = {
  ledPitch: 'Output pixels per LED. At 1 every pixel is an LED; at 4 each LED is a 4 × 4 block, the mean of the clip under it. Zebra stripes and dead LEDs are always whole LEDs.',
  fill: 'The lit fraction of each LED’s square, 5% to 100%. At 1 the LEDs touch; lower, black opens between them. Only visible above LED Pitch 1.',
  tileW: 'The cabinet’s width in LEDs. The wall starts at the top-left; partial cabinets at the right and bottom are cabinets like any other.',
  tileH: 'The cabinet’s height in LEDs.',
  modulesX: 'Modules across a cabinet (at most its width). Module faults and Module Spread act per module.',
  modulesY: 'Modules down a cabinet (at most its height).',
  scan: 'The scan ratio. In 1/S scan, row r of a module is addressed as r mod S, which is what zebra and dead rows act on. 1/1 has no address lines, so no zebra.',
  route: 'The order the cable visits the cabinets: Row Snake (along a row, back along the next — what most walls are cabled as), Column Snake, Rows, Columns. Invisible on a healthy wall; it matters for the repeat, a break, a ripple, a crossed pair.',
  corner: 'Which corner the cable starts in.',
  perPort: 'Cabinets each sending-card port drives before a new chain starts; 0 is one chain through the whole wall. Each chain breaks, repeats and ripples on its own.',
  tileSpread: 'How far a cabinet’s brightness can be off, up to 60% darker. Every calibration gain is at most 1, so white never clips the evidence away.',
  moduleSpread: 'The same for each module inside a cabinet, in brightness and colour.',
  colourSpread: 'How far a cabinet’s colour can be off: up to half of one channel.',
  batches: 'How many calibration tables the cabinets were dealt from. 0 gives every cabinet its own; 3 gives exactly three looks.',
  seams: 'Gaps between cabinets: a gap darkens the LEDs either side of it, an overlap brightens them. Visible on flat areas.',
  heat: 'How much red a fully hot cabinet loses, up to half; green loses about a third as much, blue a sixth. Off by default.',
  heatTime: 'Each cabinet’s temperature follows its own brightness with this time constant, 1 s to 10 minutes.',
  repeatTiles: 'N cabinets whose content the processor’s map repeats along the cable. 0 is off.',
  repeatFrom: 'Where along the cable the N cabinets start, as a fraction of the chain.',
  repeatReach: 'How much of the rest of the chain, after the N, shows the repeat. At 0 the N loop among themselves.',
  repeatMotion: 'Hold (still), Step (a whole cabinet at a time), Scroll (an LED at a time), Ping-Pong (out the length of the N and back) or Random (a random whole-cabinet offset). Content runs along the cable, so on a snake alternate rows run opposite ways — never mirrored.',
  repeatSpeed: 'Cabinets per second, −8 to +8, square law about the middle; negative runs against the cable.',
  swapped: 'The chance that a neighbouring pair of cabinets on the cable is crossed, so each shows the other’s content.',
  flipped: 'The chance that a cabinet was hung upside down: its content turned 180°.',
  hopDelay: 'Frames of buffering per hop along the cable, 0 to 2: a cut ripples down the chain. Needs a moving clip. Capped by the frames the ring holds (32 at most).',
  lagTiles: 'The chance that a cabinet’s card is late.',
  lagFrames: 'By up to how many frames a late card is late.',
  chainBreak: 'The chance that a chain’s cable is broken somewhere. Everything downstream of the break loses its signal.',
  intermittent: 'How often a broken connector makes contact anyway, re-decided ten times a second.',
  dropouts: 'The chance a cabinet loses its signal in any one slot of Dropout Time, each on its own clock.',
  dropoutTime: 'The dropout slot, 0.05 to 5 s.',
  lostSignal: 'What a card shows with no signal: Black, Hold (the last frame it received — needs a moving clip to tell from the route), Test Pattern (the module grid over a colour stepping red, green, blue, white once a second) or Garbage (new every frame).',
  deadTiles: 'The chance a cabinet has no power at all.',
  flickerTiles: 'The chance a cabinet’s supply is failing.',
  flickerRate: 'How fast a failing supply flickers off and on, 0.5 to 30 Hz.',
  psuLimit: 'The mean brightness a cabinet’s supply can hold, 5% to 100%. 100% is no limit.',
  psuMode: 'Dim: a cabinet driven over the limit sags to it. Hiccup: it trips off, restarts after 0.27 s, and trips again while it is still over.',
  deadModules: 'The chance a module is dark.',
  zebra: 'The chance a module’s address decoder has a line stuck: half the rows dark, the other half lit twice with two rows’ data. Light moved, not removed.',
  zebraWidth: 'Which line is stuck: Random per module, or a stripe of 1, 2, 4, 8 or 16 rows. A line the scan ratio lacks is clamped to the widest it has.',
  zebraMode: 'Stuck holds the line; Floating lets it pick up noise, so the stripes swap every frame.',
  deadRows: 'The chance a module has a dead row driver: one address dark in every scan group, a pinstripe every S rows.',
  colourLoss: 'The chance a module’s data chain breaks at a driver chip: one colour missing from that chip’s first column to the module’s edge.',
  shifted: 'The chance a module’s shift register missed a clock: its content slips one to four LEDs sideways.',
  deadLeds: 'Black LEDs: a dead pixel or two at the bottom of the slider, a fifth of the wall at the top.',
  stuckLeds: 'LEDs with one colour stuck full on, however dark the picture.',
  faultSeed: 'Which cabinets, modules and LEDs fail. The same seed fails the same ones every time.',
  mix: 'The wall over the clip. The wall is a display, so it is opaque: a dead cabinet is black, not a hole, and alpha goes to 1 with Mix.',
};

const PARAMS = TABLE.map((info) => {
  const base = { id: info.key, name: info.name, group: info.group, hint: HINTS[info.key] };
  if (info.kind === 'integer') {
    const elements = [];
    for (let n = info.min; n <= info.max; n += 1) elements.push(String(n));
    return { ...base, type: 'option', elements, default: info.value - info.min, hint: base.hint + INTEGER_NOTE(info) };
  }
  if (info.kind === 'option') return { ...base, type: 'option', elements: info.options, default: info.value };
  // The decimal the table writes; host() rounds it to the plugin's float.
  return { ...base, type: 'standard', default: info.value, display: DISPLAY[info.key] };
});

/** A preset's value for an integer control: the dropdown's index. */
const integerIndex = (key, n) => n - INFO[key].min;

let renderer = null;

const mounted = mountDemo({
  name: 'Patchwork',
  pluginId: 'PW01',
  kind: 'effect',
  tagline:
    'A faulty LED wall, as the signal chain it is. An LED wall is not one display but a few hundred small ones on a daisy chain: a processor cuts the picture into cabinets and sends them down a cable that snakes from cabinet to cabinet; each cabinet’s receiving card takes its rectangle by its place on the chain, applies its own calibration table and drives its modules one scan line at a time. Every stage can lie, and nothing is drawn: the patchwork, tiles repeated and flowing along the cable, everything downstream of a break gone black or frozen, a cut rippling down the chain, zebra stripes from a stuck address line, supplies sagging and tripping and red drooping as the wall heats up are those stages misbehaving on your picture.',
  repo: 'https://github.com/stoatworks-labs/patchwork',
  page: 'https://stoatworks-labs.com/software/patchwork/',

  blurb:
    'It is Patchwork’s own seven passes — wall, ring, route, hold, stats, panel and display — ported from the repository to WebGL2 and driven by a JavaScript port of the plugin’s CPU half (the controls, the wall’s layout, the repeat’s motion, the time slots, the ring of past frames, the hold and the heat), which only a reader checks. It runs on generated clips in this page, frame by frame at your display’s rate.',

  // Every pass writes alpha 1 at Mix 1; below it the clip's own alpha shows.
  showBackdrop: true,

  // The wall, route, hold and panel are RGBA16F and the stats RGBA32F: a
  // float render target is an opt-in in WebGL2 (EXT_color_buffer_float).
  needFloat: true,

  // Moving clips first: Hop Delay, Lag and Hold do nothing on a still picture.
  sources: ['scene', 'spot', 'grid', 'bars', 'ramp', 'detail', 'alpha'],

  params: PARAMS,

  // The user guide's "Start here" walk, as this page's own combinations. The
  // plugin ships no presets; every value is a real control at a real position.
  presets: {
    'Every fault off (LED Pitch 1: the clip)': {
      ledPitch: integerIndex('ledPitch', 1), tileSpread: 0, moduleSpread: 0, colourSpread: 0, seams: 0,
      dropouts: 0, deadTiles: 0, zebra: 0, deadRows: 0, colourLoss: 0, shifted: 0, deadLeds: 0, stuckLeds: 0,
    },
    'Three cabinets repeated, scrolling': { repeatTiles: integerIndex('repeatTiles', 3), repeatFrom: 0.45 },
    'Three cabinets repeated, random': { repeatTiles: integerIndex('repeatTiles', 3), repeatFrom: 0.45, repeatMotion: Motion.Random },
    'A broken cable, black': { chainBreak: 1 },
    'A broken cable, test pattern': { chainBreak: 1, lostSignal: 2 },
    'A broken cable, holding the last frame': { chainBreak: 1, lostSignal: 1 },
    'Chains of ten, about half broken, garbage': { perPort: integerIndex('perPort', 10), chainBreak: 0.5, lostSignal: 3 },
    'Zebra everywhere, floating': { zebra: 0.6, zebraMode: 1 },
    'Zebra, 4-row stripes': { zebra: 0.6, zebraWidth: 3 },
    'A cut rippling down the cable': { hopDelay: 0.5 },
    'Late cards': { lagTiles: 0.25, lagFrames: integerIndex('lagFrames', 12) },
    'Three batches': { batches: integerIndex('batches', 3), colourSpread: 0.8, tileSpread: 0.5 },
    'Supplies tripping on bright content': { psuLimit: 0.25, psuMode: 1 },
    'Hot wall, red drooping fast': { heat: 1, heatTime: ParamFromHeatTime(2.0) },
  },

  differences: [
    'The CPU half is a PORT, not the plugin’s code, and nothing checks it but a reader. Controls.cpp’s table and every conversion (IntegerOf, OptionIndex, the scan groups, ThresholdU32 and each control’s FromParam), wall::MakeLayout, the repeat’s motion (its phase integrated frame by frame, the offset in LEDs for Hold, Step, Scroll, Ping-Pong and Random with Random’s PCG, reduced twice for a full and a last chain), wall::SlotOf for the dropouts, the flicker and the intermittent connector, the ring of past walls (its depth, write layer and fill), the hold and state ping-pongs and their validity flags, the heat’s one-pole alpha, the 0.27 s restart, the frame counter and the test pattern’s phase are translated to JavaScript from Controls.cpp, Wall.cpp, Hash.h and Patchwork::ProcessOpenGL. The repository’s pwtest --chain-law and --motion-law check the C++ and have never heard of this page. When it was written it was compared once against the plugin itself: this page driven headlessly (Chrome, on an Apple GPU), its first frame at 0 s and then one Step of 1/60 s at a time, against the repository’s pwtest --pipe on the same clip frames at 640 × 360, 40 frames each of ten settings — the defaults; the repeat scrolling at two speeds, Random on a column snake from the bottom right, and Ping-Pong over chains of 11 with a shorter last chain; a break under Hold with fast dropouts; a break under Garbage with floating zebra; Hop Delay with late cards; Hiccup with the heat up; and 27 controls moved at once. Nine came out identical byte for byte, the tenth differed in 2 of 9.2 million pixels by one level, and seven deliberate one-line mistakes in the port each showed up. That comparison does not run again.',
    'The GPU half is not a port: the wall, copy, route, hold, stats, panel and display passes are the plugin’s own GLSL, joined in the order the plugin’s Assemble() joins them, into buffers of the plugin’s formats (RGBA16F at LED resolution, an RGBA32F texel per cabinet, an RGBA8 array texture of past walls). demo/tools/check_shaders.py fails the repository’s verify script if a character of them, or that order, drifts.',
    'The frames are this page’s, at your display’s refresh rate, not a composition’s frame rate. Hop Delay and Lag Frames count frames, so on a 120 Hz display a ripple crosses the wall in half the seconds it would in a 60 fps composition. The kit’s clock also caps a frame at a tenth of a second, so the plugin’s own quarter-second bound on a frame, ported, never comes into play.',
    'The clock is the page’s, in seconds. The plugin votes on whether the host counts in seconds or milliseconds; a browser’s unit is known, so that half is absent. Restart steps the clock back to 0, which the plugin takes as no time passing: dropouts, flicker and the intermittent connector replay their slots from 0 and the test pattern’s colour starts again at red, while the heat, the hiccup, the hold, the ring and the repeat’s phase carry on.',
    'With the page paused, moving a control redraws the frame, and that redraw does not count as a new frame for the ring or the frame counter, as the plugin would count every frame a host draws. So a paused ripple stays put while you move controls, and Garbage and Floating zebra hold still; a host calls the plugin for every frame it draws, so there its ring and its frame counter never stand still. The hold, the heat and the hiccup run as the plugin’s would with no time passing.',
    'The integer controls are dropdowns. LED Pitch, Tile W, Tile H, Modules X, Modules Y, Tiles Per Port, Batches, Repeat Tiles, Lag Frames and Fault Seed are FF_TYPE_INTEGER in the plugin, which a host shows as a number to type; the kit has no integer control, so each is a dropdown of every value in the plugin’s own range. A shared link carries the dropdown’s index, which is the value minus the range’s minimum (LED Pitch 2 is index 1).',
    'The ring of past walls is the plugin’s: RGBA8, as deep as 32 frames fit in 256 MB, allocated only while Hop Delay or Lag Tiles is up. At LED Pitch 1 and 1920 × 1080 that is a 265 MB texture, which a browser may refuse; the plugin then turns the delays off and so does this page, and the line under the picture says so. The ring fills at this page’s frame rate.',
    'Still clips show nothing of Hop Delay, Lag or Lost Signal’s Hold: a late copy of a still picture is the same picture. The synthetic scene and the lights on black move; the colour bars and the ramps do not. Resolume’s own clips are DXV, which may carry alpha; the kit’s clips here are premultiplied.',
    'The presets are this page’s own combinations, after the user guide’s “Start here”. The plugin ships no presets; every value is a real control at a real position.',
    'The plugin has no audio input, so nothing is missing there. The About block — buttons that open a browser — is not on the panel.',
    'Nothing here is measured. The plugin’s harness holds a wall with nothing wrong to the clip byte for byte, a stuck address line to blacking half the rows while the module’s mean light stays put, a cut to arriving one hop per frame in the cable’s exact order for every route, corner and port size, the repeat to the map-offset model byte for byte in every motion, a lost card to the last frame it received, the heat to its one-pole to 1e-7 and a tripped supply to blinking on the frame it should — each with a negative control. That harness, not this page, is the reason to believe the wall. The plugin itself has never been loaded into Resolume on macOS; on Windows it loads, registers and renders in Resolume Arena 7.27.1, on software rendering.',
  ],

  createRenderer: (gl, quad) => {
    renderer = createRenderer(gl, quad);
    return renderer;
  },
});

//---------------------------------------------------------------------------
// A status line under the picture: reports, measures nothing.
//---------------------------------------------------------------------------
const query = new URLSearchParams(window.location.search);
const embed = query.has('embed') && query.get('embed') !== '0';

if (mounted && !embed) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    line.id = 'patchwork-stats';
    line.setAttribute('aria-live', 'off');
    stage.append(line);
    const plural = (n, word) => `${n} ${word}${n === 1 ? '' : 's'}`;
    setInterval(() => {
      const s = renderer?.stats;
      if (!s) return;
      const l = s.layout;
      const parts = [
        `wall ${l.ledsW} × ${l.ledsH} LEDs`,
        `${l.cols} × ${l.rows} cabinets of ${l.tileW} × ${l.tileH}, ${s.modules[0]} × ${s.modules[1]} modules at 1/${s.scan}`,
        s.chains === 1 ? `one chain of ${l.perPort}` : `${s.chains} chains of up to ${l.perPort}`,
      ];
      if (s.ring) parts.push(`ring ${plural(s.ring.depth, 'frame')} deep, ${s.ring.filled} written`);
      else if (s.ringFailed) {
        parts.push(`the ring (${s.ringFailed.depth} × ${s.ringFailed.width} × ${s.ringFailed.height}) could not be allocated: delays are off, as in the plugin`);
      }
      if (s.repeat) {
        parts.push(`repeat offset ${s.repeat.offset} of ${s.repeat.window * s.repeat.along} LEDs along the cable`);
      }
      if (s.deadUniforms && s.deadUniforms.length > 0) {
        parts.push(`uniforms no pass declares: ${s.deadUniforms.join(', ')}`);
      }
      line.textContent = parts.join(' · ');
    }, 250);
  }
}
