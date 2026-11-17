/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#ifndef __RENDERBACKEND_H__
#define __RENDERBACKEND_H__

/*
===============================================================================

	idRenderBackend: the graphics-API-neutral interface of the renderer.

	All shared render code talks to one object, renderBackend. The cvar r_renderApi picks its
	class once at startup through idRenderBackendFactory, so a new backend is a new folder and
	lines in sources.cmake, with no #ifdef in shared code.

	  game, front end (game thread)            frame back end (main thread)
	  -----------------------------            ----------------------------------------------
	  idRenderSystemLocal, idRenderWorld       command loop, stereo compositor
	  idVertexCache (writes into the mapped    tr_backend_draw.cpp          the passes
	   set of the frame)                       tr_backend_rendertools.cpp   debug tools
	  idImage, idRenderProgManager, idMaterial RenderLog.cpp                r_pix
	        |  handles                                |  State / Cull / BindTexture / BindProgram / DrawIndexed
	        v                                         v
	  +-----------------------------------------------------------------------------------+
	  | idRenderBackend  (RenderBackend.h, RenderBackend.cpp)                             |
	  |   renderLatch_t + inline wrappers (shared, no API)  ->  virtual hooks API_*       |
	  |   idRenderBackendFactory + r_renderApi (one backend per run)                      |
	  +---------------+---------------------------+---------------------------+-----------+
	                  v                           v                           v
	  renderer/OpenGL/                     renderer/D3D12/ (later)     renderer/Vulkan/ (later)
	                  |                           |                           |
	                  v                           v                           v
	  sys/win32/win_glimp.cpp, win_qgl.cpp        the window (HWND) of the platform layer
	   (the WGL context)

	A draw is the latch (the GLS_* state vector, face culling, the program, the textures per slot)
	plus a drawCall_t and one virtual call, DrawIndexed. The latch lives in the base class: it is
	the record of what shared code asked for. In front of every virtual hook stands a non-virtual
	inline wrapper that drops repeats, so a repeated State, Cull or BindTexture costs one compare.
	An immediate backend (OpenGL) issues its API calls in the hooks. A pipeline backend (D3D12,
	Vulkan) leaves the change hooks empty, builds a pipelineKey_t from the latch in API_DrawIndexed
	and looks a pipeline up only when the key differs from the last one.

	Two rules of the OpenGL path change pixels, so they are part of this contract, of the latch and
	of pipelineKey_t: after a TWO_SIDED Cull face culling stays off until a one-sided Cull right after
	a TWO_SIDED one (renderLatch_t::cullEnabled; the CULL_UNSET of SetViewMirror does not turn it on),
	and the depth test is strict LESS after SetDefaultState until a State changes the depth function
	(renderLatch_t::depthFuncStrict). r_glCheckLatch 1 compares both with GL on every draw.

	Diagnostics every backend has, in the shared wrappers and the frame loop: r_debugMaxDraws N
	drops the draws of a frame after the first N, r_hashDrawStream 1 prints a hash of every frame's
	draws without handles and buffer offsets (two runs of a visual demo print the same hashes),
	r_testClearPattern 1 ends every frame with a fixed pattern of clears (tools/vdemo/check-pattern.py).

	Where OpenGL lives. Only the files of renderer/OpenGL/ and sys/win32/win_glimp.cpp and
	win_qgl.cpp include the GL headers; the precompiled header does not, so a GL type or a qgl call in
	shared code does not compile. tools/check-render-api-leaks.ps1 finds the rest (gl* functions
	that windows.h declares, GL includes, code no config builds) and fails when it finds a name:

	  ctest --test-dir build/vs2022-x64 -C Release -R renderApiLeaks

	Shared code keeps the engine's own names: the GLS_* state bits (RenderState.h), screenConfig
	(the window and the screen), renderProgManager, RB_* for the passes.

	Adding a backend:
	  1. a folder renderer/<Api>/ with a class derived from idRenderBackend that implements
	     Init .. Present and the API_* hooks; its API headers stay inside that folder;
	  2. its files in neo/sources.cmake and one static idRenderBackendFactory object with the name
	     r_renderApi selects (see gl_RenderBackend.cpp);
	  3. the window is made by sys/sys_window.h before Init and arrives as
	     renderBackendInit_t::nativeWindow (an HWND); win_glimp.cpp is the WGL context only;
	  4. the visual demos (tools/vdemo) are the check: the frames of the new backend against the
	     OpenGL ones.

	The rules of the contract:

	R1	Every method is called on the owner thread (IsOwnerThread). The exception: memory returned
		by MapBuffer may be written by any thread until UnmapBuffer.
	R2	Draws, clears, copies and latch changes are legal between BeginFrame and EndFrame. Creating,
		uploading, changing the sampler of and destroying resources is legal at any time. Nothing
		is bound or drawn between frames.
	R3	Destroy* makes the handle invalid at once and takes it out of the latch; the memory and the
		API's descriptor slots are released after the next Present.
	R4	Uploads and sampler changes inside a frame are ordered: draws issued before see the old
		contents, draws issued after see the new. A backend may run an upload to a texture not yet
		drawn with in this frame before all draws of the frame. An upload to a texture already
		drawn with in this frame is legal but breaks the pass of a pipeline backend.
	R5	A copy (CopyOutputToTexture, CopyDepthToTexture) changes the size and the contents of the
		texture for the draws after it; the handle stays. Recreating a resource inside a backend
		never changes what already issued draws sample.
	R6	MapBuffer of a FRAME buffer returns memory no unfinished draw reads; the pointer is stable
		until UnmapBuffer, which comes before the first draw that reads the buffer. A STATIC buffer
		needs the exact dirty range.
	R7	drawCall_t and its parms are read during the call only. reuseParms is the promise of shared
		code that the program and the parm values are those of the previous DrawIndexed.
	R8	Present blocks. ReadOutputPixels is legal after EndFrame and before Present and reads the
		frame just drawn.
	R9	One backend per run: r_renderApi is CVAR_INIT, vid_restart does not change the API.
	R10	A pipeline backend opens its pass at the first draw or clear after BeginFrame or after a
		copy, and closes it at a copy and in EndFrame. SetDefaultState does not close the pass.
	R11	A slot the program samples and shared code did not bind reads as opaque black.

===============================================================================
*/

