#include "Shaders.h"

namespace patchwork::shaders
{

const char* const kVersion = "#version 410 core\n";

const char* const kQuadVertex = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// What every fragment pass shares: the wall's geometry, the cable, the hash
// and the signal's fate. Integer arithmetic throughout; GLSL's % and / are
// undefined on negative operands, so nothing here ever hands them one.
//---------------------------------------------------------------------------
const char* const kCommon = R"(
uniform ivec2 Leds;    // LEDs across and down
uniform ivec2 Tile;    // LEDs per tile
uniform ivec2 Tiles;   // tiles across and down
uniform ivec2 Modules; // modules per tile, across and down
uniform int Scan;      // scan groups S: LED row r of a module has address r mod S
uniform int Route;     // 0 row snake, 1 column snake, 2 rows, 3 columns
uniform int Corner;    // 0 top left, 1 top right, 2 bottom left, 3 bottom right
uniform int PerPort;   // tiles per chain, resolved (never 0)
uniform uint Seed;
uniform int Hooks;     // negative-control hooks; 0 in every shipped frame

uniform uint TBreak;        // a chain is broken
uniform uint TConnected;    // ...and in a given slot its connector is making contact
uniform uint IntermittentBase;
uniform float IntermittentFrac;
uniform uint TDropout;      // a tile has lost its signal in a given slot
uniform uint DropoutBase;
uniform float DropoutFrac;

const uint SALT_BREAK     = 0x9e3779b9u;
const uint SALT_BREAK_AT  = 0x7f4a7c15u;
const uint SALT_CONTACT   = 0x85ebca6bu;
const uint SALT_CONTACT_P = 0xc2b2ae35u;
const uint SALT_DROP      = 0x27d4eb2fu;
const uint SALT_DROP_P    = 0x165667b1u;

//The PCG output mix: exact 32-bit integer arithmetic, the same on every GPU
//and in Hash.h. Never fract( sin( x ) ).
uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
uint hash3( uint a, uint b, uint c )
{
	return pcg( a ^ pcg( b ^ pcg( c ) ) );
}
uint hash4( uint a, uint b, uint c, uint d )
{
	return pcg( a ^ pcg( b ^ pcg( c ^ pcg( d ) ) ) );
}
//24 bits of a hash as a float in [0, 1), exactly.
float unit( uint h )
{
	return float( h >> 8u ) * ( 1.0 / 16777216.0 );
}
uint pack2( ivec2 v )
{
	return uint( v.x ) | ( uint( v.y ) << 16u );
}
uint tileHash( ivec2 t, uint salt )
{
	return hash3( pack2( t ), Seed, salt );
}

//--- the cable --------------------------------------------------------------
struct Link
{
	int port;// which chain
	int pos; // 0 at the sending card
	int span;// tiles on this chain
};

bool flipX()
{
	return Corner == 1 || Corner == 3;
}
bool flipY()
{
	return Corner >= 2;
}

//The order the cable visits the tiles, every port end to end.
int linearIndex( ivec2 t )
{
	int x = flipX() ? Tiles.x - 1 - t.x : t.x;
	int y = flipY() ? Tiles.y - 1 - t.y : t.y;
	if( Route == 0 )
		return y * Tiles.x + ( ( y & 1 ) == 0 ? x : Tiles.x - 1 - x );
	if( Route == 1 )
		return x * Tiles.y + ( ( x & 1 ) == 0 ? y : Tiles.y - 1 - y );
	if( Route == 2 )
		return y * Tiles.x + x;
	return x * Tiles.y + y;
}

ivec2 tileAt( int linear )
{
	int x, y;
	if( Route == 0 || Route == 2 )
	{
		y = linear / Tiles.x;
		x = linear - y * Tiles.x;
		if( Route == 0 && ( y & 1 ) != 0 )
			x = Tiles.x - 1 - x;
	}
	else
	{
		x = linear / Tiles.y;
		y = linear - x * Tiles.y;
		if( Route == 1 && ( x & 1 ) != 0 )
			y = Tiles.y - 1 - y;
	}
	return ivec2( flipX() ? Tiles.x - 1 - x : x, flipY() ? Tiles.y - 1 - y : y );
}

Link linkOf( ivec2 t )
{
	int linear = linearIndex( t );
	Link ln;
	ln.port = linear / PerPort;
	ln.pos  = linear - ln.port * PerPort;
	ln.span = min( PerPort, Tiles.x * Tiles.y - ln.port * PerPort );
	return ln;
}

//+1 when the cable runs through this tile towards +x (row routes) or down
//the picture (column routes); -1 the other way.
int travelOf( ivec2 t )
{
	int x = flipX() ? Tiles.x - 1 - t.x : t.x;
	int y = flipY() ? Tiles.y - 1 - t.y : t.y;
	if( Route == 0 )
		return ( ( y & 1 ) == 0 ? 1 : -1 ) * ( flipX() ? -1 : 1 );
	if( Route == 1 )
		return ( ( x & 1 ) == 0 ? 1 : -1 ) * ( flipY() ? -1 : 1 );
	if( Route == 2 )
		return flipX() ? -1 : 1;
	return flipY() ? -1 : 1;
}

//--- the modules --------------------------------------------------------------
//Module m of a tile spans LEDs [ floor( m T / M ), floor( ( m + 1 ) T / M ) ).
ivec2 moduleIndex( ivec2 local )
{
	return ( ( local + 1 ) * Modules - 1 ) / Tile;
}
ivec2 moduleStart( ivec2 m )
{
	return ( m * Tile ) / Modules;
}

//--- whether a tile's receiving card has a signal ------------------------------
//A slot index plus a per-tile phase, from an integer base and a small
//fraction the CPU reduced in double: no large float ever reaches here.
uint slotOf( uint base, float fraction, float phase )
{
	return base + ( fraction + phase >= 1.0 ? 1u : 0u );
}

bool tileLost( ivec2 t, Link ln )
{
	if( TBreak > 0u && hash3( uint( ln.port ), Seed, SALT_BREAK ) < TBreak )
	{
		//The break is after position `at`'s input: it and everything downstream
		//of it have nothing.
		int at          = int( hash3( uint( ln.port ), Seed, SALT_BREAK_AT ) % uint( ln.span ) );
		bool downstream = ( Hooks & 16 ) != 0 ? ln.pos <= at : ln.pos >= at;
		if( downstream )
		{
			float phase    = unit( hash3( uint( ln.port ), Seed, SALT_CONTACT_P ) );
			uint slot      = slotOf( IntermittentBase, IntermittentFrac, phase );
			bool connected = TConnected > 0u && hash4( uint( ln.port ), slot, Seed, SALT_CONTACT ) < TConnected;
			if( !connected )
				return true;
		}
	}
	if( TDropout > 0u )
	{
		float phase = unit( tileHash( t, SALT_DROP_P ) );
		uint slot   = slotOf( DropoutBase, DropoutFrac, phase );
		if( hash4( pack2( t ), slot, Seed, SALT_DROP ) < TDropout )
			return true;
	}
	return false;
}
)";

