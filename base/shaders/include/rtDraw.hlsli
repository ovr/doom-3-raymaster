// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "raytracing.hlsli"

// b4, the root constants of the pixel stage of a render program that traces (d3d12TraceResources_t, in its
// order): the heap indices of what a ray of the GPU scene reads. Only a row of the program table marked as
// tracing may bind it. All zero while nothing may be traced: ask RtDrawValid before any ray.
cbuffer TraceResources : register( b4 ) {
	uint rtTlas;
	uint rtRecords;
	uint rtInstances;
	uint rtVertices;
	uint rtIndexes;
	uint rtSkinned;
	uint rtNoise;		// include/rtNoise.hlsli
	uint rtSampler;		// linear, repeating: the alpha of perforated casters
	uint rtFlags;
};

static const uint RT_DRAW_VALID = 1u << 0;

bool RtDrawValid() {
	return ( rtFlags & RT_DRAW_VALID ) != 0;
}

RtHeap RtDrawHeap() {
	RtHeap heap;
	heap.tlas = rtTlas;
	heap.records = rtRecords;
	heap.instances = rtInstances;
	heap.vertices = rtVertices;
	heap.indexes = rtIndexes;
	heap.skinned = rtSkinned;
	return heap;
}