#include "RenderState.h"
#include "RenderBackendTypes.h"

class idImageOpts;

// r_debugMaxDraws and r_hashDrawStream for every backend. The cvars are read once a frame, so with
// both off a draw pays one compare.
class idDrawStreamCheck {
public:
	void				BeginFrame();
	void				EndFrame();		// prints the frame's hash with r_hashDrawStream
	bool				Active() const { return active; }
	// counts and hashes the draw; false when r_debugMaxDraws drops it
	[[nodiscard]] bool	Admit( const renderLatch_t & latch, const drawCall_t & dc );

private:
	bool				active = false;
	bool				hashing = false;
	int					maxDraws = -1;
	int					numDraws = 0;
	int					numFrames = 0;
	uint64				hash = 0;
};

class idRenderBackend {
public:
	virtual					~idRenderBackend() = default;

	// ---- device: main thread, outside a frame ------------------------------------------------
	// Attaches to the window, creates the device and the output, fills caps. False = this API cannot run here.
	virtual bool			Init( const renderBackendInit_t & init ) = 0;
	// Destroys everything still alive; every handle is invalid afterwards.
	virtual void			Shutdown() = 0;
	// What this device can do; constant between Init and Shutdown.
	const renderCaps_t &	Caps() const { return caps; }
	// True on the thread that created the backend. Every other method asserts it.
	bool					IsOwnerThread() const { return Sys_GetCurrentThreadID() == ownerThread; }
	// The window's client area changed (mode switch, vid_restart). Sample count does not change.
	virtual void			ResizeOutput( int width, int height ) = 0;
	// The size of the last Init or ResizeOutput.
	int						OutputWidth() const { return outputWidth; }
	int						OutputHeight() const { return outputHeight; }
	// A hint between frames; a backend without the notion does nothing.
	virtual void			SetOption( renderOption_t option, bool enable ) = 0;
	// Display gamma tables from R_SetColorMappings. GL: device ramp; others: applied in the present blit.
	virtual void			SetGammaRamp( const unsigned short red[256], const unsigned short green[256], const unsigned short blue[256] ) = 0;
	// Reports pending API errors or validation messages to the console; may do nothing.
	virtual void			CheckErrors() = 0;
	// Prints the gfxInfo block of this API.
	virtual void			PrintInfo() const = 0;

