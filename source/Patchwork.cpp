#include "Patchwork.h"

#include "Diag.h"
#include "GLState.h"
#include "Shaders.h"

#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace patchwork;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Patchwork >,// Create method
	"PW01",                    // Plugin unique ID of maximum length 4.
	"SW Patchwork",            // Plugin name
	2,                         // API major version number
	1,                         // API minor version number
	0,                         // Plugin major version number
	1,                         // Plugin minor version number
	FF_EFFECT,                 // Plugin type
	"A faulty LED wall.\n\nThe clip is cut into cabinets on a daisy chain, each with its own receiving card, calibration table and scan-driven modules -- and every stage of that chain can lie.\n\nWhat falls out: the decalibrated patchwork, tiles repeated and flowing along the cable, everything downstream of a break gone black or frozen, a cut rippling down the chain, zebra stripes from a stuck address line, a colour missing from a driver chip on, supplies sagging and tripping, and red drooping as the wall heats up.",
	"Patchwork FFGL effect" // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame is allowed to advance the clock by.
/// A stalled host or a dragged transport must not dump a minute of heat, or
/// a hundred tiles of repeat motion, into one frame.
constexpr double kMaxFrameDelta = 0.25;

/// Wall clock, for hosts that never call SetTime.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// glGetString returns nullptr when there is no current context.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

void setUint( FFGLShader& shader, const char* name, uint32_t value )
{
	glUniform1ui( shader.FindUniform( name ), value );
}
void setInt2( FFGLShader& shader, const char* name, int x, int y )
{
	glUniform2i( shader.FindUniform( name ), x, y );
}
} // namespace

