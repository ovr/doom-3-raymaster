// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "d3d12_Descriptors.h"
#include "d3d12_Formats.h"
#include "d3d12_HandleTable.h"
#include "d3d12_Memory.h"

/*
	Textures: one committed resource per textureHandle_t, created in COPY_DEST with every level and face
	(a cube is a 2D array of 6 in GL's face order, subresource = level + face * levels). Uploads are copies
	recorded in the open list (R4); a texture leaves COPY_DEST only when a draw binds it (MakeReadable).
	Rows are never flipped: row 0 of an upload is row 0 of the texture, as in GL.

	With RecordUploads( true ) every upload keeps a hash of the bytes it copied after conversion (the
	blocks, idDxtDecoder's output, the swapped 565 words), and idD3D12TextureCheck reads every such
	subresource back and hashes it again: d3d12_checkImages and r_d3d12CheckImages.

	A copy (CopyOutputToTexture, CopyDepthToTexture) redefines level 0 at the size of its box, as
	glCopyTexImage2D does. The texture keeps the surfaces of its last two sizes: the current one and a
	spare, swapped back in when a copy has its size again, so resolution scaling never recreates one.
	A third size buries the spare after the open list (R5). Levels 1 and up of GL's texture keep the
	size of its creation, so after a copy at another size GL sees an incomplete texture under a mipmap
	filter and samples (0,0,0,1): SampledView gives the black fallback then (T6).
*/

// 64-bit hash of bytes, continued from hash; 8 bytes a step. The upload and the check hash the same rows.
[[nodiscard]] uint64				D3D12_HashBytes( uint64 hash, std::span<const byte> bytes );
constexpr uint64					D3D12_HASH_SEED = 0x9E3779B97F4A7C15ull;

// The last upload of one subresource, as the check compares it.
struct d3d12UploadRecord_t {
	uint64							hash = D3D12_HASH_SEED;
	uint32							rowBytes = 0;		// of each row it wrote, from x = 0
	uint32							rows = 0;			// pixel rows, or block rows of a BC resource
	bool							uploaded = false;
};

struct d3d12TextureSurface_t {
	idComPtr<ID3D12Resource>		resource;
	d3d12ResourceState_t			state;
	d3d12Descriptor_t				srv;			// in the SRV pool, with the format's mapping
	d3d12Descriptor_t				rtv;			// a format with an RTV only (DEPTH_COPY)
	int								width = 0;
	int								height = 0;
	int								levels = 0;
	int								faces = 0;
	uint64							bytes = 0;		// GPU memory of the resource
};

// What a copy writes into its target.
enum class d3d12CopyKind_t : uint8 {
	COLOR,		// the scene's colour: an R8G8B8A8_UNORM 2D texture (COLOR_COPY)
	DEPTH		// the scene's depth through the depth-copy pass: an R32_FLOAT 2D texture with an RTV (DEPTH_COPY)
};

// What a textureHandle_t names.
struct d3d12Texture_t {
	idImageOpts						opts;			// as created: a copy changes the surface only
	textureRole_t					role = textureRole_t::SAMPLED;
	textureKind_t					kind = textureKind_t::TEX_2D;
	d3d12FormatInfo_t				format;
	d3d12TextureSurface_t			surface;		// what draws sample
	d3d12TextureSurface_t			spare;			// copy targets: the surface of the other recent size
	uint64							sampler = 0;	// D3D12_PackSampler of the last SetSampler; a colour copy makes it LINEAR + CLAMP
	uint32							samplerSlot = 0;	// sampler's slot in the shader-visible sampler heap
	bool							copiedResized = false;	// the last copy had another size than the creation (T6)
	idStr							name;
	std::vector<d3d12UploadRecord_t>	uploads;	// by subresource of surface, while uploads are recorded

	// GL's texture is incomplete: level 0 does not match the mips a mipmap filter reads.
	[[nodiscard]] bool				IncompleteCopy() const;
};