	// ---- frame: main thread ----------------------------------------------------------------------
	// Opens the frame and starts the GPU timer. Draws, clears and copies are legal until EndFrame.
	virtual void			BeginFrame() = 0;
	// Closes the frame, hands every command to the GPU without waiting, stops the GPU timer.
	virtual void			EndFrame() = 0;
	// Shows the finished frame and blocks: on return no unfinished draw reads a FRAME buffer mapped next.
	virtual presentResult_t	Present( presentMode_t mode ) = 0;
	// GPU time of the frame closed before the last Present; blocks until known; 0 without caps.gpuTimer.
	virtual uint64			GetFrameGpuMicroseconds() = 0;
	// Which eye buffer draws go to. LEFT and RIGHT need caps.quadBufferStereo.
	virtual void			SelectOutput( outputEye_t eye ) = 0;
	// Reads the finished frame: after EndFrame, before Present. RGB8, tightly packed, bottom row first.
	virtual void			ReadOutputPixels( int x, int y, int width, int height, byte * rgb ) = 0;
	// The frame loop brackets the draws of a frame with these: after BeginFrame, before EndFrame.
	void					BeginDrawStream() { drawStream.BeginFrame(); }
	void					EndDrawStream() { drawStream.EndFrame(); }

	// ---- latched state: inside a frame ---------------------------------------------------------
	// Resets the latch: state bits 0, cull unset but enabled, strict depth test, offsets and bounds off,
	// scissor = output, texture hooks re-sent.
	void					SetDefaultState();
	// Sets the whole state vector. A draw uses the vector latched at its DrawIndexed.
	void					State( uint64 stateBits );
	// The latched vector: for asserts and for the shadow pass that carries stencil ops forward.
	uint64					GetStateBits() const { return latch.stateBits; }
	// Starts a view: the mirror flag reverses Cull; cull is unset until the next Cull().
	void					SetViewMirror( bool isMirror );
	// Face culling for the draws that follow: CT_FRONT_SIDED, CT_BACK_SIDED, CT_TWO_SIDED.
	void					Cull( int cullType );
	// The latched culling, CULL_UNSET before the first Cull() of a view: for code that changes it and puts it back.
	int						GetCull() const { return latch.cull; }
	// Depth offset = scale * slope + bias * (smallest depth step of the target); used while GLS_POLYGON_OFFSET is set.
	void					PolygonOffset( float scale, float bias );
	// Depth bounds test in window depth; ( 0, 0 ) turns it off; ignored without caps.depthBoundsTest or when zmin > zmax.
	void					DepthBoundsTest( float zmin, float zmax );
	// Viewport for the draws that follow.
	void					Viewport( int x, int y, int width, int height );
	// Scissor for the draws and clears that follow.
	void					Scissor( int x, int y, int width, int height );
	// Clears inside the latched scissor, obeying the latched write masks. A depth clear needs depth writes on.
	void					Clear( bool color, bool depth, bool stencil, byte stencilValue, float r, float g, float b, float a );

