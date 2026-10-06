#pragma once

#include "Controls.h"
#include "PassBuffer.h"
#include "Wall.h"

#include <FFGLSDK.h>

#include "StoatworksAboutParams.h"

#include <string>

/**
	Patchwork -- a faulty LED wall, as an FFGL effect.

	**The one idea.** An LED wall is not one display. It is a few hundred
	small ones on a daisy chain: a processor cuts the picture into cabinet
	rectangles and sends them down a cable that snakes from cabinet to
	cabinet; each cabinet's receiving card takes its rectangle by its place on
	the chain, applies its own calibration table, and drives its modules one
	scan line at a time through an address decoder and a row of shift-register
	driver chips. Every failure an operator sees is one stage of that chain
	lying -- see the table in README.md.

	**Seven passes**, in `Shaders.h`, all at LED resolution but the last. The
	CPU's share is the time: the clock (pitch's unit vote), the repeat's
	phase in double, the slots the random processes are in, and the heat's
	alpha. `Wall.h` has the geometry the harness checks the GPU against.
*/
class Patchwork : public CFFGLPlugin
{
public:
	Patchwork();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- for the harness ------------------------------------------------------
	/// The offline harness DECLARES its clock unit rather than leaving the
	/// vote to infer one from a synthetic clock.
	void SetClockScaleForTest( double scale );
	/// Negative-control hooks: `shaders::Hook` bits for the GLSL, and the
	/// CPU's own two. All zero in a shipped instance.
	void SetHooksForTest( int glslHooks );
	void SetHeatEulerForTest( bool euler );
	void SetResizeKeepsStateForTest( bool keep );

	/// The geometry of the last frame, and its buffers, for read-back.
	const patchwork::wall::Layout& CurrentLayout() const
	{
		return layout;
	}
	GLuint PanelTextureID() const
	{
		return panelPass.TextureID();
	}
	GLuint StateTextureID() const
	{
		return statePass[ stateIndex ].TextureID();
	}
	int RingDepth() const
	{
		return ringDepth;
	}
	double RepeatPhase() const
	{
		return motion.Phase();
	}

	static constexpr unsigned int ParamCount()
	{
		return patchwork::PT_COUNT;
	}

private:
	/// The host's clock in seconds, whatever unit it arrived in.
	double nowSeconds();

	bool ensureRing( int ledsW, int ledsH, int depth );
	void releaseRing();

	ffglex::FFGLShader wallShader, copyShader, routeShader, holdShader, statsShader, panelShader, displayShader;
	ffglex::FFGLScreenQuad quad;

	patchwork::PassBuffer wallPass;      ///< LED res: the clip resampled
	patchwork::PassBuffer routePass;     ///< LED res: what each card is sent
	patchwork::PassBuffer holdPass[ 2 ]; ///< LED res: what each card last received
	patchwork::PassBuffer statePass[ 2 ];///< tile res: heat, hiccup, mean drive
	patchwork::PassBuffer panelPass;     ///< LED res: what each LED emits
	int holdIndex  = 0;
	int stateIndex = 0;
	bool holdValid  = false;
	bool stateValid = false;

	/// The ring of past walls, a 2D array texture, one layer per frame.
	GLuint ringTexture = 0, ringFBO = 0;
	/// A 1 x 1 x 1 array bound to the ring's unit while there is no ring: a
	/// sampler2DArray on an empty unit is "unloadable" to Apple's driver and
	/// undefined to the spec, even when no fragment reads it.
	GLuint emptyRing = 0;
	int ringW = 0, ringH = 0, ringDepth = 0;
	int ringWrite  = 0;
	int ringFilled = 0;

	patchwork::wall::Layout layout;
	patchwork::wall::RepeatMotion motion;
	uint32_t frameCounter = 0;

	//--- the clock (readout's unit voting, via pitch) --------------------------
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastNow      = -1.0;
	int clockFrames     = 0;

	//--- test hooks ----------------------------------------------------------
	int hooks              = 0;
	bool heatEuler         = false;
	bool resizeKeepsState  = false;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ patchwork::PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