//---------------------------------------------------------------------------
// What a receiving card puts out: its route, or -- when it has no signal --
// its fallback. The stats and the panel both need it.
//---------------------------------------------------------------------------
const char* const kSignal = R"(
uniform sampler2D RouteTex;
uniform sampler2D HoldTex;
uniform int Lost;        // 0 black, 1 hold, 2 test pattern, 3 garbage
uniform int PatternPhase;// the test pattern's colour, changing once a second
uniform uint Frame;      // a frame counter, for garbage and floating lines

const uint SALT_GARBAGE = 0xd3a2646cu;

vec3 testPattern( ivec2 led, ivec2 t )
{
	//The card's own: the module grid in white over a colour that steps
	//red, green, blue, white.
	ivec2 local = led - t * Tile;
	ivec2 m0    = moduleStart( moduleIndex( local ) );
	if( local.x == m0.x || local.y == m0.y )
		return vec3( 1.0 );
	vec3 colours[ 4 ] = vec3[ 4 ]( vec3( 1.0, 0.0, 0.0 ), vec3( 0.0, 1.0, 0.0 ), vec3( 0.0, 0.0, 1.0 ), vec3( 1.0 ) );
	return 0.5 * colours[ PatternPhase & 3 ];
}

vec3 garbage( ivec2 led )
{
	//Corrupt data: runs of eight LEDs of a random colour, new every frame.
	uint h  = hash4( uint( led.x >> 3 ) | ( uint( led.y ) << 16u ), Frame, Seed, SALT_GARBAGE );
	vec3 on = vec3( float( h & 1u ), float( ( h >> 1u ) & 1u ), float( ( h >> 2u ) & 1u ) );
	return on * unit( pcg( h ) );
}