	// ---- textures --------------------------------------------------------------------------------------
	// Allocates every level of opts; contents undefined until uploaded or copied. NONE on failure.
	virtual textureHandle_t	CreateTexture( const idImageOpts & opts, textureRole_t role, const samplerDesc_t & sampler, const char * debugName ) = 0;
	// Removes the texture from the latch; the memory is released after the next Present.
	void					DestroyTexture( textureHandle_t texture );
	// Replaces one whole level (face = cube side). Draws issued earlier keep the old contents (rule R4).
	// width and height are the size the image file gives the level: a compressed cube level under 4 pixels says 4.
	// pixelPitch is the row length of pixels in pixels, 0 = width.
	virtual void			UploadTexture( textureHandle_t texture, int mipLevel, int face, int width, int height, const void * pixels, int pixelPitch ) = 0;
	// Changes filtering and addressing of the texture for the draws that follow.
	virtual void			SetTextureSampler( textureHandle_t texture, const samplerDesc_t & sampler ) = 0;
	// Makes a COLOR_COPY texture a width x height copy of that output rectangle. Earlier draws keep the old contents.
	virtual void			CopyOutputToTexture( textureHandle_t texture, int x, int y, int width, int height ) = 0;
	// The same for a DEPTH_COPY texture and the depth buffer.
	virtual void			CopyDepthToTexture( textureHandle_t texture, int x, int y, int width, int height ) = 0;
	// What the draws that follow sample on this slot for this kind. A slot keeps one 2D and one cube entry.
	void					BindTexture( int slot, textureKind_t kind, textureHandle_t texture );
	// The next BindTexture of every slot reaches the API hook even when the handle is unchanged.
	// idImage::PurgeImage calls it unconditionally; what is bound does not change.
	void					ForgetTextureBindings() { latch.texturesSent = 0; }

	// ---- buffers ----------------------------------------------------------------------------------------
	// Allocates size bytes; data may be nullptr. JOINT buffers get any tail padding the API needs.
	virtual bufferHandle_t	CreateBuffer( bufferType_t type, bufferUsage_t usage, int size, const void * data ) = 0;
	// Releases the buffer after the next Present.
	virtual void			DestroyBuffer( bufferHandle_t buffer ) = 0;
	// Maps the whole buffer for writing only. The pointer may be written by any thread until UnmapBuffer.
	// No unfinished draw reads the memory returned for a FRAME buffer.
	virtual void *			MapBuffer( bufferHandle_t buffer ) = 0;
	// Ends the mapping; only [dirtyOffset, dirtyOffset + dirtyBytes) was written since MapBuffer.
	virtual void			UnmapBuffer( bufferHandle_t buffer, int dirtyOffset, int dirtyBytes ) = 0;

	// ---- programs ---------------------------------------------------------------------------------------
	// Loads one stage of base/renderprogs/<name> in the backend's own form. NONE when it is missing or fails.
	virtual shaderHandle_t	CreateShaderStage( shaderStageType_t stage, const char * name, shaderStageInfo_t & info ) = 0;
	// Pairs two stages. NONE on failure.
	virtual programHandle_t	CreateProgram( shaderHandle_t vertex, shaderHandle_t fragment, const char * debugName, programInfo_t & info ) = 0;
	virtual void			DestroyShaderStage( shaderHandle_t shader ) = 0;
	virtual void			DestroyProgram( programHandle_t program ) = 0;
	// The program for the draws that follow; NONE = none, a draw is then illegal.
	void					BindProgram( programHandle_t program );

	// ---- draw -------------------------------------------------------------------------------------------
	// Draws with the latched state, program and textures. Reads dc and dc.parms during the call only.
	void					DrawIndexed( const drawCall_t & dc );

