# Attributions

Patchwork is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The LED-wall effect's shape, the clock and PassBuffer — Stoatworks pitch

<https://github.com/stoatworks-labs/pitch>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin class's shape, the host-clock unit vote (readout's, carried by pitch), the integer cabinet arithmetic and seeded per-cabinet faults by PCG hash, source/PassBuffer.{h,cpp} (tinsel's FFGLFBO with the colour-texture leak fixed), source/Diag.{h,cpp} and the release workflow and scripts/release-lib.sh are pitch's, adapted.

### Harness, GL state and tooling — Stoatworks conway

<https://github.com/stoatworks-labs/conway>  
Licence: MIT  
Copyright: Stoatworks Labs

source/GLState.h, source/Hash.h, the harness's rig, PNG writer, cue sheets, --pipe/--film, --negative with its Perturb, --state, --names, --cues and --offline, and tools/verify.sh, tools/mutate.sh, tools/glslc.sh, tools/sweep.py and the CI workflow are conway's (which carries them from radar, boreal, flyback, downpour, tinsel and plotter).

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### How LED video walls fail

The way a wall is built and cabled -- a processor cutting the picture into cabinets, receiving cards daisy-chained from a sending card's port, per-cabinet calibration tables, modules multiplexed one scan line at a time -- and the failures every operator has seen on one: mismatched batches, broken data chains, lost-signal fallbacks, zebra and pinstriped modules, dead driver chips, tripping supplies. Modelled from how the hardware works; no vendor's firmware, test patterns or documents are copied.

## Standards and published specifications

What the implementation is measured against.

- **HUB75 LED module interface** — the common LED panel interface: row address lines (A to E) select one row of each scan group through a decoder while shift-register driver chips clock in the column data. A stuck address line is the plugin's zebra mechanism, a dead row driver its pinstripes, a broken data chain its lost colour.
- **Melissa E. O'Neill, PCG (2014)** — the integer hash behind every seeded fault and random process, compared with integer thresholds on the GPU and in the harness.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
