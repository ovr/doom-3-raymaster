// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "d3d12_Device.h"

#include <functional>

// The text of a file of the game data by its path under base/ ("shaders/include/common.hlsli"), or nullopt:
// fileSystem in the engine, the repo's base/ in the tests.
using d3d12ShaderReader_t = std::function<std::optional<idStr>( const char * path )>;

// One DXC compile: the DXIL container, signed by dxil.dll, and DXC's warnings.
struct d3d12DxcOutput_t {
	std::vector<byte>				code;
	idStr							warnings;
};

// HLSL 2021 at -O3. profile: "vs_6_6", "ps_6_6", ...; defines: "NAME" or "NAME=VALUE". Fails with DXC's errors.
// name is the source's path: #include "x" is looked up beside it through include; without include there is none.
// Without include it is safe on several threads at once.
[[nodiscard]] std::expected<d3d12DxcOutput_t, idStr>	D3D12_DxcCompile( const idD3D12Library & library, std::string_view source,
															const char * name, const char * entry, const char * profile,
															std::span<const char * const> defines = {},
															const d3d12ShaderReader_t * include = nullptr );
// The source with its #include files expanded and the defines applied, as DXC's -P writes it (with #line).
[[nodiscard]] std::expected<idStr, idStr>				D3D12_DxcPreprocess( const idD3D12Library & library, std::string_view source,
															const char * name, std::span<const char * const> defines,
															const d3d12ShaderReader_t & include );
// The reflection of a DXIL container (IDxcUtils::CreateReflection).
[[nodiscard]] std::expected<idComPtr<ID3D12ShaderReflection>, idStr>	D3D12_DxcReflect( const idD3D12Library & library,
															std::span<const byte> container );

// dxil.dll writes a 16-byte digest after the container's fourcc; the runtime refuses DXIL whose digest is zero.
[[nodiscard]] bool										D3D12_DxilSigned( std::span<const byte> container );
