#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace patchwork
{
namespace
{
double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double geometric( float v, double low, double high )
{
	return low * std::pow( high / low, clamp01( v ) );
}
float inverseGeometric( double value, double low, double high )
{
	const double lo = std::min( low, high ), hi = std::max( low, high );
	return static_cast< float >( std::log( std::clamp( value, lo, hi ) / low ) / std::log( high / low ) );
}

constexpr double kHeatTimeLow = 1.0, kHeatTimeHigh = 600.0;
constexpr double kDropLow = 0.05, kDropHigh = 5.0;
constexpr double kFlickerLow = 0.5, kFlickerHigh = 30.0;
constexpr double kMaxRepeatSpeed = 8.0;
constexpr double kMaxHop         = 2.0;

const char* const kScanNames[]   = { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" };
const char* const kRouteNames[]  = { "Row Snake", "Column Snake", "Rows", "Columns" };
const char* const kCornerNames[] = { "Top Left", "Top Right", "Bottom Left", "Bottom Right" };
const char* const kMotionNames[] = { "Hold", "Step", "Scroll", "Ping-Pong", "Random" };
const char* const kLostNames[]   = { "Black", "Hold", "Test Pattern", "Garbage" };
const char* const kPsuNames[]    = { "Dim", "Hiccup" };
const char* const kWidthNames[]  = { "Random", "1 Row", "2 Rows", "4 Rows", "8 Rows", "16 Rows" };
const char* const kZebraNames[]  = { "Stuck", "Floating" };

ParamInfo standard( unsigned int id, const char* name, const char* group, float value )
{
	return { id, name, group, Kind::Standard, value, 0.0f, 1.0f, nullptr, 0 };
}
ParamInfo integer( unsigned int id, const char* name, const char* group, float value, float lo, float hi )
{
	return { id, name, group, Kind::Integer, value, lo, hi, nullptr, 0 };
}
ParamInfo option( unsigned int id, const char* name, const char* group, float value, const char* const* names, int count )
{
	return { id, name, group, Kind::Option, value, 0.0f, static_cast< float >( count - 1 ), names, count };
}
} // namespace

//---------------------------------------------------------------------------
// The table. The defaults add up to a rental wall that has had a hard
// season: 60 x 60 LED cabinets of four modules at 1/16 scan, two pixels an
// LED (16 x 9 cabinets at 1080p), every cabinet a little off in brightness
// and colour, dark seams, a dead cabinet or two, a few modules with zebra
// stripes, pinstripes, a colour missing or the data slipped, and the odd
// cabinet dropping out for half a second. The repeat is off: it is a choice,
// not a fault you would leave in a default.
//---------------------------------------------------------------------------
const ParamInfo& InfoOf( unsigned int id )
{
	static const ParamInfo table[ PT_ABOUT_FIRST ] = {
		integer( PT_LED_PITCH, "LED Pitch", "Wall", 2.0f, 1.0f, 16.0f ),
		standard( PT_FILL, "Fill", "Wall", 1.0f ),
		integer( PT_TILE_W, "Tile W", "Wall", 60.0f, 4.0f, 512.0f ),
		integer( PT_TILE_H, "Tile H", "Wall", 60.0f, 4.0f, 512.0f ),
		integer( PT_MODULES_X, "Modules X", "Wall", 2.0f, 1.0f, 8.0f ),
		integer( PT_MODULES_Y, "Modules Y", "Wall", 2.0f, 1.0f, 8.0f ),
		option( PT_SCAN, "Scan", "Wall", 4.0f, kScanNames, kScanCount ),

		option( PT_ROUTE, "Route", "Chain", 0.0f, kRouteNames, static_cast< int >( Route::Count ) ),
		option( PT_CORNER, "Start Corner", "Chain", 0.0f, kCornerNames, static_cast< int >( Corner::Count ) ),
		integer( PT_PER_PORT, "Tiles Per Port", "Chain", 0.0f, 0.0f, 256.0f ),

		standard( PT_TILE_SPREAD, "Tile Spread", "Calibration", 0.3f ),
		standard( PT_MODULE_SPREAD, "Module Spread", "Calibration", 0.15f ),
		standard( PT_COLOUR_SPREAD, "Colour Spread", "Calibration", 0.3f ),
		integer( PT_BATCHES, "Batches", "Calibration", 0.0f, 0.0f, 16.0f ),
		standard( PT_SEAMS, "Seams", "Calibration", 0.3f ),
		standard( PT_HEAT, "Heat", "Calibration", 0.0f ),
		standard( PT_HEAT_TIME, "Heat Time", "Calibration", ParamFromHeatTime( 30.0 ) ),

		integer( PT_REPEAT_TILES, "Repeat Tiles", "Repeat", 0.0f, 0.0f, 64.0f ),
		standard( PT_REPEAT_FROM, "Repeat From", "Repeat", 0.0f ),
		standard( PT_REPEAT_REACH, "Repeat Reach", "Repeat", 1.0f ),
		option( PT_REPEAT_MOTION, "Repeat Motion", "Repeat", static_cast< float >( Motion::Scroll ), kMotionNames,
		        static_cast< int >( Motion::Count ) ),
		standard( PT_REPEAT_SPEED, "Repeat Speed", "Repeat", ParamFromRepeatSpeed( 1.0 ) ),

		standard( PT_SWAPPED, "Swapped Tiles", "Mapping", 0.0f ),
		standard( PT_FLIPPED, "Flipped Tiles", "Mapping", 0.0f ),
		standard( PT_HOP_DELAY, "Hop Delay", "Mapping", 0.0f ),
		standard( PT_LAG_TILES, "Lag Tiles", "Mapping", 0.0f ),
		integer( PT_LAG_FRAMES, "Lag Frames", "Mapping", 4.0f, 1.0f, static_cast< float >( kMaxRing - 1 ) ),

		standard( PT_CHAIN_BREAK, "Chain Break", "Signal", 0.0f ),
		standard( PT_INTERMITTENT, "Intermittent", "Signal", 0.0f ),
		standard( PT_DROPOUTS, "Dropouts", "Signal", 0.02f ),
		standard( PT_DROPOUT_TIME, "Dropout Time", "Signal", ParamFromDropoutTime( 0.5 ) ),
		option( PT_LOST_SIGNAL, "Lost Signal", "Signal", 0.0f, kLostNames, static_cast< int >( LostSignal::Count ) ),

		standard( PT_DEAD_TILES, "Dead Tiles", "Power", 0.02f ),
		standard( PT_FLICKER_TILES, "Flicker Tiles", "Power", 0.0f ),
		standard( PT_FLICKER_RATE, "Flicker Rate", "Power", ParamFromFlickerRate( 8.0 ) ),
		standard( PT_PSU_LIMIT, "PSU Limit", "Power", 1.0f ),
		option( PT_PSU_MODE, "PSU Mode", "Power", 0.0f, kPsuNames, static_cast< int >( PsuMode::Count ) ),

		standard( PT_DEAD_MODULES, "Dead Modules", "Modules", 0.0f ),
		standard( PT_ZEBRA, "Zebra", "Modules", 0.02f ),
		option( PT_ZEBRA_WIDTH, "Zebra Width", "Modules", 0.0f, kWidthNames, kZebraWidthCount ),
		option( PT_ZEBRA_MODE, "Zebra Mode", "Modules", 0.0f, kZebraNames, static_cast< int >( ZebraMode::Count ) ),
		standard( PT_DEAD_ROWS, "Dead Rows", "Modules", 0.02f ),
		standard( PT_COLOUR_LOSS, "Colour Loss", "Modules", 0.02f ),
		standard( PT_SHIFTED, "Shifted", "Modules", 0.01f ),

		standard( PT_DEAD_LEDS, "Dead LEDs", "LEDs", 0.05f ),
		standard( PT_STUCK_LEDS, "Stuck LEDs", "LEDs", 0.03f ),

		integer( PT_SEED, "Fault Seed", "Output", 1.0f, 0.0f, 999.0f ),
		standard( PT_MIX, "Mix", "Output", 1.0f ),
	};
	static const ParamInfo none = { PT_COUNT, "?", "?", Kind::Standard, 0.0f, 0.0f, 1.0f, nullptr, 0 };
	return id < PT_ABOUT_FIRST ? table[ id ] : none;
}

int ScanGroups( float option )
{
	return 1 << OptionIndex( option, kScanCount );
}

int IntegerOf( unsigned int id, float value )
{
	const ParamInfo& info = InfoOf( id );
	return std::clamp( static_cast< int >( std::lround( value ) ), static_cast< int >( info.minimum ),
	                   static_cast< int >( info.maximum ) );
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

double FillFromParam( float v )
{
	return 0.05 + 0.95 * clamp01( v );
}
double SpreadFromParam( float v )
{
	return 0.6 * clamp01( v );
}
double ColourSpreadFromParam( float v )
{
	return 0.5 * clamp01( v );
}
double SeamFromParam( float v )
{
	return 0.6 * clamp01( v );
}
double HeatFromParam( float v )
{
	return 0.5 * clamp01( v );
}
double HeatTimeFromParam( float v )
{
	return geometric( v, kHeatTimeLow, kHeatTimeHigh );
}
float ParamFromHeatTime( double seconds )
{
	return inverseGeometric( seconds, kHeatTimeLow, kHeatTimeHigh );
}
double RepeatSpeedFromParam( float v )
{
	const double centred = 2.0 * clamp01( v ) - 1.0;
	return ( centred < 0.0 ? -1.0 : 1.0 ) * kMaxRepeatSpeed * centred * centred;
}
float ParamFromRepeatSpeed( double tilesPerSecond )
{
	const double magnitude = std::sqrt( std::min( std::fabs( tilesPerSecond ), kMaxRepeatSpeed ) / kMaxRepeatSpeed );
	return static_cast< float >( 0.5 + 0.5 * ( tilesPerSecond < 0.0 ? -magnitude : magnitude ) );
}
double HopDelayFromParam( float v )
{
	return kMaxHop * clamp01( v );
}
float ParamFromHopDelay( double framesPerHop )
{
	return static_cast< float >( std::clamp( framesPerHop / kMaxHop, 0.0, 1.0 ) );
}
double DropoutTimeFromParam( float v )
{
	return geometric( v, kDropLow, kDropHigh );
}
float ParamFromDropoutTime( double seconds )
{
	return inverseGeometric( seconds, kDropLow, kDropHigh );
}
double FlickerRateFromParam( float v )
{
	return geometric( v, kFlickerLow, kFlickerHigh );
}
float ParamFromFlickerRate( double hertz )
{
	return inverseGeometric( hertz, kFlickerLow, kFlickerHigh );
}
double PsuLimitFromParam( float v )
{
	return 0.05 + 0.95 * clamp01( v );
}
double LedRateFromParam( float v )
{
	const double x = clamp01( v );
	return 0.2 * x * x;
}

uint32_t ThresholdU32( double probability )
{
	if( !( probability > 0.0 ) )
		return 0u;
	const double scaled = std::round( probability * 4294967296.0 );
	return scaled >= 4294967295.0 ? 0xFFFFFFFFu : static_cast< uint32_t >( scaled );
}

} // namespace patchwork
