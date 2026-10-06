/**
    pwtest -- render Patchwork offline, and measure what the wall is doing.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic clock, in a headless CGL context. Most checks read
    the panel -- one RGBA16F texel per LED, top row first, read back from the
    plugin's own buffer -- so they measure in LEDs; --identity reads the
    host's output, so the display pass is covered too.

        pwtest --out /tmp/wall.png     the defaults on the harness's card
        pwtest --list                  every parameter and its default
        pwtest --pipe                  raw RGBA frames in on stdin, out on stdout
        pwtest --film N                N frames of the card, raw RGBA on stdout
        pwtest --offline               the checks that need no GL context (CI)

    `--script` is the fleet's cue format: `frame  Parameter Name  value`
    lines, held before the first key and after the last. A STANDARD (0..1)
    control is linear between its keys; an option, a boolean, an event or an
    integer STEPS.

    PWTEST_RENDERER=software asks for Apple's software renderer by id, on a
    Mac with a GPU: what a GPU-less CI runner falls back to.
*/

#include "Controls.h"
#include "Hash.h"
#include "Patchwork.h"
#include "Shaders.h"
#include "Wall.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace patchwork;

namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 4096 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	++g_checks;
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Tolerances, each with where it comes from.
//---------------------------------------------------------------------------
/// One half-float ULP just below 1.0 (2^-11), plus float32 slack for the
/// arithmetic before the store. The wall, the route, the hold and the panel
/// are RGBA16F, so a value in 0..1 is converted once on the way in and every
/// later copy of it is exact. Not HALF an ULP: a conversion may truncate
/// rather than round, and Apple's GPU measurably does (an identity error of
/// 0.000486 against a half-ULP of 0.000244).
constexpr double kHalfUlp = 1.0 / 2048.0 + 1e-6;
/// The heat state is RGBA32F and is updated once a frame: a float32's
/// relative rounding (6e-8) accumulated over at most 600 updates.
constexpr double kStateTol = 1e-4;

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written
/// top row first, which is the only place anything here flips.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = c == 3 ? 1.0f : rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The context.
//---------------------------------------------------------------------------
bool g_software = false;

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute fallback[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "PWTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		g_software = true;
		std::fprintf( stderr, "pwtest: PWTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( fallback, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_INTEGER: return "integer";
	default: return "other";
	}
}

/// A control whose value is a choice, a switch, a press or a count: cues
/// STEP between keys for these, and only a standard control ramps.
bool stepsBetweenCues( unsigned int type )
{
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT || type == FF_TYPE_INTEGER;
}

//---------------------------------------------------------------------------
// The cue sheet.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( std::istream& in, const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		char* end         = nullptr;
		const float value = std::strtof( parts.back().c_str(), &end );
		if( end == parts.back().c_str() || *end != '\0' )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": '" + parts.back() + "' is not a number";
			return {};
		}
		parts.pop_back();
		std::string name = parts.front();
		for( size_t i = 1; i < parts.size(); ++i )
			name += " " + parts[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
// Pictures. Every one is rows BOTTOM first (GL's order), as uploaded.
//---------------------------------------------------------------------------
double hash01( uint32_t a, uint32_t b = 0 )
{
	return Pcg( a * 2654435761u ^ Pcg( b + 0x9e3779b9u ) ) * ( 1.0 / 4294967296.0 );
}

/// The look card: a sunset sky, a sun, a skyline, a stripe of saturated bars
/// and a white title block -- enough edges, flats and colour for every fault
/// to have something to act on.
Floats buildCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	const double s = std::min( width, height );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 - 0.5 * width ) / s, v = ( y + 0.5 - 0.5 * height ) / s;
			//Sky: deep blue at the top to orange at the horizon.
			const double k = std::clamp( 0.5 + v, 0.0, 1.0 );
			double r = 0.95 - 0.8 * k, g = 0.45 - 0.3 * k, b = 0.25 + 0.5 * k;
			//The sun.
			const double d = std::sqrt( ( u + 0.25 ) * ( u + 0.25 ) + ( v + 0.02 ) * ( v + 0.02 ) );
			if( d < 0.2 )
			{
				r = 1.0;
				g = 0.85 - 0.5 * d;
				b = 0.35;
			}
			//The skyline.
			const int block = static_cast< int >( std::floor( ( u + 2.0 ) * 11.0 ) );
			const double top = -0.12 + 0.16 * hash01( static_cast< uint32_t >( block ), 7 );
			if( v < top )
			{
				r = g = b = 0.05;
				const int wx = static_cast< int >( std::floor( ( u + 2.0 ) * 70.0 ) ), wy = static_cast< int >( std::floor( v * 60.0 + 100.0 ) );
				if( ( wx % 3 ) != 0 && ( wy % 2 ) == 0 && hash01( static_cast< uint32_t >( wx * 131 + wy ), 9 ) > 0.55 )
				{
					r = 1.0;
					g = 0.9;
					b = 0.55;
				}
			}
			//Saturated bars along the bottom.
			if( v < -0.36 )
			{
				const int bar           = std::clamp( static_cast< int >( std::floor( ( x + 0.5 ) / width * 7.0 ) ), 0, 6 );
				const double bars[ 7 ][ 3 ] = { { 1, 1, 1 }, { 1, 1, 0 }, { 0, 1, 1 }, { 0, 1, 0 }, { 1, 0, 1 }, { 1, 0, 0 }, { 0, 0, 1 } };
				r = 0.75 * bars[ bar ][ 0 ];
				g = 0.75 * bars[ bar ][ 1 ];
				b = 0.75 * bars[ bar ][ 2 ];
			}
			//A white title block.
			if( std::fabs( u - 0.42 ) < 0.28 && std::fabs( v - 0.3 ) < 0.06 )
				r = g = b = 0.95;
			float* o = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			o[ 0 ]   = static_cast< float >( r );
			o[ 1 ]   = static_cast< float >( g );
			o[ 2 ]   = static_cast< float >( b );
			o[ 3 ]   = 1.0f;
		}
	return card;
}

Floats flatCard( int width, int height, float r, float g, float b )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < card.size(); i += 4 )
	{
		card[ i ]     = r;
		card[ i + 1 ] = g;
		card[ i + 2 ] = b;
		card[ i + 3 ] = 1.0f;
	}
	return card;
}

/// The card rolled `shift` pixels to the right, wrapping: --moving pans the
/// card three pixels a frame, so delays, lag and the hold have something to
/// show (on a still card every past frame is the same frame).
Floats pannedCard( const Floats& card, int width, int height, int shift )
{
	Floats out( card.size() );
	shift = ( ( shift % width ) + width ) % width;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
				out[ ( static_cast< size_t >( y ) * width + ( x + shift ) % width ) * 4 + c ] = card[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
	return out;
}

/// Eight-bit values from a hash of the pixel: every pixel different, and a
/// value that survives the half-float round trip to the same byte.
Floats noiseCard( int width, int height, uint32_t salt )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const uint32_t h = Hash3( Pack2( x, height - 1 - y ), salt, static_cast< uint32_t >( c ) );
				card[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ] = c == 3 ? 1.0f : static_cast< float >( h % 256u ) / 255.0f;
			}
	return card;
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format = GL_RGBA32F )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float output framebuffer, a synthetic clock.
//---------------------------------------------------------------------------
struct Rig
{
	Patchwork plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0, readFBO = 0;
	int frame          = 0;
	double fps         = 60.0;
	double clockOffset = 0.0;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Rig()
	{
		plugin.DeInitGL();
		release();
		if( readFBO )
			glDeleteFramebuffers( 1, &readFBO );
	}

	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool attach( int w, int h, const Floats* picture )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO   = outputFBO;
		const Floats card = picture ? *picture : buildCard( width, height );
		sourceTexture     = makeTexture( width, height, card.data() );
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		return true;
	}

	bool Init( int w, int h, const Floats* picture = nullptr )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/patchwork for which shader\n" );
			return false;
		}
		plugin.SetClockScaleForTest( 1.0 );
		return attach( w, h, picture );
	}

	/// The host's raster changes under a running instance: no InitGL.
	bool Resize( int w, int h, const Floats* picture = nullptr )
	{
		release();
		return attach( w, h, picture );
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			const double seconds = clockOffset + static_cast< double >( frame ) / fps;
			plugin.SetTime( seconds );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	Floats readTexture( GLuint texture, int w, int h )
	{
		Floats pixels( static_cast< size_t >( w ) * h * 4 );
		if( !readFBO )
			glGenFramebuffers( 1, &readFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, readFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, w, h, GL_RGBA, GL_FLOAT, pixels.data() );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		return pixels;
	}

	/// What every LED emits, ledsW x ledsH, row 0 at the TOP.
	Floats Panel()
	{
		const wall::Layout& l = plugin.CurrentLayout();
		return readTexture( plugin.PanelTextureID(), l.ledsW, l.ledsH );
	}

	/// Per tile ( heat, time since a trip or -1, mean drive, off ), row 0 at the top.
	Floats State()
	{
		const wall::Layout& l = plugin.CurrentLayout();
		return readTexture( plugin.StateTextureID(), l.cols, l.rows );
	}
};

/// A wall with nothing wrong with it: every fault, spread and process off,
/// one pixel per LED, Fill 1, Mix 1.
void perfect( Rig& rig )
{
	for( unsigned int id : { PT_TILE_SPREAD, PT_MODULE_SPREAD, PT_COLOUR_SPREAD, PT_SEAMS, PT_HEAT, PT_SWAPPED, PT_FLIPPED,
	                         PT_HOP_DELAY, PT_LAG_TILES, PT_CHAIN_BREAK, PT_INTERMITTENT, PT_DROPOUTS, PT_DEAD_TILES,
	                         PT_FLICKER_TILES, PT_DEAD_MODULES, PT_ZEBRA, PT_DEAD_ROWS, PT_COLOUR_LOSS, PT_SHIFTED,
	                         PT_DEAD_LEDS, PT_STUCK_LEDS } )
		rig.Set( id, 0.0f );
	rig.Set( PT_LED_PITCH, 1.0f );
	rig.Set( PT_FILL, 1.0f );
	rig.Set( PT_PSU_LIMIT, 1.0f );
	rig.Set( PT_REPEAT_TILES, 0.0f );
	rig.Set( PT_BATCHES, 0.0f );
	rig.Set( PT_MIX, 1.0f );
}