vec3 received( ivec2 led, ivec2 t, bool lost )
{
	if( !lost )
		return texelFetch( RouteTex, led, 0 ).rgb;
	if( Lost == 1 )
		return texelFetch( HoldTex, led, 0 ).rgb;
	if( Lost == 2 )
		return testPattern( led, t );
	if( Lost == 3 )
		return garbage( led );
	return vec3( 0.0 );
}
)";

//---------------------------------------------------------------------------
// 1. wall: the box mean of each LED's pixels. The input is read by
// texelFetch, so the host's padding (Width < HardwareWidth) never enters,
// and at LED Pitch 1 this is a copy -- which --identity relies on.
//---------------------------------------------------------------------------
const char* const kWallFragment = R"(
uniform sampler2D InputTexture;
uniform ivec2 Size; // the picture, in pixels
uniform int Pitch;  // pixels per LED

out vec4 fragColor;

void main()
{
	ivec2 led = ivec2( gl_FragCoord.xy );
	int x0    = led.x * Pitch + ( ( Hooks & 1 ) != 0 ? 1 : 0 );
	int y0    = led.y * Pitch;
	int x1    = min( x0 + Pitch, Size.x );
	int y1    = min( y0 + Pitch, Size.y );
	vec4 sum  = vec4( 0.0 );
	int n     = 0;
	for( int y = y0; y < y1; ++y )
		for( int x = x0; x < x1; ++x )
		{
			//Top-down LED rows, bottom-up GL rows: the one flip on the way in.
			sum += texelFetch( InputTexture, ivec2( x, Size.y - 1 - y ), 0 );
			++n;
		}
	fragColor = n > 0 ? sum / float( n ) : vec4( 0.0 );
}
)";

//---------------------------------------------------------------------------
// 2. copy: the wall into the ring's current layer.
//---------------------------------------------------------------------------
const char* const kCopyFragment = R"(
uniform sampler2D WallTex;

out vec4 fragColor;

void main()
{
	fragColor = texelFetch( WallTex, ivec2( gl_FragCoord.xy ), 0 );
}
)";

//---------------------------------------------------------------------------
// 3. route: the processor's map and the cable.
//
// Per destination tile, in this order: a crossed cable swaps it with its
// neighbour on the chain; the map's repeat picks which chain slot's content
// it is sent and where along it; a cabinet hung upside down turns that 180
// degrees; and the card's buffering picks how many frames old it is.
//
// The repeat. With the motion offset m in LEDs (0 <= m < N L), a tile at
// chain position pos in the region, and e the LED's index along the cable
// through this tile (0 at the end the cable comes in):
//
//     R = ( pos - s0 ) L + e - m        source = s0 + ( R div L ) mod N
//                                       e'     = R mod L
//
// and e' becomes a column (or row) with the DESTINATION's direction of
// travel: at rest every tile shows its source the right way round, and in
// motion the content flows along the cable, turning the corner at the end of
// every snake row. N L is added to R so nothing here is ever negative.
//---------------------------------------------------------------------------
const char* const kRouteFragment = R"(
uniform sampler2D WallTex;
uniform sampler2DArray RingTex;
uniform int RingActive;
uniform int RingDepth;
uniform int RingWrite;  // the layer holding this frame
uniform int RingFilled; // layers written since the ring was allocated

