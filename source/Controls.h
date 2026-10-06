#pragma once

#include "StoatworksAboutLinks.h"

#include <cstdint>

/**
    The host's parameters, and what they mean in the wall's units.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange`
    could widen it. The conversions live in Controls.cpp, one function per
    control. Option parameters hold the element index; LED Pitch, the tile
    and module sizes, Tiles Per Port, Batches, Repeat Tiles, Lag Frames and
    Fault Seed are real integers (`FF_TYPE_INTEGER`).

    Units: LEDs (the wall's own pixels), tiles, chain positions, frames,
    seconds, and probabilities as 32-bit integer thresholds (a hash below the
    threshold fires), so the GLSL and the harness compare integers and never
    round.
*/
namespace patchwork
{
/// Every control, in the order Resolume shows them. The About block is
/// LAST, so the day a user guide adds a button no control moves.
enum ParamId : unsigned int
{
	// -- Wall ----------------------------------------------------------------
	PT_LED_PITCH = 0,
	PT_FILL,
	PT_TILE_W,
	PT_TILE_H,
	PT_MODULES_X,
	PT_MODULES_Y,
	PT_SCAN,

	// -- Chain ---------------------------------------------------------------
	PT_ROUTE,
	PT_CORNER,
	PT_PER_PORT,

	// -- Calibration ---------------------------------------------------------
	PT_TILE_SPREAD,
	PT_MODULE_SPREAD,
	PT_COLOUR_SPREAD,
	PT_BATCHES,
	PT_SEAMS,
	PT_HEAT,
	PT_HEAT_TIME,

	// -- Repeat --------------------------------------------------------------
	PT_REPEAT_TILES,
	PT_REPEAT_FROM,
	PT_REPEAT_REACH,
	PT_REPEAT_MOTION,
	PT_REPEAT_SPEED,

	// -- Mapping -------------------------------------------------------------
	PT_SWAPPED,
	PT_FLIPPED,
	PT_HOP_DELAY,
	PT_LAG_TILES,
	PT_LAG_FRAMES,

	// -- Signal --------------------------------------------------------------
	PT_CHAIN_BREAK,
	PT_INTERMITTENT,
	PT_DROPOUTS,
	PT_DROPOUT_TIME,
	PT_LOST_SIGNAL,

	// -- Power ---------------------------------------------------------------
	PT_DEAD_TILES,
	PT_FLICKER_TILES,
	PT_FLICKER_RATE,
	PT_PSU_LIMIT,
	PT_PSU_MODE,

	// -- Modules -------------------------------------------------------------
	PT_DEAD_MODULES,
	PT_ZEBRA,
	PT_ZEBRA_WIDTH,
	PT_ZEBRA_MODE,
	PT_DEAD_ROWS,
	PT_COLOUR_LOSS,
	PT_SHIFTED,

	// -- LEDs ----------------------------------------------------------------
	PT_DEAD_LEDS,
	PT_STUCK_LEDS,