	// ---- GPU timestamps and debug groups (r_pix) --------------------------------------------------------
	// Records the GPU time of this point of the frame into slot index of a set (0 .. TIMESTAMP_SETS - 1).
	virtual void			WriteTimestamp( int set, int index ) = 0;
	// True when slot index of the set can be read without blocking.
	virtual bool			TimestampReady( int set, int index ) = 0;
	// Reads count slots of the set in nanoseconds; legal after TimestampReady of its last written slot.
	virtual void			ReadTimestamps( int set, int count, uint64 * nanoseconds ) = 0;
	// Releases every timestamp slot.
	virtual void			FreeTimestamps() = 0;
	// Named group for capture tools; nests up to caps.maxDebugGroupDepth; nothing without caps.debugGroups.
	virtual void			PushDebugGroup( const char * name ) = 0;
	virtual void			PopDebugGroup() = 0;

protected:
	// Hooks of an immediate backend (GL): called only for a real change, after the latch is updated.
	// A pipeline backend leaves them empty and reads the latch in API_DrawIndexed.
	virtual void			API_SetDefaultState() {}
	virtual void			API_State( uint64 stateBits, uint64 changedBits ) {}
	virtual void			API_BeginView() {}
	virtual void			API_Cull( int cullType, int previousCullType ) {}
	virtual void			API_PolygonOffset( float scale, float bias ) {}
	virtual void			API_DepthBoundsTest( float zmin, float zmax ) {}
	virtual void			API_Viewport( const renderRect_t & rect ) {}
	virtual void			API_Scissor( const renderRect_t & rect ) {}
	virtual void			API_BindTexture( int slot, textureKind_t kind, textureHandle_t texture ) {}
	virtual void			API_BindProgram( programHandle_t program ) {}
	// Every backend implements these.
	virtual void			API_Clear( const clearDesc_t & clear ) = 0;
	virtual void			API_DestroyTexture( textureHandle_t texture ) = 0;
	virtual void			API_DrawIndexed( const drawCall_t & dc ) = 0;

	// The shared part of a pipeline key; the backend adds program, target format and depth-bias slot.
	pipelineKey_t			PipelineKey( const drawCall_t & dc ) const;

	renderLatch_t			latch;
	idDrawStreamCheck		drawStream;
	renderCaps_t			caps = {};
	uintptr_t				ownerThread = 0;
	int						outputWidth = 0;
	int						outputHeight = 0;
	bool					inFrame = false;	// between BeginFrame and EndFrame; set by the backend, read by asserts
};

// One static instance per backend; shared code never names a backend class.
class idRenderBackendFactory {
public:
								idRenderBackendFactory( const char * apiName, idRenderBackend * ( *create )() );
	static idRenderBackend *	Create( const char * apiName );		// nullptr when no backend has that name
	static void					ListApis( idStrList & names );
private:
	const char *				apiName;
	idRenderBackend *			( *create )();
	idRenderBackendFactory *	next;
	static idRenderBackendFactory *	staticFactories;	// the idCVar::staticVars pattern, safe before main
};

extern idRenderBackend *	renderBackend;	// created in R_InitOpenGL, destroyed in idRenderSystemLocal::Shutdown
extern idCVar				r_renderApi;
extern idCVar				r_useStateCaching;
extern idCVar				r_debugMaxDraws;		// RenderSystem_init.cpp, like the two below
extern idCVar				r_hashDrawStream;
extern idCVar				r_testClearPattern;

/*
===============================================================================

	The hot path: every wrapper keeps the repeat test the OpenGL path had.

===============================================================================
*/

// with caching off every group is applied; an unchanged vector costs one compare
ID_INLINE void idRenderBackend::State( uint64 stateBits ) {
	uint64 changed = stateBits ^ latch.stateBits;
	if ( !r_useStateCaching.GetBool() ) {
		changed = ~0ull;
	} else if ( changed == 0 ) {
		return;
	}
	latch.stateBits = stateBits;
	if ( ( changed & GLS_DEPTHFUNC_BITS ) != 0 ) {
		latch.depthFuncStrict = false;
	}
	API_State( stateBits, changed );
}

ID_INLINE void idRenderBackend::SetViewMirror( bool isMirror ) {
	latch.viewIsMirror = isMirror;
	latch.cull = CULL_UNSET;
	API_BeginView();
}