/// The panel's value at LED ( x, y ), y from the top.
const float* at( const Floats& panel, int ledsW, int x, int y )
{
	return &panel[ ( static_cast< size_t >( y ) * ledsW + x ) * 4 ];
}

/// The picture's value at ( x, y ), y from the TOP, from a bottom-up buffer.
const float* pixelTop( const Floats& picture, int width, int height, int x, int y )
{
	return &picture[ ( static_cast< size_t >( height - 1 - y ) * width + x ) * 4 ];
}

int byteOf( float v )
{
	return static_cast< int >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) );
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( Patchwork& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

/// `Name=Value`. The value is a number, or for an option the name of one of
/// its elements; anything else is refused, never read as 0 (polyhedral's
/// trap: strtof( "Fixed" ) is 0, silently).
bool applySetting( Patchwork& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			if( parameter.type == FF_TYPE_OPTION )
				for( unsigned int e = 0; e < plugin.GetNumParamElements( parameter.index ); ++e )
					if( value == plugin.GetParamElementName( parameter.index, e ) )
					{
						plugin.SetFloatParameter( parameter.index, static_cast< float >( e ) );
						return true;
					}
			char* end       = nullptr;
			const float got = std::strtof( value.c_str(), &end );
			if( value.empty() || end == value.c_str() || *end != '\0' )
			{
				error = "'" + value + "' is not a number" + ( parameter.type == FF_TYPE_OPTION ? " or an option of " + name : "" );
				return false;
			}
			plugin.SetFloatParameter( parameter.index, got );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

struct Cue
{
	Track track;
	bool ramp;
};

bool bindScript( Patchwork& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::map< std::string, Track > tracks = loadScript( file, path, error );
	if( !error.empty() )
		return false;
	const std::vector< NamedParameter > known = listParameters( plugin );
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first )
			{
				out[ parameter.index ] = Cue { entry.second, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a parameter (try --list)";
			return false;
		}
	}
	return true;
}

//===========================================================================
// --pipe and --film. Raw RGBA, top row first, on the synthetic clock.
//===========================================================================
/// `readStdin`: frames come in on stdin, one out per one in, until a partial
/// frame or EOF. Otherwise `count` frames of the card are made.
int runPipe( int width, int height, double fps, const std::string& scriptPath, int count, bool readStdin,
             const std::vector< std::string >& settings, bool moving )
{
	const Floats card = buildCard( width, height );
	Rig rig;
	rig.fps = fps;
	if( !rig.Init( width, height ) )
		return 1;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( rig.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}

	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; readStdin || index < count; ++index )
	{
		if( readStdin )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			//A partial frame is the end of the stream, never a frame.
			if( filled < in.size() )
			{
				if( filled > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes): dropped\n", filled, in.size() );
				break;
			}
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			rig.Upload( picture );
		}
		else if( moving )
			rig.Upload( pannedCard( card, width, height, 3 * index ) );

		for( const auto& cue : automation )
			rig.plugin.SetFloatParameter( cue.first, valueAt( cue.second.track, index, cue.second.ramp ) );
		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		Bytes bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >(
					std::lround( std::clamp( out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ], 0.0f, 1.0f ) * 255.0f ) );
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone. SIGPIPE is ignored in main(), so this is
			//EPIPE and not a silent 141: say so and stop.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}

//===========================================================================
// --bench: GPU time per frame from GL_TIME_ELAPSED (valve's lesson: a wall
// clock over identical frames measures the driver's queue, not the work).
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	struct Case
	{
		const char* what;
		std::function< void( Rig& ) > set;
	};
	const Case cases[] = {
		{ "defaults (LED Pitch 2)", []( Rig& ) {} },
		{ "LED Pitch 1, repeat, delays, hold", []( Rig& r ) {
			 r.Set( PT_LED_PITCH, 1.0f );
			 r.Set( PT_REPEAT_TILES, 3.0f );
			 r.Set( PT_HOP_DELAY, ParamFromHopDelay( 0.25 ) );
			 r.Set( PT_LAG_TILES, 0.1f );
			 r.Set( PT_LOST_SIGNAL, static_cast< float >( LostSignal::Hold ) );
		 } },
	};
	std::printf( "\n=== bench: GPU time per frame (GL_TIME_ELAPSED), after 60 frames of warm-up\n" );
	GLuint query = 0;
	glGenQueries( 1, &query );
	for( const Case& c : cases )
		for( const Size& size : sizes )
		{
			Rig rig;
			if( !rig.Init( size.w, size.h ) )
				return 1;
			c.set( rig );
			if( !rig.Render( 60 ) )
				return 1;
			constexpr int kTimed = 60;
			std::vector< double > times;
			for( int f = 0; f < kTimed; ++f )
			{
				glBeginQuery( GL_TIME_ELAPSED, query );
				if( !rig.Render( 1 ) )
					return 1;
				glEndQuery( GL_TIME_ELAPSED );
				GLuint64 ns = 0;
				glGetQueryObjectui64v( query, GL_QUERY_RESULT, &ns );
				times.push_back( static_cast< double >( ns ) * 1e-6 );
			}
			std::sort( times.begin(), times.end() );
			std::printf( "  %-36s %-6s median %6.2f ms/frame, worst %6.2f  (%4.1f%% of a 60 fps frame)\n", c.what, size.name,
			             times[ kTimed / 2 ], times.back(), 100.0 * times[ kTimed / 2 ] / ( 1000.0 / 60.0 ) );
		}
	glDeleteQueries( 1, &query );
	return 0;
}

//===========================================================================
// The checks.
//
// Each takes a Perturb. With every field at its default the check scores the
// plugin against the stated law; `--negative` sets one field at a time to a
// deliberately wrong MODEL -- a hook in the plugin, so the shipped shaders
// compute the wrong thing -- and requires the check to FAIL.
//===========================================================================
struct Perturb
{
	int hooks          = 0;    ///< shaders::Hook bits
	bool heatEuler     = false;///< --heat: alpha = dt / tau
	bool resizeKeeps   = false;///< --resize: the hold survives a reallocation
	bool lawRaster     = false;///< --chain-law: the snake's claims made of the raster routes
	bool motionAbsolute= false;///< --motion-law: phase = speed x elapsed, not integrated
	bool cuesRamp      = false;///< --cues: every control ramps
};

using CheckFn = int ( * )( const Perturb& );

struct Raster
{
	int w, h;
};
std::vector< Raster > kRasters = { { 1280, 720 }, { 320, 180 } };

bool startRig( Rig& rig, const Raster& raster, const Floats& picture, const Perturb& perturb )
{
	if( !rig.Init( raster.w, raster.h, &picture ) )
		return false;
	rig.plugin.SetHooksForTest( perturb.hooks );
	rig.plugin.SetHeatEulerForTest( perturb.heatEuler );
	rig.plugin.SetResizeKeepsStateForTest( perturb.resizeKeeps );
	perfect( rig );
	return true;
}

/// Tile sizes that give a wall of about 10 x 6 tiles at any raster, at one
/// pixel per LED, deliberately not dividing the raster.
void smallTiles( Rig& rig, const Raster& raster )
{
	rig.Set( PT_TILE_W, static_cast< float >( std::max( 4, raster.w / 10 + 3 ) ) );
	rig.Set( PT_TILE_H, static_cast< float >( std::max( 4, raster.h / 6 + 1 ) ) );
}