uniform uint TSwap;
uniform uint TFlip;
uniform uint TLag;
uniform int LagFrames;
uniform float HopDelay; // frames per hop

uniform int RepeatN;        // 0: no repeat
uniform float RepeatFrom;
uniform float RepeatReach;
uniform int RepeatShiftFull;// m for a window of min( RepeatN, PerPort )
uniform int RepeatShiftLast;// m for the last, shorter chain's window

const uint SALT_SWAP  = 0x2545f491u;
const uint SALT_FLIP  = 0x4f6cdd1du;
const uint SALT_LAG   = 0x6c8e9cf5u;
const uint SALT_LAG_N = 0x5bd1e995u;

out vec4 fragColor;

void main()
{
	ivec2 led   = ivec2( gl_FragCoord.xy );
	ivec2 t     = led / Tile;
	ivec2 local = led - t * Tile;
	Link ln     = linkOf( t );
	int base    = ln.port * PerPort;

	//A crossed pair: positions 2k and 2k+1 swap.
	int pos = ln.pos;
	if( TSwap > 0u )
	{
		int partner = pos ^ 1;
		if( partner < ln.span && hash3( uint( base + ( pos >> 1 ) ), Seed, SALT_SWAP ) < TSwap )
			pos = partner;
	}

	int srcPos     = pos;
	ivec2 srcLocal = local;
	if( RepeatN > 0 )
	{
		int n     = ln.span;
		int N     = min( RepeatN, n );
		int s0    = clamp( int( floor( RepeatFrom * float( n ) ) ), 0, n - N );
		int extra = int( floor( RepeatReach * float( n - s0 - N ) + 0.5 ) );
		if( pos >= s0 && pos < s0 + N + extra )
		{
			bool alongX = Route == 0 || Route == 2;
			int L       = alongX ? Tile.x : Tile.y;
			int shift   = N == min( RepeatN, PerPort ) ? RepeatShiftFull : RepeatShiftLast;
			int dir     = travelOf( t );
			int a       = alongX ? local.x : local.y;
			int e       = dir > 0 ? a : L - 1 - a;
			int R       = ( pos - s0 ) * L + e - shift + N * L;
			srcPos      = s0 + ( R / L ) % N;
			int e2      = R % L;
			if( ( Hooks & 32 ) != 0 )
				dir = travelOf( tileAt( base + srcPos ) );
			int a2 = dir > 0 ? e2 : L - 1 - e2;
			if( alongX )
				srcLocal.x = a2;
			else
				srcLocal.y = a2;
		}
	}

	if( TFlip > 0u && tileHash( t, SALT_FLIP ) < TFlip )
		srcLocal = Tile - 1 - srcLocal;

	ivec2 src = tileAt( base + srcPos ) * Tile + srcLocal;

	int delay = 0;
	if( RingActive != 0 )
	{
		int hops = ln.pos + ( ( Hooks & 256 ) != 0 ? 1 : 0 );
		delay    = int( floor( HopDelay * float( hops ) + 0.5 ) );
		if( TLag > 0u && tileHash( t, SALT_LAG ) < TLag )
			delay += 1 + int( tileHash( t, SALT_LAG_N ) % uint( LagFrames ) );
		delay = min( delay, min( RingDepth, RingFilled ) - 1 );
	}

	vec4 c = vec4( 0.0 );
	if( src.x < Leds.x && src.y < Leds.y )
	{
		if( delay <= 0 )
			c = texelFetch( WallTex, src, 0 );
		else
			c = texelFetch( RingTex, ivec3( src, ( RingWrite - delay + RingDepth ) % RingDepth ), 0 );
	}
	fragColor = c;
}
)";