// the hook gets the previous value: GL applies the enable rule of latch.cullEnabled itself
ID_INLINE void idRenderBackend::Cull( int cullType ) {
	if ( latch.cull == cullType ) {
		return;
	}
	const int previous = latch.cull;
	latch.cull = cullType;
	if ( cullType == CT_TWO_SIDED ) {
		latch.cullEnabled = false;
	} else if ( previous == CT_TWO_SIDED ) {
		latch.cullEnabled = true;
	}
	API_Cull( cullType, previous );
}

ID_INLINE void idRenderBackend::PolygonOffset( float scale, float bias ) {
	latch.polyOfsScale = scale;
	latch.polyOfsBias = bias;
	if ( ( latch.stateBits & GLS_POLYGON_OFFSET ) != 0 ) {
		API_PolygonOffset( scale, bias );
	}
}

ID_INLINE void idRenderBackend::DepthBoundsTest( float zmin, float zmax ) {
	if ( !caps.depthBoundsTest || zmin > zmax ) {
		return;
	}
	latch.depthBoundsMin = zmin;
	latch.depthBoundsMax = zmax;
	API_DepthBoundsTest( zmin, zmax );
}

// viewport and scissor are not filtered
ID_INLINE void idRenderBackend::Viewport( int x, int y, int width, int height ) {
	latch.viewport = { x, y, width, height };
	API_Viewport( latch.viewport );
}

// a negative size is an error in GL (nothing changes) and has no meaning elsewhere
ID_INLINE void idRenderBackend::Scissor( int x, int y, int width, int height ) {
	if ( width < 0 || height < 0 ) {
		return;
	}
	latch.scissor = { x, y, width, height };
	API_Scissor( latch.scissor );
}

// skipped only when the hook has seen this very entry
ID_INLINE void idRenderBackend::BindTexture( int slot, textureKind_t kind, textureHandle_t texture ) {
	const int k = static_cast<int>( kind );
	const uint32 bit = 1u << ( slot * 2 + k );
	if ( ( latch.texturesSent & bit ) != 0 && latch.textures[slot][k] == texture ) {
		return;
	}
	latch.textures[slot][k] = texture;
	latch.texturesSent |= bit;
	API_BindTexture( slot, kind, texture );
}

// not filtered here: idRenderProgManager::BindShader keeps its index compare
ID_INLINE void idRenderBackend::BindProgram( programHandle_t program ) {
	latch.program = program;
	API_BindProgram( program );
}

ID_INLINE void idRenderBackend::DrawIndexed( const drawCall_t & dc ) {
	assert( IsOwnerThread() && inFrame );
	assert( latch.cull != CULL_UNSET );					// Cull() must follow SetViewMirror and SetDefaultState
	assert( latch.program != programHandle_t::NONE );
	if ( drawStream.Active() && !drawStream.Admit( latch, dc ) ) {
		return;
	}
	API_DrawIndexed( dc );
}

// raster cull: 1 = cull the faces GL calls GL_FRONT (the engine winds clockwise), 2 = GL_BACK; a mirror
// swaps them. A one-sided cull counts only while latch.cullEnabled, as in GL.
ID_INLINE pipelineKey_t idRenderBackend::PipelineKey( const drawCall_t & dc ) const {
	pipelineKey_t key = {};
	key.stateBits = latch.stateBits & GLS_PIPELINE_BITS;
	key.layout = dc.layout;
	int cull = 0;
	if ( latch.cullEnabled && latch.cull != CT_TWO_SIDED ) {
		cull = ( ( latch.cull == CT_BACK_SIDED ) != latch.viewIsMirror ) ? 2 : 1;
	}
	key.raster = static_cast<uint8>( cull | ( dc.topology == primitive_t::LINES ? 4 : 0 ) | ( latch.depthFuncStrict ? 64 : 0 ) );
	return key;
}

#endif /* !__RENDERBACKEND_H__ */
