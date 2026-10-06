#include "Wall.h"

#include "Hash.h"

#include <algorithm>
#include <cmath>

namespace patchwork::wall
{
namespace
{
int ceilDiv( int a, int b )
{
	return b > 0 ? ( a + b - 1 ) / b : 0;
}

bool flipX( Corner corner )
{
	return corner == Corner::TopRight || corner == Corner::BottomRight;
}
bool flipY( Corner corner )
{
	return corner == Corner::BottomLeft || corner == Corner::BottomRight;
}
} // namespace

Layout MakeLayout( int width, int height, int pitch, int tileW, int tileH, int perPort, Route route, Corner corner )
{
	Layout layout;
	pitch        = std::max( pitch, 1 );
	layout.ledsW = ceilDiv( std::max( width, 1 ), pitch );
	layout.ledsH = ceilDiv( std::max( height, 1 ), pitch );
	layout.tileW = std::max( tileW, 1 );
	layout.tileH = std::max( tileH, 1 );
	layout.cols  = ceilDiv( layout.ledsW, layout.tileW );
	layout.rows  = ceilDiv( layout.ledsH, layout.tileH );
	layout.route  = route;
	layout.corner = corner;
	layout.perPort = perPort > 0 ? std::min( perPort, layout.Tiles() ) : layout.Tiles();
	return layout;
}

int LinearIndex( const Layout& l, int tx, int ty )
{
	const int x = flipX( l.corner ) ? l.cols - 1 - tx : tx;
	const int y = flipY( l.corner ) ? l.rows - 1 - ty : ty;
	switch( l.route )
	{
	case Route::RowSnake: return y * l.cols + ( ( y & 1 ) == 0 ? x : l.cols - 1 - x );
	case Route::ColumnSnake: return x * l.rows + ( ( x & 1 ) == 0 ? y : l.rows - 1 - y );
	case Route::Rows: return y * l.cols + x;
	case Route::Columns:
	default: return x * l.rows + y;
	}
}

void TileAt( const Layout& l, int linear, int& tx, int& ty )
{
	int x = 0, y = 0;
	switch( l.route )
	{
	case Route::RowSnake:
		y = linear / l.cols;
		x = linear - y * l.cols;
		if( ( y & 1 ) != 0 )
			x = l.cols - 1 - x;
		break;
	case Route::ColumnSnake:
		x = linear / l.rows;
		y = linear - x * l.rows;
		if( ( x & 1 ) != 0 )
			y = l.rows - 1 - y;
		break;
	case Route::Rows:
		y = linear / l.cols;
		x = linear - y * l.cols;
		break;
	case Route::Columns:
	default:
		x = linear / l.rows;
		y = linear - x * l.rows;
		break;
	}
	tx = flipX( l.corner ) ? l.cols - 1 - x : x;
	ty = flipY( l.corner ) ? l.rows - 1 - y : y;
}

Link LinkOf( const Layout& l, int tx, int ty )
{
	const int linear = LinearIndex( l, tx, ty );
	Link link;
	link.port   = linear / l.perPort;
	link.pos    = linear - link.port * l.perPort;
	link.length = std::min( l.perPort, l.Tiles() - link.port * l.perPort );
	return link;
}

int TravelOf( const Layout& l, int tx, int ty )
{
	const int x = flipX( l.corner ) ? l.cols - 1 - tx : tx;
	const int y = flipY( l.corner ) ? l.rows - 1 - ty : ty;
	switch( l.route )
	{
	case Route::RowSnake: return ( ( y & 1 ) == 0 ? 1 : -1 ) * ( flipX( l.corner ) ? -1 : 1 );
	case Route::ColumnSnake: return ( ( x & 1 ) == 0 ? 1 : -1 ) * ( flipY( l.corner ) ? -1 : 1 );
	case Route::Rows: return flipX( l.corner ) ? -1 : 1;
	case Route::Columns:
	default: return flipY( l.corner ) ? -1 : 1;
	}
}

int RepeatMotion::OffsetLeds( Motion motion, int window, int along, uint32_t seed ) const
{
	if( window <= 0 || along <= 0 )
		return 0;
	const long long span = static_cast< long long >( window ) * along;
	long long m          = 0;
	switch( motion )
	{
	case Motion::Hold: m = 0; break;
	case Motion::Step: m = static_cast< long long >( std::floor( phase ) ) * along; break;
	case Motion::Scroll: m = static_cast< long long >( std::floor( phase * along ) ); break;
	case Motion::PingPong:
	{
		//Out the length of the window and back: a triangle in tiles.
		const double period = 2.0 * window;
		const double u      = phase - period * std::floor( phase / period );
		const double tiles  = u < window ? u : period - u;
		m                   = static_cast< long long >( std::floor( tiles * along ) );
		break;
	}
	case Motion::Random:
	{
		//A new whole-tile offset each time the phase crosses a whole tile.
		const uint32_t slot = static_cast< uint32_t >( static_cast< long long >( std::floor( std::fabs( phase ) ) ) );
		m = static_cast< long long >( Pcg( slot ^ Pcg( seed ^ 0x52e9a1c3u ) ) % static_cast< uint32_t >( window ) ) * along;
		break;
	}
	default: break;
	}
	m %= span;
	if( m < 0 )
		m += span;
	return static_cast< int >( m );
}

Slot SlotOf( double seconds, double period )
{
	Slot slot;
	if( !( period > 0.0 ) )
		return slot;
	const double u     = seconds / period;
	const double whole = std::floor( u );
	slot.base          = static_cast< uint32_t >( static_cast< unsigned long long >( static_cast< long long >( whole ) ) & 0xFFFFFFFFull );
	slot.fraction      = static_cast< float >( u - whole );
	//A fraction that rounds up to 1.0 in float is a carry.
	if( slot.fraction >= 1.0f )
	{
		slot.fraction = 0.0f;
		++slot.base;
	}
	return slot;
}

} // namespace patchwork::wall