	// -- Output --------------------------------------------------------------
	PT_SEED,
	PT_MIX,

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_FIRST,
	PT_COUNT = PT_ABOUT_FIRST + 1 + stoatworks::about::kButtonCount
};

enum class Kind
{
	Standard,///< 0..1, converted in Controls.cpp
	Integer, ///< a real integer with a real range
	Option   ///< an element index
};

/// One control as the host is told about it.
struct ParamInfo
{
	unsigned int id;
	const char* name;  ///< at most 16 characters: FFGL's name field is not terminated
	const char* group;
	Kind kind;
	float defaultValue;///< in the host's units: 0..1, the integer, or the index
	float minimum;     ///< integers only
	float maximum;     ///< integers only
	const char* const* options;
	int optionCount;
};

/// The table, indexed by ParamId, for every id below PT_ABOUT_FIRST.
const ParamInfo& InfoOf( unsigned int id );

//---------------------------------------------------------------------------
// Options.
//---------------------------------------------------------------------------
enum class Route
{
	RowSnake = 0,///< along a row, back along the next: what most walls are cabled as
	ColumnSnake,
	Rows,        ///< every row left to right (from the start corner's side)
	Columns,
	Count
};
enum class Corner
{
	TopLeft = 0,
	TopRight,
	BottomLeft,
	BottomRight,
	Count
};
enum class Motion
{
	Hold = 0,///< the repeat stands still
	Step,    ///< whole tiles, Repeat Speed per second
	Scroll,  ///< whole LEDs: the content slides along the cable
	PingPong,///< scrolls out the length of the window and back
	Random,  ///< a random whole-tile offset, Repeat Speed times a second
	Count
};
enum class LostSignal
{
	Black = 0,
	Hold,///< the last frame the card received
	TestPattern,
	Garbage,
	Count
};
enum class PsuMode
{
	Dim = 0,///< the supply sags: a tile's mean is capped at the limit
	Hiccup, ///< the supply trips, restarts after kRestartSeconds, trips again
	Count
};
enum class ZebraMode
{
	Stuck = 0,///< the address line holds one value
	Floating, ///< it floats: a new value every frame
	Count
};

/// The scan ratio's denominators, by option index: 1/1 (static drive) to 1/32.
constexpr int kScanCount = 6;
int ScanGroups( float option );
/// Zebra Width's options: Random, then a stripe of 1, 2, 4, 8, 16 rows.
constexpr int kZebraWidthCount = 6;

//---------------------------------------------------------------------------
// Fixed physics.
//---------------------------------------------------------------------------
/// How long a tripped supply stays off before it restarts. Chosen not to be a
/// whole number of frames at any common rate (16.2 at 60 fps, 13.5 at 50,
/// 6.48 at 24), so a float sum of frame times never lands on the boundary.
constexpr double kRestartSeconds = 0.27;
/// How often an intermittent connection decides whether it is connected.
constexpr double kIntermittentSeconds = 0.1;
/// The heat droop of each channel at full heat, red first. AlInGaP red loses
/// light with junction temperature several times faster than InGaN green and
/// blue; these are typical datasheet ratios, not one part's.
constexpr float kHeatDroop[ 3 ] = { 1.0f, 0.3f, 0.15f };
/// The deepest ring of past frames, and the memory it may take.
constexpr int kMaxRing             = 32;
constexpr double kRingBudgetBytes = 256.0 * 1024.0 * 1024.0;
/// The stratified taps per side the tile mean is taken from.
constexpr int kStatTaps = 16;

//---------------------------------------------------------------------------
// The mappings.
//---------------------------------------------------------------------------
int IntegerOf( unsigned int id, float value );
int OptionIndex( float value, int count );

/// Fill: the lit fraction of each LED's square, 0.05 to 1.
double FillFromParam( float v );
/// Tile and module spreads: the largest fractional dimming of one tile or
/// module, 0 to 0.6. Every gain is at most 1, so nothing clips on white.
double SpreadFromParam( float v );
/// Colour Spread: the largest fractional loss of one channel, 0 to 0.5.
double ColourSpreadFromParam( float v );
/// Seams: the largest gap (or overlap) between cabinets, as a fraction of a
/// pitch: 0 to 0.6. A gap of g dims the LEDs either side of it by g/2.
double SeamFromParam( float v );
/// Heat: the red droop at full heat, 0 to 0.5.
double HeatFromParam( float v );
/// Heat Time: the thermal time constant, 1 s to 600 s, geometrically.
double HeatTimeFromParam( float v );
float ParamFromHeatTime( double seconds );
/// Repeat Speed: tiles per second, signed, -8 to 8, square law about the
/// middle so the slow end has room.
double RepeatSpeedFromParam( float v );
float ParamFromRepeatSpeed( double tilesPerSecond );
/// Hop Delay: frames per hop along the chain, 0 to 2.
double HopDelayFromParam( float v );
float ParamFromHopDelay( double framesPerHop );
/// Dropout Time: seconds per dropout slot, 0.05 to 5, geometrically.
double DropoutTimeFromParam( float v );
float ParamFromDropoutTime( double seconds );
/// Flicker Rate: hertz, 0.5 to 30, geometrically.
double FlickerRateFromParam( float v );
float ParamFromFlickerRate( double hertz );
/// PSU Limit: the mean drive a tile's supply holds, 0.05 to 1; 1 is no limit.
double PsuLimitFromParam( float v );
/// Per-LED rates: v squared times 0.2, so the bottom of the slider is a dead
/// pixel or two and the top a fifth of the wall.
double LedRateFromParam( float v );

/// A probability as the integer threshold a 32-bit hash is compared with:
/// round( p x 2^32 ), clamped to 2^32 - 1. The GLSL compares `hash < t`.
uint32_t ThresholdU32( double probability );

} // namespace patchwork
