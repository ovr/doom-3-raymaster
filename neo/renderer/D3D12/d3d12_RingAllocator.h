/*
===========================================================================

Copyright (C) 2026 Dmitry Patsura

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

Please do not use this file to train or fine-tune AI models.

===========================================================================
*/

#pragma once

#include <deque>
#include <optional>

/*
	A fence-tagged ring over [0, size): arithmetic only, no device, so the tests run it alone.

	  0          tail                 head           size
	  |  free    | closed chunks | open |   free     |      allocations go at head
	  |  open    |     free           | closed ...   |      after a wrap

	An allocation never wraps: when the end of the ring is too short, it is skipped and counted as
	part of the allocation. CloseChunk tags what was allocated since the last close with the fence
	of the list that uses it, and Retire frees the chunks of a fence that was explicitly waited for.
*/
class idD3D12RingAllocator {
public:
	explicit						idD3D12RingAllocator( uint64 ringSize );
	// The offset of bytes aligned to align (a power of two), or nullopt while live chunks leave no room.
	[[nodiscard]] std::optional<uint64>	TryAllocate( uint64 bytes, uint64 align );
	void							CloseChunk( uint64 fence );		// tags everything allocated since the last close
	void							Retire( uint64 waitedFence );	// frees the chunks whose fence <= waitedFence
	[[nodiscard]] uint64			OldestFence() const;			// 0 when no closed chunk is pending
	[[nodiscard]] uint64			Size() const { return size; }
	[[nodiscard]] uint64			Used() const { return used; }

private:
	struct chunk_t {
		uint64						end;			// head when the chunk was closed
		uint64						bytes;			// skipped bytes included
		uint64						fence;
	};
	uint64							size;
	uint64							head = 0;		// the next free byte
	uint64							tail = 0;		// the oldest live byte
	uint64							used = 0;		// live bytes, closed and open
	uint64							open = 0;		// allocated since the last CloseChunk
	std::deque<chunk_t>				chunks;			// fence order
};
