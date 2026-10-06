# Attributions

Patchwork is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

It is a PROVISIONAL HAND COPY: the fleet's master lists live in the
`stoatworks-backend` repo and are pushed out by `scripts/sync-attributions.py`
once a plugin is registered. Landing patchwork in the fleet regenerates it.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The LED-wall effect's shape, the clock and PassBuffer — Stoatworks pitch

<https://github.com/stoatworks-labs/pitch>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin class's shape, the host-clock unit vote (readout's, carried by pitch),
the integer cabinet arithmetic and seeded per-cabinet faults by PCG hash,
`source/PassBuffer.{h,cpp}` (tinsel's FFGLFBO with the colour-texture leak fixed),
`source/Diag.{h,cpp}`, the About headers and the release workflow and
`scripts/release-lib.sh` are pitch's, adapted.

### Harness, GL state and tooling — Stoatworks conway

<https://github.com/stoatworks-labs/conway>  
Licence: MIT  
Copyright: Stoatworks Labs

`source/GLState.h`, `source/Hash.h`, the harness's rig, PNG writer, cue sheets,
`--pipe`/`--film`, `--negative` with its `Perturb`, `--state`, `--names`, `--cues`
and `--offline`, and `tools/verify.sh`, `tools/mutate.sh`, `tools/glslc.sh`,
`tools/sweep.py` and the CI workflow are conway's (which carries them from radar,
boreal, flyback, downpour, tinsel and plotter).

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned at b1afaf9 like the fleet.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

## Media

`docs/hero.png` is one frame of `IntoTheGlow_02`, a demo clip bundled with Resolume
Arena, rendered through the plugin by `pwtest --clip`.