//---------------------------------------------------------------------------
// 4. hold: a card that has lost its signal keeps showing the last frame it
// received. Every other tile's hold follows its route, so the moment a tile
// is lost its hold is already the frame before.
//---------------------------------------------------------------------------
const char* const kHoldFragment = R"(
uniform sampler2D RouteTex;
uniform sampler2D HoldPrev;
uniform int HoldValid;// 0 on the first frame and after a reallocation

out vec4 fragColor;

void main()
{
	ivec2 led = ivec2( gl_FragCoord.xy );
	ivec2 t   = led / Tile;
	bool keep = HoldValid != 0 && ( Hooks & 64 ) == 0 && tileLost( t, linkOf( t ) );
	fragColor = keep ? texelFetch( HoldPrev, led, 0 ) : texelFetch( RouteTex, led, 0 );
}
)";

//---------------------------------------------------------------------------
// 5. stats: one texel per tile. P is the tile's mean drive, (r + g + b) / 3
// of what its card puts out, from a 16 x 16 grid of stratified taps (exact on
// a flat field, an estimate on a picture). Then
//
//   heat:   H <- H + ( P - H ) alpha,  alpha = 1 - exp( -dt / tau ) in double
//   hiccup: armed (-1) -> tripped at 0 when P > limit -> off while the time
//           since the trip is under the restart -> on for the frame it
//           restarts, armed again.
//
// Out: ( H, time since the trip or -1, P, 1 if off this frame ).
//---------------------------------------------------------------------------
const char* const kStatsFragment = R"(
uniform sampler2D StatePrev;
uniform int StateValid;
uniform float HeatAlpha;
uniform float Dt;
uniform float PsuLimit;
uniform int PsuMode;
uniform float Restart;

out vec4 fragColor;

void main()
{
	ivec2 t      = ivec2( gl_FragCoord.xy );
	bool lost    = tileLost( t, linkOf( t ) );
	ivec2 origin = t * Tile;
	ivec2 size   = min( Tile, Leds - origin );
	ivec2 taps   = min( size, ivec2( 16 ) );
	float sum    = 0.0;
	for( int y = 0; y < taps.y; ++y )
		for( int x = 0; x < taps.x; ++x )
		{
			ivec2 led = origin + ( ivec2( x, y ) * 2 + 1 ) * size / ( taps * 2 );
			vec3 c    = received( led, t, lost );
			sum += ( c.r + c.g + c.b ) / 3.0;
		}
	float P = sum / float( taps.x * taps.y );

	vec4 prev   = StateValid != 0 ? texelFetch( StatePrev, t, 0 ) : vec4( 0.0, -1.0, 0.0, 0.0 );
	float heat  = prev.r + ( P - prev.r ) * HeatAlpha;
	float since = -1.0;
	float off   = 0.0;
	if( PsuMode == 1 )
	{
		since = prev.g;
		if( since >= 0.0 )
		{
			since += Dt;
			if( since < Restart )
				off = 1.0;
			else
				since = -1.0;
		}
		else if( P > PsuLimit )
		{
			since = 0.0;
			off   = 1.0;
		}
	}
	fragColor = vec4( heat, since, P, off );
}
)";

//---------------------------------------------------------------------------
// 6. panel: what each LED emits.
//
// The module first, because its faults are in the card's DATA: a shift
// register that missed a clock moves the row sideways; an address line stuck
// at v leaves every row whose address has bit k != v unselected (black) and
// selects every other row twice -- in its own slot and its partner's,
// r XOR 2^k -- so it shows the SUM of both rows: light moved, not removed. A
// dead row driver darkens one address in every scan group; a broken data
// chain loses one colour from a driver chip's first column on.
//
// Then the LED, the calibration table (the tile's batch and its module, every
// gain at most 1), the seams, the heat droop (red first), a stuck LED, and
// last the supply: dead, flickering, sagging or tripped.
//---------------------------------------------------------------------------
const char* const kPanelFragment = R"(
uniform sampler2D StateTex;

