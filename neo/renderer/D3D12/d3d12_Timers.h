// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "d3d12_Device.h"

#include <bitset>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/*
	GPU timestamps: one TIMESTAMP query heap, resolved into a READBACK buffer with the same slots.

	  slot 0, 1                              the frame timer: BeginFrame, EndFrame → GetFrameGpuMicroseconds
	  FIRST_SET + set * SLOTS_PER_SET + i    r_pix 1: WriteTimestamp( set, i ), TIMESTAMP_SETS sets

	EndFrame ends the frame timer and resolves it and every slot the frame wrote, in the open list, tagged
	with that list's fence as the graveyard tags its graves. A slot is ready once queue.Waited() has reached
	the tag: Present waits for its frame, so a frame's times are there after its Present. Polling reads no
	fence, waits for nothing and frees nothing, so r_pix cannot change what two runs record.
*/
class idD3D12GpuTimers {
public:
	static constexpr int			SLOTS_PER_SET = 2048;	// RenderLog: 1024 blocks of two timestamps

									idD3D12GpuTimers( const idD3D12Device & device, const idD3D12Queue & queue );
	[[nodiscard]] bool				Available() const { return heap != nullptr; }	// caps.gpuTimer, caps.timestamps
	[[nodiscard]] uint64			Frequency() const { return frequency; }
	void							BeginFrame( ID3D12GraphicsCommandList4 & list );
	void							Write( ID3D12GraphicsCommandList4 & list, int set, int index );
	void							EndFrame( ID3D12GraphicsCommandList4 & list );	// the end stamp and the resolves; submit next
	[[nodiscard]] bool				Ready( int set, int index ) const;
	void							Read( int set, std::span<uint64> nanoseconds ) const;	// zeros unless the set is ready
	// The frame EndFrame resolved last, once its list was waited for (Present does): never blocks; 0 before.
	[[nodiscard]] uint64			FrameMicroseconds();
	void							Free();					// every r_pix slot unwritten; the frame timer stays
	[[nodiscard]] int				WrittenThisFrame() const { return writes; }
	void							ResetFrameCounters() { writes = 0; }

private:
	static constexpr UINT			FRAME_BEGIN = 0;
	static constexpr UINT			FRAME_END = 1;
	static constexpr UINT			FIRST_SET = 2;
	static constexpr UINT			SLOTS = FIRST_SET + TIMESTAMP_SETS * SLOTS_PER_SET;

	struct set_t {
		std::bitset<SLOTS_PER_SET>	written;		// this frame, resolved by EndFrame
		std::bitset<SLOTS_PER_SET>	resolved;		// by the list of fence
		uint64						fence = 0;		// 0: nothing resolved
	};

	[[nodiscard]] static constexpr UINT	Slot( int set, int index ) { return FIRST_SET + static_cast<UINT>( set * SLOTS_PER_SET + index ); }
	void							Resolve( ID3D12GraphicsCommandList4 & list, UINT first, UINT count ) const;
	[[nodiscard]] bool				CopyTicks( UINT first, std::span<uint64> ticks ) const;

	const idD3D12Device &			device;
	const idD3D12Queue &			queue;
	idComPtr<ID3D12QueryHeap>		heap;
	idComPtr<ID3D12Resource>		readback;
	uint64							frequency = 0;			// ticks per second of the direct queue
	std::array<set_t, TIMESTAMP_SETS>	sets;
	bool							frameBegun = false;
	uint64							frameFence = 0;			// the list that resolved the last frame timer
	uint64							frameReadFence = 0;		// frameMicroseconds is the time of this list's frame
	uint64							frameMicroseconds = 0;
	int								writes = 0;
};

/*
	r_pix 4: the GPU time of every render graph pass, by its name (NRD's carry their denoiser's), in a TIMESTAMP
	heap of its own: FRAMES frames of MAX_PASSES pairs. EndFrame resolves the frame's pairs, tagged with the fence
	of the list it submits; BeginFrame adds up the frames the queue has waited for, never waits, and prints the
	passes about once a second, the most expensive first. A frame whose slots are still in flight is not timed.
*/
class idD3D12PassTimer {
public:
	static constexpr int			FRAMES = 4;
	static constexpr int			MAX_PASSES = 1024;

