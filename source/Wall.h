#pragma once

#include "Controls.h"

#include <cstdint>

/**
    The wall's geometry and its cable, in integers. No GL.

    The shaders compute the same functions per LED (`kCommon` in Shaders.cpp);
    this copy is what the plugin uses for the motion and what the harness
    measures the GPU against. Where the two could disagree, `pwtest --chain`
    measures the GPU's chain order out of the picture -- a cut arriving one
    hop at a time -- and compares it with this one, and `--chain-law` (no GL)
    checks the properties this one claims: a bijection onto the tiles, and
    consecutive positions adjacent on the snakes.

    Everything is top-down: tile row 0 and LED row 0 are the TOP of the
    picture, as an LED processor counts them. Only the display pass and the
    harness's read-back ever think about GL's bottom-up rows.
*/
namespace patchwork::wall
{
struct Layout
{
	int ledsW = 0, ledsH = 0;  ///< LEDs across and down: ceil( picture / pitch )
	int tileW = 1, tileH = 1;  ///< LEDs per tile
	int cols = 0, rows = 0;    ///< tiles across and down: ceil( leds / tile )
	int perPort = 1;           ///< tiles per chain (Tiles Per Port, 0 resolved to all)
	Route route   = Route::RowSnake;
	Corner corner = Corner::TopLeft;

	int Tiles() const
	{
		return cols * rows;
	}
	/// The along-the-cable axis is x for the row routes, y for the columns.
	bool AlongX() const
	{
		return route == Route::RowSnake || route == Route::Rows;
	}
	/// LEDs along the cable through one tile.
	int AlongLength() const
	{
		return AlongX() ? tileW : tileH;
	}
};

Layout MakeLayout( int width, int height, int pitch, int tileW, int tileH, int perPort, Route route, Corner corner );

/// Where a tile sits on its cable.
struct Link
{
	int port   = 0;///< which chain
	int pos    = 0;///< 0 at the sending card
	int length = 0;///< tiles on this chain
};

/// 0 .. Tiles()-1: the order the cable visits the tiles, all ports end to end.
int LinearIndex( const Layout& layout, int tx, int ty );
/// The inverse.
void TileAt( const Layout& layout, int linear, int& tx, int& ty );
Link LinkOf( const Layout& layout, int tx, int ty );
/// +1 when the cable runs through this tile towards +x (row routes) or
/// downwards (column routes), in screen terms; -1 the other way.
int TravelOf( const Layout& layout, int tx, int ty );

/**
    The repeat's motion: one phase in tiles, integrated in double every frame
    (so a change of Repeat Speed never jumps the content), turned into an
    offset in whole LEDs along the cable, reduced modulo the window.
*/
class RepeatMotion
{
public:
	void Advance( double tilesPerSecond, double dt )
	{
		phase += tilesPerSecond * dt;
	}
	void Reset()
	{
		phase = 0.0;
	}
	double Phase() const
	{
		return phase;
	}
	void SetPhase( double tiles )
	{
		phase = tiles;
	}

	/// The offset m in LEDs, 0 <= m < window x along.
	int OffsetLeds( Motion motion, int window, int along, uint32_t seed ) const;

private:
	double phase = 0.0;
};

/// The integer time-slot a feature is in: floor( t / period ) split so the
/// shader can add a per-tile phase without ever holding a large float.
struct Slot
{
	uint32_t base = 0;   ///< floor( t / period ), modulo 2^32
	float fraction = 0.0f;///< t / period - floor, in [0, 1)
};
Slot SlotOf( double seconds, double period );

} // namespace patchwork::wall