uniform uint TDeadModule;
uniform uint TZebra;
uniform int ZebraWidth;   // 0 random, else the stuck bit + 1
uniform int ZebraFloating;
uniform uint TDeadRow;
uniform uint TColourLoss;
uniform uint TShift;
uniform uint TDeadLed;
uniform uint TStuckLed;
uniform uint TDeadTile;
uniform uint TFlicker;
uniform uint FlickerBase;
uniform float FlickerFrac;

uniform float TileSpread;
uniform float ModuleSpread;
uniform float ColourSpread;
uniform int Batches;     // 0: every tile its own
uniform float Seams;
uniform float Heat;
uniform float PsuLimit;
uniform int PsuMode;

const uint SALT_SHIFT    = 0x68e31da4u;
const uint SALT_SHIFT_BY = 0xb5297a4du;
const uint SALT_ZEBRA    = 0x1b56c4e9u;
const uint SALT_ZEBRA_K  = 0x7feb352du;
const uint SALT_ZEBRA_V  = 0x846ca68bu;
const uint SALT_FLOAT    = 0x2c1b3c6du;
const uint SALT_ROW      = 0x297a2d39u;
const uint SALT_ROW_A    = 0xd35a2d97u;
const uint SALT_COLOUR   = 0xa0761d65u;
const uint SALT_COLOUR_C = 0xe7037ed1u;
const uint SALT_DEAD_MOD = 0x8ebc6af1u;
const uint SALT_DEAD_LED = 0x589965cdu;
const uint SALT_STUCK    = 0x1d8e4e27u;
const uint SALT_STUCK_C  = 0x9e6c63d0u;
const uint SALT_BATCH    = 0x3c6ef372u;
const uint SALT_GAIN     = 0xa54ff53au;
const uint SALT_TINT     = 0x510e527fu;
const uint SALT_MGAIN    = 0x9b05688cu;
const uint SALT_MTINT    = 0x1f83d9abu;
const uint SALT_VSEAM    = 0x5be0cd19u;
const uint SALT_HSEAM    = 0xcbbb9d5du;
const uint SALT_DEAD     = 0x629a292au;
const uint SALT_FLICKER  = 0x9159015au;
const uint SALT_FLICK_P  = 0x152fecd8u;
const uint SALT_FLICK_ON = 0x67332667u;

out vec4 fragColor;

vec3 tint( uint key, uint salt )
{
	return vec3( unit( hash4( key, Seed, salt, 0u ) ), unit( hash4( key, Seed, salt, 1u ) ),
	             unit( hash4( key, Seed, salt, 2u ) ) );
}

float seamGain( uint key, uint salt )
{
	//A gap of g pitches dims the LEDs either side by g / 2; an overlap (g < 0)
	//brightens them.
	return 1.0 - 0.5 * Seams * ( 2.0 * unit( hash3( key, Seed, salt ) ) - 1.0 );
}

