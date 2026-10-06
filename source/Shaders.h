#pragma once

#include <string>

/**
	The passes, every one of them at LED resolution except the last.

	1. **wall** -- each LED is the box mean of the p x p pixels it covers
	   (texelFetch, so at LED Pitch 1 it is a copy). Rows are stored top-down
	   from here on: LED row 0 is the top of the picture, as a processor counts.
	2. **copy** -- when a delay is asked for, the wall into the ring's layer.
	3. **route** -- the processor's map and the cable: which LED of which
	   tile, from which frame, each LED shows (swap, repeat, flip, delay).
	4. **hold** -- a tile that has lost its signal keeps the last frame it
	   received; every other tile's hold follows its route.
	5. **stats** -- one texel per TILE: the mean drive, the heat one-pole and
	   the supply's hiccup timer, ping-ponged.
	6. **panel** -- the receiving card's output (or its lost-signal fallback),
	   through the module, the LEDs, the calibration table and the supply.
	7. **display** -- to the host, in pixels: each pixel's LED over its share
	   of the emitter, mixed with the clip.

	The fragment shaders are assembled from pieces at InitGL -- kVersion +
	kCommon (+ kSignal) + the pass -- by `Assemble()`, the one place the order
	is written. tools/glslc.sh's ASSEMBLED table mirrors it, so the text it
	compiles is the text the plugin runs.
*/
namespace patchwork::shaders
{

extern const char* const kVersion;
extern const char* const kQuadVertex;
extern const char* const kCommon;
extern const char* const kSignal;
extern const char* const kWallFragment;
extern const char* const kCopyFragment;
extern const char* const kRouteFragment;
extern const char* const kHoldFragment;
extern const char* const kStatsFragment;
extern const char* const kPanelFragment;
extern const char* const kDisplayFragment;

enum class Pass
{
	Wall,
	Copy,
	Route,
	Hold,
	Stats,
	Panel,
	Display
};
std::string Assemble( Pass pass );

/// Negative-control hooks: bits of the `Hooks` uniform. A shipped instance
/// sets 0, and every branch they guard is then the shipped model. Each one
/// exists so a harness check can be shown to fail against a wrong model.
enum Hook : int
{
	kHookWallSkew       = 1 << 0,///< the wall's box starts one pixel right
	kHookTileSkew       = 1 << 1,///< calibration reads the tile one LED off
	kHookBatchUnbounded = 1 << 2,///< a batch id that ignores Batches
	kHookZebraSingle    = 1 << 3,///< a doubled row shows only its own data
	kHookBreakPrefix    = 1 << 4,///< a break loses the tiles BEFORE it
	kHookMirror         = 1 << 5,///< the repeat turns back with the source's direction
	kHookHoldFollows    = 1 << 6,///< the hold follows the route even when lost
	kHookPsuUncapped    = 1 << 7,///< Dim scales by the square root of the cap
	kHookHopSkew        = 1 << 8,///< the hop delay counts one hop too many
	kHookLedRate        = 1 << 9 ///< dead LEDs at twice the rate
};

} // namespace patchwork::shaders
