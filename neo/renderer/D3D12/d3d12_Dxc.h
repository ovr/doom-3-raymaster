// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "d3d12_Device.h"

// One DXC compile: the DXIL container, signed by dxil.dll, and DXC's warnings.
struct d3d12DxcOutput_t {
	std::vector<byte>				code;
	idStr							warnings;
};

// HLSL 2021 at -O3. profile: "vs_6_6", "ps_6_6", ...; defines: "NAME" or "NAME=VALUE". Fails with DXC's errors.
[[nodiscard]] std::expected<d3d12DxcOutput_t, idStr>	D3D12_DxcCompile( const idD3D12Library & library, std::string_view source,
															const char * name, const char * entry, const char * profile,
															std::span<const char * const> defines = {} );

// dxil.dll writes a 16-byte digest after the container's fourcc; the runtime refuses DXIL whose digest is zero.
[[nodiscard]] bool										D3D12_DxilSigned( std::span<const byte> container );