void main()
{
	ivec2 led   = ivec2( gl_FragCoord.xy );
	ivec2 t     = led / Tile;
	ivec2 local = led - t * Tile;
	bool lost   = tileLost( t, linkOf( t ) );

	ivec2 m   = moduleIndex( local );
	ivec2 m0  = moduleStart( m );
	ivec2 m1  = moduleStart( m + 1 );
	uint mkey = pack2( t * Modules + m );
	int r     = local.y - m0.y;
	int c     = local.x - m0.x;
	int rows  = m1.y - m0.y;
	int top   = t.y * Tile.y + m0.y;

	//A missed shift clock: the module's data one to four LEDs to the right.
	int col = led.x;
	if( TShift > 0u && hash3( mkey, Seed, SALT_SHIFT ) < TShift )
		col -= 1 + int( hash3( mkey, Seed, SALT_SHIFT_BY ) % 4u );

	vec3 v = vec3( 0.0 );
	if( col >= t.x * Tile.x + m0.x )
	{
		v        = received( ivec2( col, led.y ), t, lost );
		int bits = findMSB( Scan );
		if( bits > 0 && TZebra > 0u && hash3( mkey, Seed, SALT_ZEBRA ) < TZebra )
		{
			int k     = ZebraWidth == 0 ? int( hash3( mkey, Seed, SALT_ZEBRA_K ) % uint( bits ) ) : min( ZebraWidth - 1, bits - 1 );
			uint want = ZebraFloating != 0 ? ( hash4( mkey, Frame, Seed, SALT_FLOAT ) & 1u ) : ( hash3( mkey, Seed, SALT_ZEBRA_V ) & 1u );
			int a     = r % Scan;
			if( uint( ( a >> k ) & 1 ) != want )
				v = vec3( 0.0 );
			else if( ( Hooks & 8 ) == 0 )
			{
				int partner = r ^ ( 1 << k );
				if( partner < rows )
					v += received( ivec2( col, top + partner ), t, lost );
			}
		}
	}
	if( TDeadRow > 0u && hash3( mkey, Seed, SALT_ROW ) < TDeadRow
	    && r % Scan == int( hash3( mkey, Seed, SALT_ROW_A ) % uint( Scan ) ) )
		v = vec3( 0.0 );
	if( TColourLoss > 0u && hash3( mkey, Seed, SALT_COLOUR ) < TColourLoss )
	{
		uint h    = hash3( mkey, Seed, SALT_COLOUR_C );
		int chips = ( m1.x - m0.x + 15 ) / 16;
		if( c >= 16 * int( ( h >> 2u ) % uint( chips ) ) )
			v[ int( h % 3u ) ] = 0.0;
	}
	if( TDeadModule > 0u && hash3( mkey, Seed, SALT_DEAD_MOD ) < TDeadModule )
		v = vec3( 0.0 );

	uint deadAt = ( Hooks & 512 ) != 0 && TDeadLed < 0x80000000u ? TDeadLed * 2u : TDeadLed;
	if( deadAt > 0u && hash3( pack2( led ), Seed, SALT_DEAD_LED ) < deadAt )
		v = vec3( 0.0 );

	//The calibration table: the tile's batch (Tile Spread in brightness,
	//Colour Spread in colour), then its own module's (Module Spread in both),
	//so Batches K with Module Spread 0 is exactly K looks.
	ivec2 ct  = ( Hooks & 2 ) != 0 ? min( ( led + 1 ) / Tile, Tiles - 1 ) : t;
	uint tkey = Batches > 0 && ( Hooks & 4 ) == 0 ? tileHash( ivec2( 0 ), SALT_BATCH ) + tileHash( ct, SALT_BATCH ) % uint( Batches )
	                                              : pack2( ct ) | 0x80000000u;
	vec3 gain = ( 1.0 - TileSpread * unit( hash3( tkey, Seed, SALT_GAIN ) ) ) * ( vec3( 1.0 ) - ColourSpread * tint( tkey, SALT_TINT ) );
	gain *= ( 1.0 - ModuleSpread * unit( hash3( mkey, Seed, SALT_MGAIN ) ) ) * ( vec3( 1.0 ) - 0.4 * ModuleSpread * tint( mkey, SALT_MTINT ) );

	if( Seams > 0.0 )
	{
		float s = 1.0;
		if( local.x == 0 && t.x > 0 )
			s *= seamGain( pack2( t - ivec2( 1, 0 ) ), SALT_VSEAM );
		if( local.x == Tile.x - 1 && t.x < Tiles.x - 1 )
			s *= seamGain( pack2( t ), SALT_VSEAM );
		if( local.y == 0 && t.y > 0 )
			s *= seamGain( pack2( t - ivec2( 0, 1 ) ), SALT_HSEAM );
		if( local.y == Tile.y - 1 && t.y < Tiles.y - 1 )
			s *= seamGain( pack2( t ), SALT_HSEAM );
		gain *= s;
	}

	vec4 state = texelFetch( StateTex, t, 0 );
	gain *= vec3( 1.0 ) - Heat * state.r * vec3( 1.0, 0.3, 0.15 );
	v *= gain;

	if( TStuckLed > 0u && hash3( pack2( led ), Seed, SALT_STUCK ) < TStuckLed )
		v[ int( hash3( pack2( led ), Seed, SALT_STUCK_C ) % 3u ) ] = 1.0;

	float power = 1.0;
	if( PsuMode == 0 && state.b > PsuLimit )
		power = ( Hooks & 128 ) != 0 ? sqrt( PsuLimit / state.b ) : PsuLimit / state.b;
	if( PsuMode == 1 && state.a > 0.5 )
		power = 0.0;
	if( TDeadTile > 0u && tileHash( t, SALT_DEAD ) < TDeadTile )
		power = 0.0;
	if( TFlicker > 0u && tileHash( t, SALT_FLICKER ) < TFlicker )
	{
		uint slot = slotOf( FlickerBase, FlickerFrac, unit( tileHash( t, SALT_FLICK_P ) ) );
		if( ( hash4( pack2( t ), slot, Seed, SALT_FLICK_ON ) & 1u ) != 0u )
			power = 0.0;
	}

	fragColor = vec4( clamp( v * power, 0.0, 1.0 ), 1.0 );
}
)";