class idD3D12TextureStore {
public:
	// unalignedBlocks: OPTIONS8 (the device's, or false to test the decoding path).
									idD3D12TextureStore( const idD3D12Device & device, idD3D12Graveyard & graveyard, idD3D12DescriptorPool & srvPool,
										idD3D12DescriptorPool & rtvPool, idD3D12SamplerHeap & samplers, idD3D12Uploader & uploader,
										idD3D12Barriers & barriers, bool unalignedBlocks );
	void							CreateDefaults();	// R11: the opaque black 2D and cube an unbound slot samples
	// Every level of opts; contents undefined until uploaded. NONE for FMT_NONE, a lost device or a full table.
	[[nodiscard]] textureHandle_t	Create( const idImageOpts & opts, textureRole_t role, const samplerDesc_t & sampler, const char * name );
	void							Destroy( textureHandle_t texture );	// the resource and its views die after the open list
	// One level of one face (R4); width, height and pixelPitch as UploadTexture gives them.
	void							Upload( textureHandle_t texture, int mipLevel, int face, int width, int height, const void * pixels,
										int pixelPitch );
	void							SetSampler( textureHandle_t texture, const samplerDesc_t & sampler );
	[[nodiscard]] d3d12Texture_t *	Find( textureHandle_t texture ) { return table.Find( texture ); }
	[[nodiscard]] d3d12Texture_t &	Get( textureHandle_t texture ) { return table.Get( texture ); }
	[[nodiscard]] textureHandle_t	FindByName( const char * name );	// case-insensitive; NONE when there is none
	// The surface a copy of a width x height box writes: the current one, the spare, or a new one-level surface.
	// A colour copy sets LINEAR and CLAMP, as GL's copy does. nullptr for a dead handle or a texture of another kind.
	[[nodiscard]] d3d12TextureSurface_t *	PrepareCopyTarget( textureHandle_t texture, d3d12CopyKind_t kind, int width, int height );
	// The view a slot of this kind samples: the texture's, or the black fallback for NONE, a dead handle, the
	// other kind or an incomplete copy.
	[[nodiscard]] d3d12Descriptor_t	SampledView( textureHandle_t texture, textureKind_t kind );
	[[nodiscard]] d3d12Descriptor_t	Fallback( textureKind_t kind ) const { return fallbacks[static_cast<size_t>( kind )].srv; }
	void							MakeReadable( textureHandle_t texture );	// PIXEL_SHADER_RESOURCE, batched
	void							RecordUploads( bool record ) { recordUploads = record; }
	// visit( handle, texture ) in handle index order; it must not create or destroy.
	template<typename F> void		ForEach( F && visit ) { table.ForEach( std::forward<F>( visit ) ); }
	void							DestroyAll();		// Shutdown, the GPU idle
	[[nodiscard]] int				Num() const { return table.Num(); }
	[[nodiscard]] uint64			Bytes() const { return bytes; }
	[[nodiscard]] int				CopySurfacesThisFrame() const { return copySurfaces; }	// P1: 0 while the copy sizes repeat
	void							ResetFrameCounters() { copySurfaces = 0; }

private:
	[[nodiscard]] bool				CreateSurface( d3d12TextureSurface_t & surface, const d3d12FormatInfo_t & format, textureKind_t kind,
										int width, int height, int levels, const char * name );
	void							BurySurface( d3d12TextureSurface_t & surface );	// after the open list; its bytes leave the total
	// The rows of one region, converted, into staging and the copy; returns what the check compares.
	[[nodiscard]] d3d12UploadRecord_t	CopyRegion( d3d12TextureSurface_t & surface, const d3d12FormatInfo_t & format, UINT subresource,
										bool wholeLevel, int width, int height, const byte * source, size_t sourcePitch, bool record );

	const idD3D12Device &			device;
	idD3D12Graveyard &				graveyard;
	idD3D12DescriptorPool &			srvPool;
	idD3D12DescriptorPool &			rtvPool;
	idD3D12SamplerHeap &			samplers;
	idD3D12Uploader &				uploader;
	idD3D12Barriers &				barriers;
	bool							unalignedBlocks;
	bool							recordUploads = false;
	idD3D12HandleTable<d3d12Texture_t, textureHandle_t>	table;
	std::array<d3d12TextureSurface_t, 2>	fallbacks;	// by textureKind_t
	std::vector<byte>				scratch;		// a converted row, or a decoded DXT level
	std::vector<byte>				rowScratch;
	uint64							bytes = 0;
	int								copySurfaces = 0;	// created by PrepareCopyTarget this frame
};

struct d3d12TextureCheckResult_t {
	int								textures = 0;
	int								unrecorded = 0;		// textures without a recorded upload: render copies, or loaded before recording
	int								subresources = 0;
	int								mismatches = 0;
	uint64							bytes = 0;			// read back
};

// Reads every recorded subresource back through a READBACK buffer and compares its hash; prints each
// mismatch and a summary line. Submits and waits for the GPU: between frames only.
class idD3D12TextureCheck {
public:
									idD3D12TextureCheck( const idD3D12Device & device, idD3D12Submitter & submitter, idD3D12Barriers & barriers,
										idD3D12Graveyard & graveyard );
	d3d12TextureCheckResult_t		Run( idD3D12TextureStore & store );

private:
	static constexpr uint64			BATCH = 64 << 20;	// read back per wait
	struct pending_t {
		const d3d12Texture_t *		texture;
		UINT						subresource;
		uint64						offset;
		UINT						rowPitch;
		d3d12UploadRecord_t			expected;
	};
	void							Compare( d3d12TextureCheckResult_t & result );	// WaitIdle, then the hashes of pending

	const idD3D12Device &			device;
	idD3D12Submitter &				submitter;
	idD3D12Barriers &				barriers;
	idD3D12Graveyard &				graveyard;
	idComPtr<ID3D12Resource>		buffer;
	uint64							size = 0;
	uint64							used = 0;
	std::vector<pending_t>			pending;
};