//---------------------------------------------------------------------------
Patchwork::Patchwork()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Dropouts, flicker, an intermittent connector, the repeat's motion, the
	//heat and the supply's hiccup all run on the host's clock, so a re-render
	//of a composition fails the same tiles at the same moments.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Declaration, from the table in Controls.cpp. Every ranged
	// FF_TYPE_STANDARD parameter is a plain 0..1 float: SetParamInfo clamps a
	// STANDARD default into 0..1 before a range could be attached (SDK
	// b1afaf9). FF_TYPE_INTEGER is exempt, so the sizes, counts and the seed
	// are declared with their real ranges. Options are declared in their
	// natural order: every one here is a progression or a list to read.
	//---------------------------------------------------------------------
	for( unsigned int id = 0; id < PT_ABOUT_FIRST; ++id )
	{
		const patchwork::ParamInfo& info = InfoOf( id );
		params[ id ]                     = info.defaultValue;
		switch( info.kind )
		{
		case Kind::Standard: SetParamInfo( id, info.name, FF_TYPE_STANDARD, info.defaultValue ); break;
		case Kind::Integer:
			SetParamInfo( id, info.name, FF_TYPE_INTEGER, info.defaultValue );
			SetParamRange( id, info.minimum, info.maximum );
			break;
		case Kind::Option:
			SetOptionParamInfo( id, info.name, static_cast< unsigned int >( info.optionCount ), info.defaultValue );
			for( int i = 0; i < info.optionCount; ++i )
				SetParamElementInfo( id, static_cast< unsigned int >( i ), info.options[ i ], static_cast< float >( i ) );
			break;
		}
		SetParamGroup( id, info.group );
	}

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Patchwork effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Patchwork::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = std::string( shaders::kVersion ) + shaders::kQuadVertex;
	struct
	{
		FFGLShader* shader;
		shaders::Pass pass;
		const char* name;
	} const stages[] = {
		{ &wallShader, shaders::Pass::Wall, "wall" },       { &copyShader, shaders::Pass::Copy, "copy" },
		{ &routeShader, shaders::Pass::Route, "route" },    { &holdShader, shaders::Pass::Hold, "hold" },
		{ &statsShader, shaders::Pass::Stats, "stats" },    { &panelShader, shaders::Pass::Panel, "panel" },
		{ &displayShader, shaders::Pass::Display, "display" },
	};

	for( const auto& stage : stages )
	{
		const std::string fragment = shaders::Assemble( stage.pass );
		if( stage.shader->Compile( vertex.c_str(), fragment.c_str() ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume. This line is the only record of
		//which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Patchwork: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	glGenTextures( 1, &emptyRing );
	glBindTexture( GL_TEXTURE_2D_ARRAY, emptyRing );
	const unsigned char black[ 4 ] = { 0, 0, 0, 0 };
	glTexImage3D( GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D_ARRAY, 0 );

	holdValid = stateValid = false;
	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Patchwork::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//The unit voting is readout's, by way of pitch, unchanged: the ratio of the
//host's clock delta to a steady clock's names the unit outright, and nothing
//plausible sits between 1 and 1000.
double Patchwork::nowSeconds()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
		return wallNow - wallStart;

	const double raw = hostTime;

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
bool Patchwork::ensureRing( int ledsW, int ledsH, int depth )
{
	if( ringTexture != 0 && ringW == ledsW && ringH == ledsH && ringDepth == depth )
		return true;
	releaseRing();

	glGenTextures( 1, &ringTexture );
	glBindTexture( GL_TEXTURE_2D_ARRAY, ringTexture );
	glTexImage3D( GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, ledsW, ledsH, depth, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D_ARRAY, 0 );
	const bool allocated = glGetError() == GL_NO_ERROR;
	glGenFramebuffers( 1, &ringFBO );
	if( !allocated )
	{
		releaseRing();
		return false;
	}
	ringW      = ledsW;
	ringH      = ledsH;
	ringDepth  = depth;
	ringWrite  = 0;
	ringFilled = 0;
	return true;
}

void Patchwork::releaseRing()
{
	if( ringFBO != 0 )
		glDeleteFramebuffers( 1, &ringFBO );
	if( ringTexture != 0 )
		glDeleteTextures( 1, &ringTexture );
	ringFBO = ringTexture = 0;
	ringW = ringH = ringDepth = 0;
	ringWrite = ringFilled = 0;
}

//---------------------------------------------------------------------------
FFResult Patchwork::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	//The host's state, read before anything of ours changes it, and put back
	//whichever way out of here it is.
	ScopedGLState scopedState;
	const GLint* hostViewport = scopedState.saved.viewport;

	//---------------------------------------------------------------------
	// The clock. dt is bounded so a stall does not dump seconds of heat or
	// tiles of repeat motion into one frame.
	//---------------------------------------------------------------------
	const double now = nowSeconds();
	const double dt  = lastNow >= 0.0 && now > lastNow ? std::min( now - lastNow, kMaxFrameDelta ) : 0.0;
	lastNow          = now;
	++frameCounter;
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const int pitch    = IntegerOf( PT_LED_PITCH, params[ PT_LED_PITCH ] );
	const int tileW    = IntegerOf( PT_TILE_W, params[ PT_TILE_W ] );
	const int tileH    = IntegerOf( PT_TILE_H, params[ PT_TILE_H ] );
	const int modulesX = std::min( IntegerOf( PT_MODULES_X, params[ PT_MODULES_X ] ), tileW );
	const int modulesY = std::min( IntegerOf( PT_MODULES_Y, params[ PT_MODULES_Y ] ), tileH );
	const int scan     = ScanGroups( params[ PT_SCAN ] );
	const auto routeKind  = static_cast< Route >( OptionIndex( params[ PT_ROUTE ], static_cast< int >( Route::Count ) ) );
	const auto cornerKind = static_cast< Corner >( OptionIndex( params[ PT_CORNER ], static_cast< int >( Corner::Count ) ) );
	const int perPort  = IntegerOf( PT_PER_PORT, params[ PT_PER_PORT ] );
	const uint32_t seed = static_cast< uint32_t >( IntegerOf( PT_SEED, params[ PT_SEED ] ) );

	const wall::Layout previous = layout;
	layout = wall::MakeLayout( width, height, pitch, tileW, tileH, perPort, routeKind, cornerKind );
	const int ledsW = layout.ledsW, ledsH = layout.ledsH;

	const int repeatTiles = IntegerOf( PT_REPEAT_TILES, params[ PT_REPEAT_TILES ] );
	const auto repeatMotion = static_cast< Motion >( OptionIndex( params[ PT_REPEAT_MOTION ], static_cast< int >( Motion::Count ) ) );
	const double hopDelay   = HopDelayFromParam( params[ PT_HOP_DELAY ] );
	const double lagRate    = std::clamp( static_cast< double >( params[ PT_LAG_TILES ] ), 0.0, 1.0 );
	const bool wantRing     = hopDelay > 0.0 || lagRate > 0.0;

	//---------------------------------------------------------------------
	// Buffers. Every Ensure() happens here, before anything binds a texture:
	// allocating one leaves the active unit bound to nothing (tinsel's trap).
	// A reallocation of the hold or the state starts it again: what a card
	// last received at another raster means nothing at this one.
	//---------------------------------------------------------------------
	const bool ledsChanged  = ledsW != previous.ledsW || ledsH != previous.ledsH;
	const bool tilesChanged = layout.cols != previous.cols || layout.rows != previous.rows;
	const bool allocated    = wallPass.Ensure( ledsW, ledsH, GL_RGBA16F, PassBuffer::Sampling::Nearest )
	                       && routePass.Ensure( ledsW, ledsH, GL_RGBA16F, PassBuffer::Sampling::Nearest )
	                       && holdPass[ 0 ].Ensure( ledsW, ledsH, GL_RGBA16F, PassBuffer::Sampling::Nearest )
	                       && holdPass[ 1 ].Ensure( ledsW, ledsH, GL_RGBA16F, PassBuffer::Sampling::Nearest )
	                       && panelPass.Ensure( ledsW, ledsH, GL_RGBA16F, PassBuffer::Sampling::Nearest )
	                       && statePass[ 0 ].Ensure( layout.cols, layout.rows, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	                       && statePass[ 1 ].Ensure( layout.cols, layout.rows, GL_RGBA32F, PassBuffer::Sampling::Nearest );
	if( !allocated )
	{
		diag::error( "could not allocate the LED buffers: " + std::to_string( ledsW ) + "x" + std::to_string( ledsH )
		             + " LEDs - try a larger LED Pitch" );
		return FF_FAIL;
	}
	if( ledsChanged && !resizeKeepsState )
		holdValid = false;
	if( tilesChanged && !resizeKeepsState )
		stateValid = false;

	int ringDepthWanted = 0;
	if( wantRing )
	{
		const double layerBytes = 4.0 * ledsW * ledsH;
		ringDepthWanted         = std::clamp( static_cast< int >( kRingBudgetBytes / layerBytes ), 2, kMaxRing );
		if( !ensureRing( ledsW, ledsH, ringDepthWanted ) )
		{
			diag::warn( "could not allocate a ring of " + std::to_string( ringDepthWanted ) + " frames at " + std::to_string( ledsW )
			            + "x" + std::to_string( ledsH ) + " LEDs - delays are off" );
			ringDepthWanted = 0;
		}
	}
	else if( ringTexture != 0 )
		releaseRing();
	const bool ringActive = ringDepthWanted > 0;

	//---------------------------------------------------------------------
	// The time, reduced in double. Nothing absolute crosses into GLSL:
	// Resolume's clock has been seen at 499,217 s, where a float resolves to
	// 0.03 s. The repeat's phase is integrated (a change of speed never jumps
	// the content); the random processes take a slot index and a fraction.
	//---------------------------------------------------------------------
	motion.Advance( RepeatSpeedFromParam( params[ PT_REPEAT_SPEED ] ), dt );
	const int along       = layout.AlongLength();
	const int lastLength  = layout.Tiles() - ( ( layout.Tiles() - 1 ) / layout.perPort ) * layout.perPort;
	const int windowFull  = std::min( repeatTiles, layout.perPort );
	const int windowLast  = std::min( repeatTiles, lastLength );
	int shiftFull = 0, shiftLast = 0;
	if( repeatTiles > 0 )
	{
		const int m = motion.OffsetLeds( repeatMotion, windowFull, along, seed );
		shiftFull   = m;
		shiftLast   = windowLast > 0 ? m % ( windowLast * along ) : 0;
	}

	const wall::Slot dropoutSlot = wall::SlotOf( now, DropoutTimeFromParam( params[ PT_DROPOUT_TIME ] ) );
	const wall::Slot flickerSlot = wall::SlotOf( now, 1.0 / FlickerRateFromParam( params[ PT_FLICKER_RATE ] ) );
	const wall::Slot contactSlot = wall::SlotOf( now, kIntermittentSeconds );
	const int patternPhase       = static_cast< int >( static_cast< long long >( std::floor( now ) ) & 3 );

	const double tau       = HeatTimeFromParam( params[ PT_HEAT_TIME ] );
	const double heatAlpha = heatEuler ? std::min( dt / tau, 1.0 ) : 1.0 - std::exp( -dt / tau );

	//---------------------------------------------------------------------
	// The uniforms every pass shares.
	//---------------------------------------------------------------------
	const auto setCommon = [ & ]( FFGLShader& s ) {
		setInt2( s, "Leds", ledsW, ledsH );
		setInt2( s, "Tile", layout.tileW, layout.tileH );
		setInt2( s, "Tiles", layout.cols, layout.rows );
		setInt2( s, "Modules", modulesX, modulesY );
		s.Set( "Scan", scan );
		s.Set( "Route", static_cast< int >( routeKind ) );
		s.Set( "Corner", static_cast< int >( cornerKind ) );
		s.Set( "PerPort", layout.perPort );
		setUint( s, "Seed", seed );
		s.Set( "Hooks", hooks );
		setUint( s, "TBreak", ThresholdU32( params[ PT_CHAIN_BREAK ] ) );
		setUint( s, "TConnected", ThresholdU32( params[ PT_INTERMITTENT ] ) );
		setUint( s, "IntermittentBase", contactSlot.base );
		s.Set( "IntermittentFrac", contactSlot.fraction );
		setUint( s, "TDropout", ThresholdU32( params[ PT_DROPOUTS ] ) );
		setUint( s, "DropoutBase", dropoutSlot.base );
		s.Set( "DropoutFrac", dropoutSlot.fraction );
	};
	const auto setSignal = [ & ]( FFGLShader& s ) {
		s.Set( "RouteTex", 0 );
		s.Set( "HoldTex", 1 );
		s.Set( "Lost", OptionIndex( params[ PT_LOST_SIGNAL ], static_cast< int >( LostSignal::Count ) ) );
		s.Set( "PatternPhase", patternPhase );
		setUint( s, "Frame", frameCounter );
	};

	const float psuLimit = static_cast< float >( PsuLimitFromParam( params[ PT_PSU_LIMIT ] ) );
	const int psuMode    = OptionIndex( params[ PT_PSU_MODE ], static_cast< int >( PsuMode::Count ) );

	auto into = [ & ]( PassBuffer& buffer ) {
		glBindFramebuffer( GL_FRAMEBUFFER, buffer.GetGLID() );
		glViewport( 0, 0, static_cast< GLsizei >( buffer.GetWidth() ), static_cast< GLsizei >( buffer.GetHeight() ) );
	};
	glDisable( GL_BLEND );

	//---------------------------------------------------------------------
	// 1. The wall.
	//---------------------------------------------------------------------
	{
		into( wallPass );
		ScopedShaderBinding shader( wallShader.GetGLID() );
		bindUnit( 0, input.Handle );
		setCommon( wallShader );
		wallShader.Set( "InputTexture", 0 );
		setInt2( wallShader, "Size", width, height );
		wallShader.Set( "Pitch", pitch );
		quad.Draw();
		unbindTextureUnits( 1 );
	}

	//---------------------------------------------------------------------
	// 2. The ring: this frame's wall into the next layer.
	//---------------------------------------------------------------------
	if( ringActive )
	{
		ringWrite = ( ringWrite + 1 ) % ringDepth;
		ringFilled = std::min( ringFilled + 1, ringDepth );
		glBindFramebuffer( GL_FRAMEBUFFER, ringFBO );
		glFramebufferTextureLayer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ringTexture, 0, ringWrite );
		glViewport( 0, 0, ledsW, ledsH );
		ScopedShaderBinding shader( copyShader.GetGLID() );
		bindUnit( 0, wallPass.TextureID() );
		copyShader.Set( "WallTex", 0 );
		quad.Draw();
		unbindTextureUnits( 1 );
	}

	//---------------------------------------------------------------------
	// 3. The route.
	//---------------------------------------------------------------------
	{
		into( routePass );
		ScopedShaderBinding shader( routeShader.GetGLID() );
		bindUnit( 0, wallPass.TextureID() );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D_ARRAY, ringActive ? ringTexture : emptyRing );
		glActiveTexture( GL_TEXTURE0 );
		setCommon( routeShader );
		routeShader.Set( "WallTex", 0 );
		routeShader.Set( "RingTex", 1 );
		routeShader.Set( "RingActive", ringActive ? 1 : 0 );
		routeShader.Set( "RingDepth", std::max( ringDepth, 1 ) );
		routeShader.Set( "RingWrite", ringWrite );
		routeShader.Set( "RingFilled", std::max( ringFilled, 1 ) );
		setUint( routeShader, "TSwap", ThresholdU32( params[ PT_SWAPPED ] ) );
		setUint( routeShader, "TFlip", ThresholdU32( params[ PT_FLIPPED ] ) );
		setUint( routeShader, "TLag", ThresholdU32( lagRate ) );
		routeShader.Set( "LagFrames", IntegerOf( PT_LAG_FRAMES, params[ PT_LAG_FRAMES ] ) );
		routeShader.Set( "HopDelay", static_cast< float >( hopDelay ) );
		routeShader.Set( "RepeatN", repeatTiles );
		routeShader.Set( "RepeatFrom", std::clamp( params[ PT_REPEAT_FROM ], 0.0f, 1.0f ) );
		routeShader.Set( "RepeatReach", std::clamp( params[ PT_REPEAT_REACH ], 0.0f, 1.0f ) );
		routeShader.Set( "RepeatShiftFull", shiftFull );
		routeShader.Set( "RepeatShiftLast", shiftLast );
		quad.Draw();
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D_ARRAY, 0 );
		unbindTextureUnits( 1 );
	}

	//---------------------------------------------------------------------
	// 4. The hold.
	//---------------------------------------------------------------------
	const int holdPrev = holdIndex;
	holdIndex          = 1 - holdIndex;
	{
		into( holdPass[ holdIndex ] );
		ScopedShaderBinding shader( holdShader.GetGLID() );
		bindUnit( 0, routePass.TextureID() );
		bindUnit( 1, holdPass[ holdPrev ].TextureID() );
		setCommon( holdShader );
		holdShader.Set( "RouteTex", 0 );
		holdShader.Set( "HoldPrev", 1 );
		holdShader.Set( "HoldValid", holdValid ? 1 : 0 );
		quad.Draw();
		unbindTextureUnits( 2 );
	}
	holdValid = true;

	//---------------------------------------------------------------------
	// 5. The stats, one texel per tile.
	//---------------------------------------------------------------------
	const int statePrev = stateIndex;
	stateIndex          = 1 - stateIndex;
	{
		into( statePass[ stateIndex ] );
		ScopedShaderBinding shader( statsShader.GetGLID() );
		bindUnit( 0, routePass.TextureID() );
		bindUnit( 1, holdPass[ holdIndex ].TextureID() );
		bindUnit( 2, statePass[ statePrev ].TextureID() );
		setCommon( statsShader );
		setSignal( statsShader );
		statsShader.Set( "StatePrev", 2 );
		statsShader.Set( "StateValid", stateValid ? 1 : 0 );
		statsShader.Set( "HeatAlpha", static_cast< float >( heatAlpha ) );
		statsShader.Set( "Dt", static_cast< float >( dt ) );
		statsShader.Set( "PsuLimit", psuLimit );
		statsShader.Set( "PsuMode", psuMode );
		statsShader.Set( "Restart", static_cast< float >( kRestartSeconds ) );
		quad.Draw();
		unbindTextureUnits( 3 );
	}
	stateValid = true;

	//---------------------------------------------------------------------
	// 6. The panel.
	//---------------------------------------------------------------------
	{
		into( panelPass );
		ScopedShaderBinding shader( panelShader.GetGLID() );
		bindUnit( 0, routePass.TextureID() );
		bindUnit( 1, holdPass[ holdIndex ].TextureID() );
		bindUnit( 2, statePass[ stateIndex ].TextureID() );
		setCommon( panelShader );
		setSignal( panelShader );
		panelShader.Set( "StateTex", 2 );
		setUint( panelShader, "TDeadModule", ThresholdU32( params[ PT_DEAD_MODULES ] ) );
		setUint( panelShader, "TZebra", ThresholdU32( params[ PT_ZEBRA ] ) );
		panelShader.Set( "ZebraWidth", OptionIndex( params[ PT_ZEBRA_WIDTH ], kZebraWidthCount ) );
		panelShader.Set( "ZebraFloating", OptionIndex( params[ PT_ZEBRA_MODE ], 2 ) );
		setUint( panelShader, "TDeadRow", ThresholdU32( params[ PT_DEAD_ROWS ] ) );
		setUint( panelShader, "TColourLoss", ThresholdU32( params[ PT_COLOUR_LOSS ] ) );
		setUint( panelShader, "TShift", ThresholdU32( params[ PT_SHIFTED ] ) );
		setUint( panelShader, "TDeadLed", ThresholdU32( LedRateFromParam( params[ PT_DEAD_LEDS ] ) ) );
		setUint( panelShader, "TStuckLed", ThresholdU32( LedRateFromParam( params[ PT_STUCK_LEDS ] ) ) );
		setUint( panelShader, "TDeadTile", ThresholdU32( params[ PT_DEAD_TILES ] ) );
		setUint( panelShader, "TFlicker", ThresholdU32( params[ PT_FLICKER_TILES ] ) );
		setUint( panelShader, "FlickerBase", flickerSlot.base );
		panelShader.Set( "FlickerFrac", flickerSlot.fraction );
		panelShader.Set( "TileSpread", static_cast< float >( SpreadFromParam( params[ PT_TILE_SPREAD ] ) ) );
		panelShader.Set( "ModuleSpread", static_cast< float >( SpreadFromParam( params[ PT_MODULE_SPREAD ] ) ) );
		panelShader.Set( "ColourSpread", static_cast< float >( ColourSpreadFromParam( params[ PT_COLOUR_SPREAD ] ) ) );
		panelShader.Set( "Batches", IntegerOf( PT_BATCHES, params[ PT_BATCHES ] ) );
		panelShader.Set( "Seams", static_cast< float >( SeamFromParam( params[ PT_SEAMS ] ) ) );
		panelShader.Set( "Heat", static_cast< float >( HeatFromParam( params[ PT_HEAT ] ) ) );
		panelShader.Set( "PsuLimit", psuLimit );
		panelShader.Set( "PsuMode", psuMode );
		quad.Draw();
		unbindTextureUnits( 3 );
	}

	//---------------------------------------------------------------------
	// 7. The display, to the host.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
		ScopedShaderBinding shader( displayShader.GetGLID() );
		bindUnit( 0, panelPass.TextureID() );
		bindUnit( 1, input.Handle );
		displayShader.Set( "PanelTex", 0 );
		displayShader.Set( "InputTexture", 1 );
		setInt2( displayShader, "Size", width, height );
		displayShader.Set( "Pitch", pitch );
		displayShader.Set( "EmitHalf", static_cast< float >( 0.5 * std::sqrt( FillFromParam( params[ PT_FILL ] ) ) * pitch ) );
		displayShader.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
		quad.Draw();
		unbindTextureUnits( 2 );
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Patchwork::DeInitGL()
{
	for( FFGLShader* s : { &wallShader, &copyShader, &routeShader, &holdShader, &statsShader, &panelShader, &displayShader } )
		s->FreeGLResources();
	quad.Release();
	wallPass.Destroy();
	routePass.Destroy();
	holdPass[ 0 ].Destroy();
	holdPass[ 1 ].Destroy();
	statePass[ 0 ].Destroy();
	statePass[ 1 ].Destroy();
	panelPass.Destroy();
	releaseRing();
	if( emptyRing != 0 )
	{
		glDeleteTextures( 1, &emptyRing );
		emptyRing = 0;
	}
	holdValid = stateValid = false;
	layout                 = wall::Layout();
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Patchwork::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Patchwork::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT ? params[ index ] : 0.0f;
}

char* Patchwork::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Patchwork::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Patchwork::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}
void Patchwork::SetHooksForTest( int glslHooks )
{
	hooks = glslHooks;
}
void Patchwork::SetHeatEulerForTest( bool euler )
{
	heatEuler = euler;
}
void Patchwork::SetResizeKeepsStateForTest( bool keep )
{
	resizeKeepsState = keep;
}