//---------------------------------------------------------------------------
// 7. display: to the host, in pixels. Each pixel takes its LED times the
// share of the pixel the emitter covers -- a centred square of area Fill, so
// at Fill 1 the share is exactly 1 -- and the clip comes back under Mix. The
// wall is a display, so it is opaque: alpha goes to 1 with Mix.
//---------------------------------------------------------------------------
const char* const kDisplayFragment = R"(
uniform sampler2D PanelTex;
uniform sampler2D InputTexture;
uniform ivec2 Size;
uniform int Pitch;
uniform float EmitHalf;// half the emitter's side, in pixels
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

void main()
{
	ivec2 px     = clamp( ivec2( floor( uv * vec2( Size ) ) ), ivec2( 0 ), Size - 1 );
	int down     = Size.y - 1 - px.y;
	ivec2 led    = ivec2( px.x / Pitch, down / Pitch );
	vec2 offset  = vec2( float( px.x - led.x * Pitch ), float( down - led.y * Pitch ) );
	float centre = 0.5 * float( Pitch );
	vec2 lo      = max( offset, vec2( centre - EmitHalf ) );
	vec2 hi      = min( offset + 1.0, vec2( centre + EmitHalf ) );
	vec2 cover   = max( hi - lo, vec2( 0.0 ) );
	vec3 lit     = texelFetch( PanelTex, led, 0 ).rgb * ( cover.x * cover.y );
	vec4 src     = texelFetch( InputTexture, px, 0 );
	fragColor    = vec4( mix( src.rgb, lit, MixAmount ), mix( src.a, 1.0, MixAmount ) );
}
)";

std::string Assemble( Pass pass )
{
	std::string out = kVersion;
	out += kCommon;
	switch( pass )
	{
	case Pass::Wall: out += kWallFragment; break;
	case Pass::Copy: out += kCopyFragment; break;
	case Pass::Route: out += kRouteFragment; break;
	case Pass::Hold: out += kHoldFragment; break;
	case Pass::Stats:
		out += kSignal;
		out += kStatsFragment;
		break;
	case Pass::Panel:
		out += kSignal;
		out += kPanelFragment;
		break;
	case Pass::Display: out += kDisplayFragment; break;
	}
	return out;
}

} // namespace patchwork::shaders