	struct passTime_t {
		const char *				name;
		double						gpuMilliseconds;	// per frame, over the period
		double						maxMilliseconds;	// of one frame
		double						count;				// instances per frame
	};

									idD3D12PassTimer( const idD3D12Device & device, const idD3D12Queue & queue );
	void							Enable( bool on );	// off drops the frames in flight and the period
	[[nodiscard]] bool				Enabled() const { return enabled; }
	void							BeginFrame( int nowMilliseconds );
	void							Begin( ID3D12GraphicsCommandList4 & list, const char * name );
	void							End( ID3D12GraphicsCommandList4 & list );
	void							EndFrame( ID3D12GraphicsCommandList4 & list );
	[[nodiscard]] int				PeriodFrames() const { return periodFrames; }
	[[nodiscard]] std::vector<passTime_t>	PeriodTimes() const;	// the most expensive first; names stay valid

private:
	struct frame_t {
		std::vector<uint16>			names;		// per timed pass, into names
		uint64						fence = 0;	// 0: nothing in flight
	};
	struct stats_t {
		double						sum = 0.0;
		double						max = 0.0;
		int							count = 0;
	};

	void							Collect();
	void							Print( int periodMilliseconds ) const;
	void							ResetPeriod();
	[[nodiscard]] uint16			NameId( const char * name );

	const idD3D12Device &			device;
	const idD3D12Queue &			queue;
	idComPtr<ID3D12QueryHeap>		heap;
	idComPtr<ID3D12Resource>		readback;
	uint64							frequency = 0;
	bool							enabled = false;
	bool							timing = false;		// this frame's passes are timed
	bool							opened = false;		// the outermost pass has its first stamp
	int								depth = 0;			// passes recorded inside a pass are part of it
	int								current = 0;
	std::array<frame_t, FRAMES>		frames;
	struct nameHash_t {
		using is_transparent = void;
		[[nodiscard]] size_t		operator()( std::string_view name ) const { return std::hash<std::string_view>{}( name ); }
	};
	std::deque<std::string>			names;				// a deque: PeriodTimes hands out their pointers
	std::unordered_map<std::string, uint16, nameHash_t, std::equal_to<>>	nameIds;
	std::vector<stats_t>			period;				// by name
	std::vector<double>				frameMs;			// scratch of Collect, by name
	std::vector<int>				frameCount;
	std::vector<uint64>				ticks;
	int								periodFrames = 0;
	int								periodSkipped = 0;	// frames not timed: their slots were in flight
	int								periodUntimed = 0;	// passes over MAX_PASSES
	int								periodStart = -1;
};

/*
	The debug groups of r_pix 2 for PIX and RenderDoc: BeginEvent / EndEvent with a UTF-16 name
	(metadata 0, PIX's unicode event). A group of shared code can outlive a list, so the stack lives
	here, not in the list: SubmitChunk ends every open group in the list it closes, and the next list
	begins them again in the same order. While a list is open, every group of the stack is open in it.

	  Push   → BeginEvent in the open list (opened on demand)
	  Pop    → EndEvent if a list is open
	  CloseAll (SubmitChunk), Reopen (a new list)
*/
class idD3D12DebugGroups {
public:
	static constexpr int			MAX_DEPTH = 64;		// caps.maxDebugGroupDepth
	static constexpr int			MAX_NAME = 64;		// characters with the terminator; longer names are cut

	void							Push( ID3D12GraphicsCommandList4 & list, const char * name );
	void							Pop( ID3D12GraphicsCommandList4 * list );	// nullptr: no list is open
	void							CloseAll( ID3D12GraphicsCommandList4 & list );
	void							Reopen( ID3D12GraphicsCommandList4 & list );
	[[nodiscard]] int				Depth() const { return depth; }
	[[nodiscard]] int				PushedThisFrame() const { return pushed; }
	void							ResetFrameCounters() { pushed = 0; }

private:
	void							Begin( ID3D12GraphicsCommandList4 & list, int level ) const;

	std::array<std::array<wchar_t, MAX_NAME>, MAX_DEPTH>	names = {};
	int								depth = 0;
	int								ignored = 0;		// pushes past MAX_DEPTH, so the pops stay balanced
	int								pushed = 0;
};