//===========================================================================
// --identity: a wall with nothing wrong is the clip.
//===========================================================================
int runIdentity( const Perturb& perturb )
{
	std::printf( "\n=== identity: a perfect wall is the clip; at LED Pitch p each LED is the mean of its p x p pixels\n" );
	for( const Raster& raster : kRasters )
	{
		const Floats card = noiseCard( raster.w, raster.h, 11 );
		Rig rig;
		if( !startRig( rig, raster, card, perturb ) )
			return 1;
		rig.Set( PT_MODULES_X, 3.0f );
		if( !rig.Render( 2 ) )
			return 1;
		const Floats out = rig.Output();
		double worst     = 0.0;
		int bytesWrong   = 0;
		for( size_t i = 0; i < out.size(); ++i )
		{
			worst = std::max( worst, static_cast< double >( std::fabs( out[ i ] - card[ i ] ) ) );
			bytesWrong += byteOf( out[ i ] ) != byteOf( card[ i ] );
		}
		Check( worst <= kHalfUlp && bytesWrong == 0,
		       fmt( "%dx%d, LED Pitch 1: worst |out - clip| %.3g (one half-float ULP is %.3g); %d of %zu bytes differ", raster.w, raster.h,
		            worst, kHalfUlp, bytesWrong, out.size() ) );

		//Pitch 3 does not divide 320 or 1280: the last LED column covers two
		//pixels, and is the mean of those two.
		for( int pitch : { 2, 3 } )
		{
			rig.Set( PT_LED_PITCH, static_cast< float >( pitch ) );
			if( !rig.Render( 1 ) )
				return 1;
			const Floats got = rig.Output();
			double err       = 0.0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					const int lx = x / pitch, ly = y / pitch;
					for( int c = 0; c < 3; ++c )
					{
						double sum = 0.0;
						int n      = 0;
						for( int yy = ly * pitch; yy < std::min( ly * pitch + pitch, raster.h ); ++yy )
							for( int xx = lx * pitch; xx < std::min( lx * pitch + pitch, raster.w ); ++xx )
							{
								sum += pixelTop( card, raster.w, raster.h, xx, yy )[ c ];
								++n;
							}
						err = std::max( err, std::fabs( pixelTop( got, raster.w, raster.h, x, y )[ c ] - sum / n ) );
					}
				}
			Check( err <= kHalfUlp, fmt( "%dx%d, LED Pitch %d: every pixel is its LED's box mean to %.3g (bound %.3g)", raster.w, raster.h,
			                             pitch, err, kHalfUlp ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --tiles: the calibration is constant over a tile (a module) and changes
// only on its boundary.
//===========================================================================
int runTiles( const Perturb& perturb )
{
	std::printf( "\n=== tiles: Tile Spread is constant inside a tile and changes on tile boundaries; Module Spread on module boundaries\n" );
	for( const Raster& raster : kRasters )
		for( bool modules : { false, true } )
		{
			const Floats card = flatCard( raster.w, raster.h, 0.6f, 0.6f, 0.6f );
			Rig rig;
			if( !startRig( rig, raster, card, perturb ) )
				return 1;
			smallTiles( rig, raster );
			rig.Set( modules ? PT_MODULE_SPREAD : PT_TILE_SPREAD, 1.0f );
			rig.Set( PT_MODULES_X, 3.0f );
			rig.Set( PT_MODULES_Y, 2.0f );
			if( !rig.Render( 1 ) )
				return 1;
			const Floats panel    = rig.Panel();
			const wall::Layout& l = rig.plugin.CurrentLayout();
			//Which cell (tile or module) an LED is in, from the geometry alone.
			auto cellOf = [ & ]( int x, int y ) {
				const int tx = x / l.tileW, ty = y / l.tileH;
				if( !modules )
					return std::make_pair( tx, ty );
				const int lx = x - tx * l.tileW, ly = y - ty * l.tileH;
				const int mx = ( ( lx + 1 ) * 3 - 1 ) / l.tileW, my = ( ( ly + 1 ) * 2 - 1 ) / l.tileH;
				return std::make_pair( tx * 3 + mx, ty * 2 + my );
			};
			int insideChanges = 0, boundaries = 0, boundaryEqual = 0;
			for( int y = 0; y < l.ledsH; ++y )
				for( int x = 0; x < l.ledsW; ++x )
					for( int d = 0; d < 2; ++d )
					{
						const int nx = x + ( d == 0 ? 1 : 0 ), ny = y + ( d == 1 ? 1 : 0 );
						if( nx >= l.ledsW || ny >= l.ledsH )
							continue;
						const float a = at( panel, l.ledsW, x, y )[ 0 ], b = at( panel, l.ledsW, nx, ny )[ 0 ];
						if( cellOf( x, y ) == cellOf( nx, ny ) )
							insideChanges += a != b;
						else
						{
							++boundaries;
							boundaryEqual += a == b;
						}
					}
			Check( insideChanges == 0 && boundaries > 0 && boundaryEqual * 100 <= boundaries,
			       fmt( "%dx%d %s Spread 1, %dx%d LED tiles, 3x2 modules: %d changes inside a %s; %d of %d neighbouring pairs across "
			            "a boundary equal (at most 1%%)",
			            raster.w, raster.h, modules ? "Module" : "Tile", l.tileW, l.tileH, insideChanges, modules ? "module" : "tile",
			            boundaryEqual, boundaries ) );
		}
	return Verdict();
}

//===========================================================================
// --batches: Batches K -> at most K calibrations across the wall.
//===========================================================================
int runBatches( const Perturb& perturb )
{
	std::printf( "\n=== batches: K batches give at most K distinct tile colours (K exactly, at these seeds); 0 gives every tile its own\n" );
	for( const Raster& raster : kRasters )
		for( int k : { 0, 1, 3, 5 } )
		{
			const Floats card = flatCard( raster.w, raster.h, 0.6f, 0.6f, 0.6f );
			Rig rig;
			if( !startRig( rig, raster, card, perturb ) )
				return 1;
			smallTiles( rig, raster );
			rig.Set( PT_TILE_SPREAD, 1.0f );
			rig.Set( PT_COLOUR_SPREAD, 1.0f );
			rig.Set( PT_BATCHES, static_cast< float >( k ) );
			if( !rig.Render( 1 ) )
				return 1;
			const Floats panel    = rig.Panel();
			const wall::Layout& l = rig.plugin.CurrentLayout();
			std::set< std::vector< float > > colours;
			for( int ty = 0; ty < l.rows; ++ty )
				for( int tx = 0; tx < l.cols; ++tx )
				{
					const float* p = at( panel, l.ledsW, tx * l.tileW, ty * l.tileH );
					colours.insert( { p[ 0 ], p[ 1 ], p[ 2 ] } );
				}
			const int want = k == 0 ? l.Tiles() : k;
			Check( static_cast< int >( colours.size() ) == want,
			       fmt( "%dx%d, %d tiles, Batches %d: %zu distinct tile colours (want %d)", raster.w, raster.h, l.Tiles(), k, colours.size(), want ) );
		}
	return Verdict();
}

//===========================================================================
// --zebra: a stuck address line moves light, it does not remove it.
//===========================================================================
int runZebra( const Perturb& perturb )
{
	std::printf( "\n=== zebra: bit k stuck -> rows of one parity of bit k black, the others doubled (period 2^(k+1)); module mean kept\n" );
	for( const Raster& raster : kRasters )
	{
		//32 x 32 LED modules (one per tile) at 1/16 scan.
		const int tile = raster.w >= 640 ? 64 : 32;
		struct Case
		{
			int scanOption;// 1/2^option
			int widthOption;
			int expectBit;// -1: no stripes at all
		};
		const Case cases[] = { { 4, 1, 0 }, { 4, 2, 1 }, { 4, 3, 2 }, { 4, 4, 3 }, { 2, 4, 1 }, { 0, 3, -1 } };
		for( const Case& c : cases )
			for( bool ramp : { false, true } )
			{
				//A ramp down each module: row r of a module is 0.05 + 0.4 r / rows,
				//so a doubled row never clips.
				Floats card( static_cast< size_t >( raster.w ) * raster.h * 4 );
				for( int y = 0; y < raster.h; ++y )
					for( int x = 0; x < raster.w; ++x )
					{
						const int r    = y % tile;
						const float v  = ramp ? 0.05f + 0.4f * static_cast< float >( r ) / static_cast< float >( tile ) : 0.3f;
						float* o       = &card[ ( static_cast< size_t >( raster.h - 1 - y ) * raster.w + x ) * 4 ];
						o[ 0 ] = o[ 1 ] = o[ 2 ] = v;
						o[ 3 ]                   = 1.0f;
					}
				Rig rig;
				if( !startRig( rig, raster, card, perturb ) )
					return 1;
				rig.Set( PT_TILE_W, static_cast< float >( tile ) );
				rig.Set( PT_TILE_H, static_cast< float >( tile ) );
				rig.Set( PT_MODULES_X, 1.0f );
				rig.Set( PT_MODULES_Y, 1.0f );
				rig.Set( PT_SCAN, static_cast< float >( c.scanOption ) );
				rig.Set( PT_ZEBRA, 1.0f );
				rig.Set( PT_ZEBRA_WIDTH, static_cast< float >( c.widthOption ) );
				if( !rig.Render( 1 ) )
					return 1;
				const Floats panel    = rig.Panel();
				const wall::Layout& l = rig.plugin.CurrentLayout();
				const int scan        = 1 << c.scanOption;
				auto source           = [ & ]( int r ) { return ramp ? 0.05 + 0.4 * r / tile : 0.3; };
				double worst = 0.0, worstMean = 0.0;
				int modulesSeen = 0;
				//Whole modules only.
				for( int ty = 0; ty + 1 <= l.ledsH / tile; ++ty )
					for( int tx = 0; tx + 1 <= l.ledsW / tile; ++tx )
					{
						++modulesSeen;
						//Which value the line is stuck at, from row 0's brightness.
						const bool row0Lit = at( panel, l.ledsW, tx * tile, ty * tile )[ 0 ] > 0.0f;
						double sum = 0.0, want = 0.0;
						for( int r = 0; r < tile; ++r )
						{
							double expect = source( r );
							if( c.expectBit >= 0 )
							{
								const int bit = 1 << c.expectBit;
								const bool lit = ( ( ( r % scan ) & bit ) != 0 ) == !row0Lit;
								expect         = lit ? source( r ) + source( r ^ bit ) : 0.0;
							}
							for( int x = 0; x < tile; ++x )
							{
								const double got = at( panel, l.ledsW, tx * tile + x, ty * tile + r )[ 1 ];
								worst            = std::max( worst, std::fabs( got - expect ) );
								sum += got;
							}
							want += source( r ) * tile;
						}
						worstMean = std::max( worstMean, std::fabs( sum - want ) / ( tile * tile ) );
					}
				Check( worst <= 2.0 * kHalfUlp && worstMean <= 2.0 * kHalfUlp && modulesSeen > 0,
				       fmt( "%dx%d %s, scan 1/%d, Zebra Width option %d -> %s: %d modules, worst row %.3g, worst module-mean error %.3g "
				            "(bound two half-float ULPs, %.3g: a doubled row is a sum of two)",
				            raster.w, raster.h, ramp ? "ramp" : "flat 0.3", scan, c.widthOption,
				            c.expectBit < 0 ? "no address lines, no stripes" : fmt( "bit %d, stripes of %d rows", c.expectBit, 1 << c.expectBit ).c_str(),
				            modulesSeen, worst, worstMean, 2.0 * kHalfUlp ) );
			}
	}
	return Verdict();
}

//===========================================================================
// --chain: the cable, measured out of the picture.
//===========================================================================
/// The frame each tile first turns white after a cut from black at frame
/// `cut`, Hop Delay 1: tile -> arrival frame. -1 if it never did.
std::vector< int > arrivals( Rig& rig, int cut, int frames )
{
	const Floats black = flatCard( rig.width, rig.height, 0.0f, 0.0f, 0.0f );
	const Floats white = flatCard( rig.width, rig.height, 1.0f, 1.0f, 1.0f );
	std::vector< int > first;
	for( int f = 0; f < frames; ++f )
	{
		rig.Upload( f < cut ? black : white );
		if( !rig.Render( 1 ) )
			return {};
		const Floats panel    = rig.Panel();
		const wall::Layout& l = rig.plugin.CurrentLayout();
		first.resize( static_cast< size_t >( l.Tiles() ), -1 );
		for( int ty = 0; ty < l.rows; ++ty )
			for( int tx = 0; tx < l.cols; ++tx )
			{
				const int i = ty * l.cols + tx;
				if( first[ static_cast< size_t >( i ) ] < 0 && at( panel, l.ledsW, tx * l.tileW, ty * l.tileH )[ 0 ] > 0.5f )
					first[ static_cast< size_t >( i ) ] = f;
			}
	}
	return first;
}

int runChain( const Perturb& perturb )
{
	std::printf( "\n=== chain: a cut arrives at chain position i on frame cut + i (Hop Delay 1); the order is a snake; a break loses a suffix\n" );
	for( const Raster& raster : kRasters )
	{
		const Floats black = flatCard( raster.w, raster.h, 0.0f, 0.0f, 0.0f );
		//5 x 4 tiles: 20 hops, inside any ring this raster allows.
		const int tw = ( raster.w + 4 ) / 5, th = ( raster.h + 3 ) / 4;
		//Apple's software renderer takes ~9 s a configuration here: two
		//opposite corners there cover every flip, and the skip is printed.
		if( g_software )
			std::printf( "  skip  software renderer: Top Right and Bottom Left corners (the GPU pass runs all four)\n" );
		for( int route = 0; route < static_cast< int >( Route::Count ); ++route )
			for( int corner = 0; corner < static_cast< int >( Corner::Count ); ++corner )
				for( int perPort : { 0, 6 } )
				{
					if( g_software && ( corner == static_cast< int >( Corner::TopRight ) || corner == static_cast< int >( Corner::BottomLeft ) ) )
						continue;
					Rig rig;
					if( !startRig( rig, raster, black, perturb ) )
						return 1;
					rig.Set( PT_TILE_W, static_cast< float >( tw ) );
					rig.Set( PT_TILE_H, static_cast< float >( th ) );
					rig.Set( PT_ROUTE, static_cast< float >( route ) );
					rig.Set( PT_CORNER, static_cast< float >( corner ) );
					rig.Set( PT_PER_PORT, static_cast< float >( perPort ) );
					rig.Set( PT_HOP_DELAY, ParamFromHopDelay( 1.0 ) );
					//Twenty hops at one frame each: the ring must hold 20 frames of
					//black before the cut, and the last tile turns white 19 after.
					constexpr int kCut = 22;
					const std::vector< int > first = arrivals( rig, kCut, kCut + 21 );
					const wall::Layout& l          = rig.plugin.CurrentLayout();
					int wrong = 0;
					//Sorted by port, then by arrival: the order the cable measured.
					std::vector< std::pair< int, int > > order;
					for( int ty = 0; ty < l.rows; ++ty )
						for( int tx = 0; tx < l.cols; ++tx )
						{
							const wall::Link link = wall::LinkOf( l, tx, ty );
							const int got         = first[ static_cast< size_t >( ty * l.cols + tx ) ];
							wrong += got != kCut + link.pos;
							order.push_back( { link.port * 100000 + got, ty * l.cols + tx } );
						}
					//The measured order of each port's tiles: consecutive arrivals
					//adjacent (snakes), or adjacent within a row/column (rasters).
					std::sort( order.begin(), order.end() );
					int jumps = 0;
					for( size_t i = 1; i < order.size(); ++i )
					{
						if( order[ i ].first / 100000 != order[ i - 1 ].first / 100000 )
							continue;
						const int a = order[ i - 1 ].second, b = order[ i ].second;
						const int d = std::abs( a % l.cols - b % l.cols ) + std::abs( a / l.cols - b / l.cols );
						jumps += d != 1;
					}
					const bool snake   = route == static_cast< int >( Route::RowSnake ) || route == static_cast< int >( Route::ColumnSnake );
					const int lineEnds = ( route == static_cast< int >( Route::Rows ) ? l.rows : l.cols ) - 1;
					//A port boundary inside a line is not a jump; a raster's line
					//end is, unless a port starts there.
					const bool orderOk = snake ? jumps == 0 : jumps <= lineEnds;
					Check( wrong == 0 && orderOk,
					       fmt( "%dx%d %-12s from %-12s %s: %d tiles, %d arrive off frame cut+pos; %d non-adjacent steps (%s)", raster.w,
					            raster.h, InfoOf( PT_ROUTE ).options[ route ], InfoOf( PT_CORNER ).options[ corner ],
					            perPort ? "6 per port" : "one chain ", l.Tiles(), wrong, jumps, snake ? "a snake has none" : "a raster: one per line end at most" ) );
				}

		//A break: on every port, the lost tiles are a suffix of the chain.
		for( int seed : { 1, 2, 3 } )
		{
			const Floats white = flatCard( raster.w, raster.h, 1.0f, 1.0f, 1.0f );
			Rig rig;
			if( !startRig( rig, raster, white, perturb ) )
				return 1;
			rig.Set( PT_TILE_W, static_cast< float >( tw ) );
			rig.Set( PT_TILE_H, static_cast< float >( th ) );
			rig.Set( PT_PER_PORT, 6.0f );
			rig.Set( PT_CHAIN_BREAK, 1.0f );
			rig.Set( PT_SEED, static_cast< float >( seed ) );
			if( !rig.Render( 2 ) )
				return 1;
			const Floats panel    = rig.Panel();
			const wall::Layout& l = rig.plugin.CurrentLayout();
			std::map< int, std::vector< bool > > ports;
			for( int ty = 0; ty < l.rows; ++ty )
				for( int tx = 0; tx < l.cols; ++tx )
				{
					const wall::Link link = wall::LinkOf( l, tx, ty );
					auto& lost            = ports[ link.port ];
					lost.resize( static_cast< size_t >( link.length ), false );
					lost[ static_cast< size_t >( link.pos ) ] = at( panel, l.ledsW, tx * l.tileW + 1, ty * l.tileH + 1 )[ 0 ] < 0.5f;
				}
			int notSuffix = 0, emptyPorts = 0, lostTotal = 0;
			for( const auto& port : ports )
			{
				bool seen = false;
				int n     = 0;
				for( bool lost : port.second )
				{
					notSuffix += seen && !lost;
					seen = seen || lost;
					n += lost;
				}
				emptyPorts += n == 0;
				lostTotal += n;
			}
			Check( notSuffix == 0 && emptyPorts == 0,
			       fmt( "%dx%d Chain Break 1, seed %d, %zu ports: %d tiles lost; %d live tiles downstream of a lost one; %d ports with nothing lost",
			            raster.w, raster.h, seed, ports.size(), lostTotal, notSuffix, emptyPorts ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --repeat: N tiles' content along the cable, the right way round.
//===========================================================================
int runRepeat( const Perturb& perturb )
{
	std::printf( "\n=== repeat: tile p shows window tile s0 + ((p - s0 - j) mod N) translated f LEDs along its own cable direction "
	             "(m = jL + f), never mirrored; outside the region, itself\n" );
	struct Case
	{
		Route route;
		Corner corner;
		int perPort, n;
		float from, reach;
		Motion motion;
		double speed;
		int frames;
	};
	const Case cases[] = {
		{ Route::RowSnake, Corner::TopLeft, 0, 3, 0.0f, 1.0f, Motion::Hold, 0.0, 2 },
		{ Route::RowSnake, Corner::TopLeft, 0, 3, 0.0f, 1.0f, Motion::Step, 2.0, 61 },
		{ Route::RowSnake, Corner::TopLeft, 0, 2, 0.0f, 1.0f, Motion::Scroll, 0.75, 45 },
		{ Route::ColumnSnake, Corner::BottomRight, 7, 4, 0.3f, 0.5f, Motion::Scroll, -1.5, 37 },
		{ Route::Rows, Corner::TopRight, 9, 3, 0.5f, 1.0f, Motion::PingPong, 2.5, 70 },
		{ Route::Columns, Corner::BottomLeft, 0, 5, 0.2f, 0.3f, Motion::Random, 3.0, 50 },
	};
	for( const Raster& raster : kRasters )
		for( const Case& c : cases )
		{
			const Floats card = noiseCard( raster.w, raster.h, 23 );
			Rig rig;
			if( !startRig( rig, raster, card, perturb ) )
				return 1;
			smallTiles( rig, raster );
			rig.Set( PT_ROUTE, static_cast< float >( c.route ) );
			rig.Set( PT_CORNER, static_cast< float >( c.corner ) );
			rig.Set( PT_PER_PORT, static_cast< float >( c.perPort ) );
			rig.Set( PT_REPEAT_TILES, static_cast< float >( c.n ) );
			rig.Set( PT_REPEAT_FROM, c.from );
			rig.Set( PT_REPEAT_REACH, c.reach );
			rig.Set( PT_REPEAT_MOTION, static_cast< float >( c.motion ) );
			rig.Set( PT_REPEAT_SPEED, ParamFromRepeatSpeed( c.speed ) );
			if( !rig.Render( c.frames ) )
				return 1;
			const Floats panel    = rig.Panel();
			const wall::Layout& l = rig.plugin.CurrentLayout();
			const int L           = l.AlongLength();
			const bool alongX     = l.AlongX();

			//The offset the plugin's phase implies, by motion. Ping-Pong and
			//Random are found instead: the picture must be SOME whole-LED offset
			//of the window, the one that matches; --motion-law checks which.
			std::vector< int > offsets;
			const double phase = rig.plugin.RepeatPhase();
			const int window   = std::min( c.n, l.perPort );
			switch( c.motion )
			{
			case Motion::Hold: offsets = { 0 }; break;
			case Motion::Step: offsets = { static_cast< int >( ( static_cast< long long >( std::floor( phase ) ) * L % ( window * L ) + window * L ) % ( window * L ) ) }; break;
			case Motion::Scroll: offsets = { static_cast< int >( ( static_cast< long long >( std::floor( phase * L ) ) % ( window * L ) + window * L ) % ( window * L ) ) }; break;
			default:
				for( int m = 0; m < window * L; ++m )
					offsets.push_back( m );
			}

			//Tile -> its chain, in chain order, per port.
			std::map< int, std::vector< std::pair< int, int > > > chains;
			for( int linear = 0; linear < l.Tiles(); ++linear )
			{
				int tx = 0, ty = 0;
				wall::TileAt( l, linear, tx, ty );
				chains[ linear / l.perPort ].push_back( { tx, ty } );
			}

			int bestWrong = -1, bestM = -1;
			for( int m : offsets )
			{
				int wrong = 0;
				for( const auto& chain : chains )
				{
					const int n     = static_cast< int >( chain.second.size() );
					const int N     = std::min( c.n, n );
					const int mLocal = m % ( N * L );
					const int s0    = std::clamp( static_cast< int >( std::floor( c.from * n ) ), 0, n - N );
					const int end   = s0 + N + static_cast< int >( std::floor( c.reach * ( n - s0 - N ) + 0.5 ) );
					const int j = mLocal / L, f = mLocal % L;
					for( int p = 0; p < n; ++p )
					{
						const int tx = chain.second[ static_cast< size_t >( p ) ].first, ty = chain.second[ static_cast< size_t >( p ) ].second;
						const int dir = wall::TravelOf( l, tx, ty );
						for( int ly = 0; ly < l.tileH; ++ly )
							for( int lx = 0; lx < l.tileW; ++lx )
							{
								const int x = tx * l.tileW + lx, y = ty * l.tileH + ly;
								if( x >= l.ledsW || y >= l.ledsH )
									continue;
								int sx = x, sy = y;
								if( p >= s0 && p < end )
								{
									//Translate f LEDs along this tile's direction of travel;
									//what falls off the upstream end comes from the window
									//tile before.
									const int a  = alongX ? lx : ly;
									int a2       = a - dir * f;
									int k        = ( ( p - s0 - j ) % N + N ) % N;
									if( a2 < 0 || a2 >= L )
									{
										a2 += dir * L;
										k = ( k - 1 + N ) % N;
									}
									const auto& src = chain.second[ static_cast< size_t >( s0 + k ) ];
									sx              = src.first * l.tileW + ( alongX ? a2 : lx );
									sy              = src.second * l.tileH + ( alongX ? ly : a2 );
								}
								const bool offWall = sx >= l.ledsW || sy >= l.ledsH;
								for( int ch = 0; ch < 3; ++ch )
								{
									const int want = offWall ? 0 : byteOf( pixelTop( card, raster.w, raster.h, sx, sy )[ ch ] );
									wrong += byteOf( at( panel, l.ledsW, x, y )[ ch ] ) != want;
								}
							}
					}
				}
				if( bestWrong < 0 || wrong < bestWrong )
				{
					bestWrong = wrong;
					bestM     = m;
				}
				if( wrong == 0 )
					break;
			}
			Check( bestWrong == 0,
			       fmt( "%dx%d %-12s from %-12s port %d, N %d, From %.1f, Reach %.1f, %-9s %+.2f tiles/s, %d frames: m = %d LEDs (L %d): "
			            "%d channel bytes wrong",
			            raster.w, raster.h, InfoOf( PT_ROUTE ).options[ static_cast< int >( c.route ) ],
			            InfoOf( PT_CORNER ).options[ static_cast< int >( c.corner ) ], c.perPort, c.n, c.from, c.reach,
			            InfoOf( PT_REPEAT_MOTION ).options[ static_cast< int >( c.motion ) ], c.speed, c.frames, bestM, L, bestWrong ) );
		}

	//Measured out of the picture: on a row snake, scrolling content moves
	//right along row 0 and left along row 1.
	for( const Raster& raster : kRasters )
	{
		const Floats card = noiseCard( raster.w, raster.h, 29 );
		Rig rig;
		if( !startRig( rig, raster, card, perturb ) )
			return 1;
		smallTiles( rig, raster );
		rig.Set( PT_REPEAT_TILES, 2.0f );
		rig.Set( PT_REPEAT_MOTION, static_cast< float >( Motion::Scroll ) );
		rig.Set( PT_REPEAT_SPEED, ParamFromRepeatSpeed( 1.0 ) );
		if( !rig.Render( 20 ) )
			return 1;
		const Floats a = rig.Panel();
		if( !rig.Render( 6 ) )
			return 1;
		const Floats b        = rig.Panel();
		const wall::Layout& l = rig.plugin.CurrentLayout();
		//The shift s that maps row r of tile (1, ty)'s interior in frame a onto
		//frame b: b(x) = a(x - s).
		auto shiftIn = [ & ]( int ty ) {
			int best = 0, bestWrong = -1;
			const int y = ty * l.tileH + l.tileH / 2;
			for( int s = -l.tileW / 2; s <= l.tileW / 2; ++s )
			{
				int wrong = 0;
				for( int lx = l.tileW / 4; lx < 3 * l.tileW / 4; ++lx )
				{
					const int x = l.tileW + lx;
					wrong += byteOf( at( b, l.ledsW, x, y )[ 0 ] ) != byteOf( at( a, l.ledsW, x - s, y )[ 0 ] );
				}
				if( bestWrong < 0 || wrong < bestWrong )
				{
					bestWrong = wrong;
					best      = s;
				}
			}
			return best;
		};
		const int s0 = shiftIn( 0 ), s1 = shiftIn( 1 );
		Check( s0 > 0 && s1 < 0 && s0 == -s1,
		       fmt( "%dx%d row snake, Scroll at 1 tile/s, 6 frames: row 0 moved %+d LEDs, row 1 %+d (opposite ways, the same speed)",
		            raster.w, raster.h, s0, s1 ) );
	}
	return Verdict();
}

//===========================================================================
// --hold: a lost card shows the last frame it received.
//===========================================================================
int runHold( const Perturb& perturb )
{
	std::printf( "\n=== hold: under Lost Signal Hold a lost tile shows the frame it last received; which tiles are lost is measured\n"
	             "    from a twin rig under Lost Signal Black, never re-derived\n" );
	for( const Raster& raster : kRasters )
	{
		auto level     = []( int f ) { return static_cast< float >( 30 + ( f * 7 ) % 200 ) / 255.0f; };
		Floats picture = flatCard( raster.w, raster.h, level( 0 ), level( 0 ), level( 0 ) );
		Rig held, black;
		if( !startRig( held, raster, picture, perturb ) || !startRig( black, raster, picture, perturb ) )
			return 1;
		for( Rig* r : { &held, &black } )
		{
			smallTiles( *r, raster );
			r->Set( PT_DROPOUTS, 0.35f );
			r->Set( PT_DROPOUT_TIME, ParamFromDropoutTime( 0.1 ) );
		}
		held.Set( PT_LOST_SIGNAL, static_cast< float >( LostSignal::Hold ) );
		black.Set( PT_LOST_SIGNAL, static_cast< float >( LostSignal::Black ) );
		std::vector< int > lastLive;
		int wrong = 0, lostFrames = 0, longest = 0;
		std::vector< int > run;
		for( int f = 0; f < 90; ++f )
		{
			picture = flatCard( raster.w, raster.h, level( f ), level( f ), level( f ) );
			held.Upload( picture );
			black.Upload( picture );
			if( !held.Render( 1 ) || !black.Render( 1 ) )
				return 1;
			const Floats a = held.Panel(), b = black.Panel();
			const wall::Layout& l = held.plugin.CurrentLayout();
			lastLive.resize( static_cast< size_t >( l.Tiles() ), 0 );
			run.resize( static_cast< size_t >( l.Tiles() ), 0 );
			for( int ty = 0; ty < l.rows; ++ty )
				for( int tx = 0; tx < l.cols; ++tx )
				{
					const size_t i = static_cast< size_t >( ty * l.cols + tx );
					const int x = tx * l.tileW, y = ty * l.tileH;
					//Frame 0 has no history: every card has just received its first frame.
					const bool lost = f > 0 && at( b, l.ledsW, x, y )[ 0 ] == 0.0f;
					const int want  = byteOf( level( lost ? lastLive[ i ] : f ) );
					wrong += byteOf( at( a, l.ledsW, x, y )[ 0 ] ) != want;
					if( lost )
					{
						++lostFrames;
						longest = std::max( longest, ++run[ i ] );
					}
					else
					{
						lastLive[ i ] = f;
						run[ i ]      = 0;
					}
				}
		}
		Check( wrong == 0 && lostFrames > 20 && longest >= 2,
		       fmt( "%dx%d, 90 frames each a different level: %d tile-frames lost (longest run %d frames), %d showing the wrong frame",
		            raster.w, raster.h, lostFrames, longest, wrong ) );
	}
	return Verdict();
}

//===========================================================================
// --heat: the one-pole, and red first.
//===========================================================================
int runHeat( const Perturb& perturb )
{
	std::printf( "\n=== heat: H after n updates is 1 - exp(-n dt / tau) heating and H0 exp(-n dt / tau) cooling; droop red:green:blue 1 : 0.3 : 0.15\n" );
	for( const Raster& raster : kRasters )
	{
		const Floats white = flatCard( raster.w, raster.h, 1.0f, 1.0f, 1.0f );
		const Floats black = flatCard( raster.w, raster.h, 0.0f, 0.0f, 0.0f );
		Rig rig;
		if( !startRig( rig, raster, white, perturb ) )
			return 1;
		smallTiles( rig, raster );
		const double tau = 2.0, dt = 1.0 / rig.fps, droop = 0.5;
		rig.Set( PT_HEAT, 1.0f );
		rig.Set( PT_HEAT_TIME, ParamFromHeatTime( tau ) );
		const double tauGot = HeatTimeFromParam( ParamFromHeatTime( tau ) );
		//The first frame has no previous one: dt 0, no heating.
		double worstState = 0.0, worstRatio = 0.0;
		int updates       = 0;
		for( int step : { 1, 30, 90, 120 } )
		{
			if( !rig.Render( step ) )
				return 1;
			updates += step;
			const double expect = 1.0 - std::exp( -( updates - 1 ) * dt / tauGot );
			const Floats state  = rig.State();
			worstState          = std::max( worstState, std::fabs( state[ 0 ] - expect ) );
			const Floats panel  = rig.Panel();
			const float* p      = at( panel, rig.plugin.CurrentLayout().ledsW, 2, 2 );
			const double want[ 3 ] = { 1.0 - droop * expect, 1.0 - 0.3 * droop * expect, 1.0 - 0.15 * droop * expect };
			for( int c = 0; c < 3; ++c )
				worstRatio = std::max( worstRatio, std::fabs( p[ c ] - want[ c ] ) );
		}
		Check( worstState <= kStateTol && worstRatio <= kHalfUlp + kStateTol,
		       fmt( "%dx%d heating on white, tau %.3f s, 241 frames: worst |H - (1 - e^(-t/tau))| %.3g (bound %.0e); worst droop %.3g",
		            raster.w, raster.h, tauGot, worstState, kStateTol, worstRatio ) );

		const double h0 = rig.State()[ 0 ];
		rig.Upload( black );
		double worstCool = 0.0;
		int cooled       = 0;
		for( int step : { 1, 60, 120 } )
		{
			if( !rig.Render( step ) )
				return 1;
			cooled += step;
			worstCool = std::max( worstCool, std::fabs( rig.State()[ 0 ] - h0 * std::exp( -cooled * dt / tauGot ) ) );
		}
		Check( worstCool <= kStateTol, fmt( "%dx%d cooling on black from H %.4f, 181 frames: worst |H - H0 e^(-t/tau)| %.3g (bound %.0e)",
		                                    raster.w, raster.h, h0, worstCool, kStateTol ) );
	}
	return Verdict();
}

//===========================================================================
// --psu: a sagging supply caps a tile's mean; a tripping one blinks.
//===========================================================================
int runPsu( const Perturb& perturb )
{
	std::printf( "\n=== psu: Dim caps a tile's mean at the limit (tiles under it untouched); Hiccup is off ceil(R/dt) frames, on 1\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		Floats card( static_cast< size_t >( raster.w ) * raster.h * 4 );
		if( !startRig( rig, raster, card, perturb ) )
			return 1;
		smallTiles( rig, raster );
		rig.Render( 1 );
		const wall::Layout l = rig.plugin.CurrentLayout();
		//Each tile its own flat level, 0.1 .. 0.9.
		auto levelOf = [ & ]( int tx, int ty ) { return 0.1f + 0.8f * static_cast< float >( ( tx * 5 + ty * 3 ) % 9 ) / 8.0f; };
		for( int y = 0; y < raster.h; ++y )
			for( int x = 0; x < raster.w; ++x )
			{
				const float v = levelOf( x / l.tileW, y / l.tileH );
				float* o      = &card[ ( static_cast< size_t >( raster.h - 1 - y ) * raster.w + x ) * 4 ];
				o[ 0 ] = o[ 1 ] = o[ 2 ] = v;
				o[ 3 ]                   = 1.0f;
			}
		rig.Upload( card );
		const double limit = 0.5;
		rig.Set( PT_PSU_LIMIT, static_cast< float >( ( limit - 0.05 ) / 0.95 ) );
		rig.Render( 1 );
		const Floats panel = rig.Panel();
		double worst       = 0.0;
		int capped         = 0;
		for( int ty = 0; ty < l.rows; ++ty )
			for( int tx = 0; tx < l.cols; ++tx )
			{
				if( ( tx + 1 ) * l.tileW > l.ledsW || ( ty + 1 ) * l.tileH > l.ledsH )
					continue;
				const double v = levelOf( tx, ty );
				capped += v > limit;
				worst = std::max( worst, std::fabs( at( panel, l.ledsW, tx * l.tileW + 1, ty * l.tileH + 1 )[ 1 ] - std::min( v, limit ) ) );
			}
		Check( worst <= 2.0 * kHalfUlp && capped > 0,
		       fmt( "%dx%d Dim at %.2f: %d tiles over it, worst |tile - min( level, limit )| %.3g (bound two half-float ULPs)", raster.w, raster.h,
		            limit, capped, worst ) );

		for( double fps : { 60.0, 50.0 } )
		{
			const Floats bright = flatCard( raster.w, raster.h, 0.8f, 0.8f, 0.8f );
			Rig hic;
			if( !startRig( hic, raster, bright, perturb ) )
				return 1;
			hic.fps = fps;
			smallTiles( hic, raster );
			hic.Set( PT_PSU_LIMIT, static_cast< float >( ( limit - 0.05 ) / 0.95 ) );
			hic.Set( PT_PSU_MODE, static_cast< float >( PsuMode::Hiccup ) );
			std::vector< int > on;
			for( int f = 0; f < 80; ++f )
			{
				hic.Render( 1 );
				if( hic.Panel()[ 0 ] > 0.0f )
					on.push_back( f );
			}
			//The first frame has dt 0, so the first period is one frame longer;
			//count the spacing from the first on-frame.
			const int period = static_cast< int >( std::ceil( kRestartSeconds * fps ) ) + 1;
			int bad          = on.size() < 3 ? 1 : 0;
			for( size_t i = 1; i < on.size(); ++i )
				bad += on[ i ] - on[ i - 1 ] != period;
			std::string list;
			for( int f : on )
				list += fmt( " %d", f );
			Check( bad == 0, fmt( "%dx%d Hiccup at %.0f fps: on at frames%s -- every %d frames (ceil( %.2f s x %.0f ) off + 1 on)", raster.w,
			                      raster.h, fps, list.c_str(), period, kRestartSeconds, fps ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --leds: dead and stuck LEDs at their rates.
//===========================================================================
int runLeds( const Perturb& perturb )
{
	std::printf( "\n=== leds: the dead and stuck fractions are binomial at their rates (within 4 sigma)\n" );
	for( const Raster& raster : kRasters )
		for( bool stuck : { false, true } )
		{
			const Floats card = flatCard( raster.w, raster.h, 0.5f, 0.5f, 0.5f );
			Rig rig;
			if( !startRig( rig, raster, card, perturb ) )
				return 1;
			const float v = 0.5f;
			rig.Set( stuck ? PT_STUCK_LEDS : PT_DEAD_LEDS, v );
			if( !rig.Render( 1 ) )
				return 1;
			const Floats panel    = rig.Panel();
			const wall::Layout& l = rig.plugin.CurrentLayout();
			long long hits        = 0;
			const long long n     = static_cast< long long >( l.ledsW ) * l.ledsH;
			for( long long i = 0; i < n; ++i )
			{
				const float* p = &panel[ static_cast< size_t >( i ) * 4 ];
				hits += stuck ? ( p[ 0 ] == 1.0f || p[ 1 ] == 1.0f || p[ 2 ] == 1.0f ) : ( p[ 0 ] == 0.0f && p[ 1 ] == 0.0f && p[ 2 ] == 0.0f );
			}
			const double rate  = LedRateFromParam( v );
			const double sigma = std::sqrt( rate * ( 1.0 - rate ) / static_cast< double >( n ) );
			const double got   = static_cast< double >( hits ) / static_cast< double >( n );
			Check( std::fabs( got - rate ) <= 4.0 * sigma,
			       fmt( "%dx%d %s LEDs %.2f: %lld of %lld (%.4f; rate %.4f, 4 sigma %.4f)", raster.w, raster.h, stuck ? "Stuck" : "Dead ", v, hits,
			            n, got, rate, 4.0 * sigma ) );
		}
	return Verdict();
}

//===========================================================================
// --resize: a new raster starts the hold again, never shows a stale buffer.
//===========================================================================
int runResize( const Perturb& perturb )
{
	std::printf( "\n=== resize: after the host's raster changes, the wall is exactly a fresh instance's at the new raster\n" );
	const Floats a = buildCard( 1280, 720 );
	const Floats b = noiseCard( 640, 360, 5 );
	auto configure = []( Rig& rig ) {
		perfect( rig );
		rig.Set( PT_TILE_W, 64.0f );
		rig.Set( PT_TILE_H, 36.0f );
		rig.Set( PT_CHAIN_BREAK, 1.0f );
		rig.Set( PT_LOST_SIGNAL, static_cast< float >( LostSignal::Hold ) );
		rig.Set( PT_HOP_DELAY, ParamFromHopDelay( 0.5 ) );
		rig.Set( PT_TILE_SPREAD, 0.5f );
	};
	Rig resized;
	if( !resized.Init( 1280, 720, &a ) )
		return 1;
	resized.plugin.SetHooksForTest( perturb.hooks );
	resized.plugin.SetResizeKeepsStateForTest( perturb.resizeKeeps );
	configure( resized );
	if( !resized.Render( 30 ) || !resized.Resize( 640, 360, &b ) || !resized.Render( 3 ) )
		return 1;
	Rig fresh;
	if( !fresh.Init( 640, 360, &b ) )
		return 1;
	configure( fresh );
	if( !fresh.Render( 3 ) )
		return 1;
	const Floats x = resized.Output(), y = fresh.Output();
	int differ     = 0;
	for( size_t i = 0; i < x.size(); ++i )
		differ += byteOf( x[ i ] ) != byteOf( y[ i ] );
	Check( differ == 0, fmt( "1280x720 for 30 frames, then 640x360 for 3, against a fresh 640x360 for 3 (Chain Break 1 under Hold, Hop "
	                         "Delay on): %d bytes differ",
	                         differ ) );
	return Verdict();
}

//===========================================================================
// --state: the GL state the host hands over is the state it gets back.
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: the GL state the host hands over is the state it gets back\n" );
	Rig rig;
	if( !rig.Init( 320, 180 ) )
		return 1;
	rig.Set( PT_HOP_DELAY, 0.3f );
	rig.Set( PT_LOST_SIGNAL, static_cast< float >( LostSignal::Hold ) );
	GLuint hostArray = 0, hostBuffer = 0;
	glGenVertexArrays( 1, &hostArray );
	glGenBuffers( 1, &hostBuffer );
	int problems = 0;
	std::string what;
	for( int frame = 0; frame < 3; ++frame )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
		glViewport( 7, 5, 300, 170 );
		glBindVertexArray( hostArray );
		glBindBuffer( GL_ARRAY_BUFFER, hostBuffer );
		glEnable( GL_BLEND );
		glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
		glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
		glEnable( GL_SCISSOR_TEST );
		glScissor( 0, 0, 320, 180 );
		glActiveTexture( GL_TEXTURE0 );
		glUseProgram( 0 );
		rig.plugin.SetTime( frame / 60.0 );
		if( rig.plugin.ProcessOpenGL( &rig.process ) != FF_SUCCESS )
			return 1;
		GLint viewport[ 4 ] = {}, array = 0, buffer = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0;
		GLfloat clear[ 4 ]   = {};
		GLboolean mask[ 4 ]  = {};
		glGetIntegerv( GL_VIEWPORT, viewport );
		glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
		glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buffer );
		glGetIntegerv( GL_CURRENT_PROGRAM, &program );
		glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
		glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
		glGetIntegerv( GL_BLEND_SRC_RGB, &src );
		glGetIntegerv( GL_BLEND_DST_RGB, &dst );
		glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
		glGetBooleanv( GL_COLOR_WRITEMASK, mask );
		auto expect = [ & ]( bool ok, const char* name ) {
			if( !ok )
			{
				++problems;
				what += std::string( " " ) + name;
			}
		};
		expect( viewport[ 0 ] == 7 && viewport[ 1 ] == 5 && viewport[ 2 ] == 300 && viewport[ 3 ] == 170, "viewport" );
		expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
		expect( buffer == static_cast< GLint >( hostBuffer ), "array-buffer" );
		expect( program == 0, "program" );
		expect( unit == GL_TEXTURE0, "active-unit" );
		expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
		expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
		expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
		expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
		expect( mask[ 0 ] && mask[ 1 ] && mask[ 2 ] && mask[ 3 ], "colour-mask" );
		for( int u = 0; u < 10; ++u )
		{
			GLint bound = 0, boundArray = 0;
			glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
			glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
			glGetIntegerv( GL_TEXTURE_BINDING_2D_ARRAY, &boundArray );
			expect( bound == 0 && boundArray == 0, "texture-unit" );
		}
		glActiveTexture( GL_TEXTURE0 );
	}
	glDisable( GL_SCISSOR_TEST );
	glDisable( GL_BLEND );
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glDeleteVertexArrays( 1, &hostArray );
	glDeleteBuffers( 1, &hostBuffer );
	Check( problems == 0, fmt( "three frames with the ring and the hold live: viewport, vertex array, array buffer, program, active unit, "
	                           "framebuffer, blend, scissor, clear colour, colour mask, ten texture units (%d wrong:%s)",
	                           problems, what.empty() ? " none" : what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --chain-law (no GL): the CPU's cable, which the GPU is measured against.
//===========================================================================
int runChainLaw( const Perturb& perturb )
{
	std::printf( "\n=== chain-law: every route is a bijection onto the tiles; the snakes step to a neighbour every hop, along the travel direction\n" );
	int layouts = 0, bad = 0;
	std::string first;
	for( int cols = 1; cols <= 9; ++cols )
		for( int rows = 1; rows <= 7; ++rows )
			for( int route = 0; route < static_cast< int >( Route::Count ); ++route )
				for( int corner = 0; corner < static_cast< int >( Corner::Count ); ++corner )
					for( int perPort : { 0, 1, 4, 7 } )
					{
						++layouts;
						const wall::Layout l = wall::MakeLayout( cols * 10, rows * 10, 1, 10, 10, perPort, static_cast< Route >( route ),
						                                         static_cast< Corner >( corner ) );
						std::vector< int > seen( static_cast< size_t >( l.Tiles() ), 0 );
						int problems = 0;
						for( int ty = 0; ty < rows; ++ty )
							for( int tx = 0; tx < cols; ++tx )
							{
								const int i = wall::LinearIndex( l, tx, ty );
								int bx = -1, by = -1;
								wall::TileAt( l, i, bx, by );
								problems += i < 0 || i >= l.Tiles() || bx != tx || by != ty;
								if( i >= 0 && i < l.Tiles() )
									++seen[ static_cast< size_t >( i ) ];
								const wall::Link link = wall::LinkOf( l, tx, ty );
								problems += link.pos < 0 || link.pos >= link.length;
							}
						for( int s : seen )
							problems += s != 1;
						//The claim for a snake (or, perturbed, made of a raster): from
						//each tile the next is one step along its travel direction or,
						//at the end of a line, the neighbour across.
						const bool snake = route <= 1;
						if( snake || perturb.lawRaster )
							for( int i = 0; i + 1 < l.Tiles(); ++i )
							{
								int ax = 0, ay = 0, bx = 0, by = 0;
								wall::TileAt( l, i, ax, ay );
								wall::TileAt( l, i + 1, bx, by );
								const int d   = std::abs( ax - bx ) + std::abs( ay - by );
								const int dir = wall::TravelOf( l, ax, ay );
								const bool along = l.AlongX() ? ( by == ay && bx == ax + dir ) : ( bx == ax && by == ay + dir );
								const bool turn  = l.AlongX() ? ( bx == ax && ( ax == 0 || ax == cols - 1 ) ) : ( by == ay && ( ay == 0 || ay == rows - 1 ) );
								problems += d != 1 || !( along || turn );
							}
						if( problems > 0 && first.empty() )
							first = fmt( " (first: %dx%d %s from %s, %d per port)", cols, rows, InfoOf( PT_ROUTE ).options[ route ],
							             InfoOf( PT_CORNER ).options[ corner ], perPort );
						bad += problems > 0;
					}
	Check( bad == 0, fmt( "%d layouts (1..9 x 1..7 tiles, 4 routes, 4 corners, 4 port sizes): %d wrong%s", layouts, bad, first.c_str() ) );
	return Verdict();
}

//===========================================================================
// --motion-law (no GL): the repeat's offset, from its integrated phase.
//===========================================================================
int runMotionLaw( const Perturb& perturb )
{
	std::printf( "\n=== motion-law: the phase is integrated (a change of speed never jumps the content); Step whole tiles, Scroll whole LEDs,\n"
	             "    Ping-Pong turns at the window, Random a whole tile that changes once per tile of phase\n" );
	const int N = 3, L = 20;
	//Speed 2 tiles/s for 1 s, then 0.5 for 1 s, at 60 fps.
	wall::RepeatMotion motion;
	double elapsed = 0.0, worstJump = 0.0;
	int lastScroll = -1, badStep = 0, badRandom = 0, lastRandomSlot = -1, lastRandom = -1;
	bool pingTurned = false;
	double lastPing = -1.0, pingDir = 0.0;
	for( int f = 1; f <= 240; ++f )
	{
		const double dt    = 1.0 / 60.0;
		const double speed = f <= 60 ? 2.0 : 0.5;
		elapsed += dt;
		motion.Advance( speed, dt );
		if( perturb.motionAbsolute )
			motion.SetPhase( speed * elapsed );
		const int scroll = motion.OffsetLeds( Motion::Scroll, N, L, 1 );
		if( lastScroll >= 0 )
		{
			int step = scroll - lastScroll;
			if( step < -N * L / 2 )
				step += N * L;
			worstJump = std::max( worstJump, std::fabs( static_cast< double >( step ) ) );
		}
		lastScroll = scroll;
		const int stepped = motion.OffsetLeds( Motion::Step, N, L, 1 );
		badStep += stepped % L != 0 || stepped != static_cast< int >( ( static_cast< long long >( std::floor( motion.Phase() ) ) % N ) * L );
		const int random = motion.OffsetLeds( Motion::Random, N, L, 1 );
		const int slot   = static_cast< int >( std::floor( motion.Phase() ) );
		badRandom += random % L != 0 || random < 0 || random >= N * L || ( slot == lastRandomSlot && random != lastRandom );
		lastRandomSlot = slot;
		lastRandom     = random;
		const double ping = motion.OffsetLeds( Motion::PingPong, N, L, 1 );
		if( lastPing >= 0.0 && ping != lastPing )
		{
			const double d = ping > lastPing ? 1.0 : -1.0;
			pingTurned     = pingTurned || ( pingDir != 0.0 && d != pingDir );
			pingDir        = d;
		}
		lastPing = ping;
	}
	//2 tiles/s at 20 LEDs a tile is 0.67 LEDs a frame: never more than one.
	Check( worstJump <= 1.0 && badStep == 0 && badRandom == 0 && pingTurned,
	       fmt( "2 tiles/s then 0.5, 240 frames: worst Scroll step %.0f LEDs (bound 1); %d Step offsets not floor(phase) tiles; %d Random "
	            "offsets not a whole tile in the window, or changing inside a slot; Ping-Pong %s",
	            worstJump, badStep, badRandom, pingTurned ? "turned" : "never turned" ) );
	return Verdict();
}

//===========================================================================
// --cues (no GL): options, booleans, events and integers step; the rest ramp.
//===========================================================================
int runCues( const Perturb& perturb )
{
	std::printf( "\n=== cues: a cue sheet steps options and integers, and ramps standard controls\n" );
	std::istringstream sheet( "0 Route 0\n60 Route 2\n0 Tile W 20\n60 Tile W 80\n0 Zebra 0\n60 Zebra 1\n" );
	std::string error;
	const auto tracks = loadScript( sheet, "cues", error );
	Patchwork plugin;
	int bad = 0;
	std::string what;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		const auto found = tracks.find( p.name );
		if( found == tracks.end() )
			continue;
		const bool ramp = perturb.cuesRamp ? true : !stepsBetweenCues( p.type );
		const float mid = valueAt( found->second, 29, ramp );
		const float a = found->second.front().second, b = found->second.back().second;
		const bool expectRamp = p.type == FF_TYPE_STANDARD;
		const bool ok         = expectRamp ? ( mid > a && mid < b ) : ( mid == a && valueAt( found->second, 60, ramp ) == b );
		bad += !ok;
		what += fmt( " %s %s(%g at frame 29)", p.name.c_str(), expectRamp ? "ramps " : "steps ", mid );
	}
	Check( error.empty() && bad == 0 && tracks.size() == 3, fmt( "%d wrong:%s", bad, what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --names (no GL)
//===========================================================================
int runNames( const Perturb& )
{
	std::printf( "\n=== names: every parameter unique (as Arena addresses them, too) and within FFGL's 16 characters\n" );
	Patchwork plugin;
	std::map< std::string, int > seen, address;
	int longNames = 0, dupes = 0, clashes = 0;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const std::string name = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
		if( name.size() > 16 )
		{
			std::printf( "    too long: %s\n", name.c_str() );
			++longNames;
		}
		if( seen[ name ]++ > 0 )
			++dupes;
		std::string key;
		for( char c : name )
			if( c != ' ' )
				key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		if( address[ key ]++ > 0 )
			++clashes;
	}
	const unsigned int aboutFirst = plugin.GetNumParams() - stoatworks::about::kParamCount;
	const bool aboutLast          = std::string( plugin.GetParamName( aboutFirst ) ) == "About";
	Check( longNames == 0 && dupes == 0 && clashes == 0 && aboutLast,
	       fmt( "SW Patchwork: %u parameters, %d too long, %d duplicated, %d clashing addresses; the About block is last (%s)",
	            plugin.GetNumParams(), longNames, dupes, clashes, aboutLast ? "yes" : "NO" ) );
	return Verdict();
}

struct CheckEntry
{
	const char* flag;
	CheckFn run;
	bool offline;///< needs no GL context
};

const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "identity", runIdentity, false }, { "tiles", runTiles, false },    { "batches", runBatches, false },
		{ "zebra", runZebra, false },       { "chain", runChain, false },    { "repeat", runRepeat, false },
		{ "hold", runHold, false },         { "heat", runHeat, false },      { "psu", runPsu, false },
		{ "leds", runLeds, false },         { "resize", runResize, false },  { "state", runState, false },
		{ "chain-law", runChainLaw, true }, { "motion-law", runMotionLaw, true }, { "cues", runCues, true },
		{ "names", runNames, true },
	};
	return list;
}

bool isOffline( const std::string& flag )
{
	for( const CheckEntry& c : checks() )
		if( flag == c.flag )
			return c.offline;
	return false;
}

//===========================================================================
// --negative
//===========================================================================
int runNegative( bool offlineOnly = false )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	using namespace shaders;
	add( "identity", runIdentity, "the wall's box starts one pixel right", []( Perturb& p ) { p.hooks = kHookWallSkew; } );
	add( "tiles", runTiles, "the calibration reads the tile one LED off", []( Perturb& p ) { p.hooks = kHookTileSkew; } );
	add( "batches", runBatches, "a batch id that ignores Batches", []( Perturb& p ) { p.hooks = kHookBatchUnbounded; } );
	add( "zebra", runZebra, "a doubled row shows only its own data", []( Perturb& p ) { p.hooks = kHookZebraSingle; } );
	add( "chain", runChain, "a break loses the tiles before it", []( Perturb& p ) { p.hooks = kHookBreakPrefix; } );
	add( "chain", runChain, "the hop delay counts one hop too many", []( Perturb& p ) { p.hooks = kHookHopSkew; } );
	add( "repeat", runRepeat, "the repeat turns back with the source tile's direction", []( Perturb& p ) { p.hooks = kHookMirror; } );
	add( "hold", runHold, "the hold follows the route even when lost", []( Perturb& p ) { p.hooks = kHookHoldFollows; } );
	add( "heat", runHeat, "alpha = dt / tau (forward Euler)", []( Perturb& p ) { p.heatEuler = true; } );
	add( "psu", runPsu, "Dim scales by the square root of the cap", []( Perturb& p ) { p.hooks = kHookPsuUncapped; } );
	add( "leds", runLeds, "dead LEDs at twice the rate", []( Perturb& p ) { p.hooks = kHookLedRate; } );
	add( "resize", runResize, "the hold survives a reallocation", []( Perturb& p ) { p.resizeKeeps = true; } );
	add( "chain-law", runChainLaw, "the snake's claims made of the raster routes", []( Perturb& p ) { p.lawRaster = true; } );
	add( "motion-law", runMotionLaw, "phase = speed x elapsed, not integrated", []( Perturb& p ) { p.motionAbsolute = true; } );
	add( "cues", runCues, "ramp every control between keys", []( Perturb& p ) { p.cuesRamp = true; } );

	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/patchwork.png";
	std::vector< std::string > settings;
	int width = 1280, height = 720, frames = 60;
	double fps = 60.0;
	std::string mode, scriptPath, clipPath;
	int filmFrames = -1;
	bool sizeGiven = false, moving = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "pwtest -- render Patchwork offline and measure its wall\n\n"
			             "  --out PATH        render the card and write a PNG (default /tmp/patchwork.png)\n"
			             "  --clip FILE       (with --out) a raw RGBA frame of --size to use instead of the card\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --frames N        frames before reading back (default 60)\n"
			             "  --moving          (with --out / --film) the picture pans 3 pixels a frame\n"
			             "  --fps N           the synthetic clock's rate (default 60)\n"
			             "  --set \"Name=V\"    set a parameter by its display name (an option by its name). Repeatable.\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA frames in on stdin, out on stdout\n"
			             "  --film N          N frames of the card, raw RGBA on stdout\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value'\n\n"
			             "  checks: --identity --tiles --batches --zebra --chain --repeat --hold --heat --psu --leds --resize --state\n"
			             "          no GL: --chain-law --motion-law --cues --names\n"
			             "          --negative   --bench\n"
			             "  --offline         the checks and negative controls that need no GL context (CI)\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--clip" && hasNext )
			clipPath = argv[ ++i ];
		else if( argument == "--moving" )
			moving = true;
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "film";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
			sizeGiven = true;
		}
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}
	if( width <= 0 || height <= 0 || !( fps > 0.0 ) )
	{
		std::fprintf( stderr, "--size and --fps must be positive\n" );
		return 2;
	}
	//A check given --size runs at that raster alone (the software pass).
	if( sizeGiven )
		kRasters = { { width, height } };

	if( mode == "list" )
	{
		Patchwork plugin;
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), kindName( parameter.type ), parameter.value );
		return 0;
	}

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( check.offline )
				failed |= check.run( Perturb {} );
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The wall checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above, and again on the software renderer.\n"
		             "\n  %s\n",
		             failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}
	for( const CheckEntry& check : checks() )
		if( mode == check.flag && check.offline )
			return check.run( Perturb {} );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}

	if( ran )
		;
	else if( mode == "pipe" )
		result = runPipe( width, height, fps, scriptPath, 0, true, settings, false );
	else if( mode == "film" )
		result = runPipe( width, height, fps, scriptPath, filmFrames, false, settings, moving );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Rig rig;
		rig.fps = fps;
		Floats card;
		if( !clipPath.empty() )
		{
			std::ifstream file( clipPath, std::ios::binary );
			Bytes raw( static_cast< size_t >( width ) * height * 4 );
			if( !file.read( reinterpret_cast< char* >( raw.data() ), static_cast< std::streamsize >( raw.size() ) ) )
			{
				std::fprintf( stderr, "--clip %s: not %dx%d RGBA\n", clipPath.c_str(), width, height );
				return 2;
			}
			card.resize( raw.size() );
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					card[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = raw[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
		}
		else
			card = buildCard( width, height );
		if( !rig.Init( width, height, &card ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			bool rendered = true;
			for( int f = 0; f < std::max( frames, 1 ) && rendered; ++f )
			{
				if( moving )
					rig.Upload( pannedCard( card, width, height, 3 * f ) );
				rendered = rig.Render( 1 );
			}
			if( !rendered )
				result = 1;
			else if( writePng( outPath, width, height, rig.Output() ) )
				std::printf( "wrote %s -- %dx%d, %d frames at %g fps (%.2f s)\n", outPath.c_str(), width, height, frames, fps, frames / fps );
			else
				result = 1;
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
