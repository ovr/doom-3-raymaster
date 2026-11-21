// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "../../idlib/precompiled.h"

#include <bit>
#include <cmath>

#include "../tr_local.h"
#include "d3d12_Hdr.h"
#include "d3d12_InternalPasses.h"
#include "d3d12_Output.h"
#include "d3d12_StateMap.h"

float d3d12Lut::Range() {
	return std::log2( 1.0f / K + 1.0f ) * 31.0f / 17.0f;
}

float d3d12Lut::Coordinate( float c ) {
	return idMath::ClampFloat( 0.0f, 1.0f, std::log2( Max( c, 0.0f ) / K + 1.0f ) / Range() );
}

float d3d12Lut::Value( float t ) {
	return K * ( std::exp2( t * Range() ) - 1.0f );
}

namespace {

using vec3_t = std::array<double, 3>;
using mat3_t = std::array<vec3_t, 3>;	// rows

[[nodiscard]] mat3_t Mul( const mat3_t & a, const mat3_t & b ) {
	mat3_t r = {};
	for ( int i = 0; i < 3; i++ ) {
		for ( int j = 0; j < 3; j++ ) {
			r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
		}
	}
	return r;
}

[[nodiscard]] vec3_t Mul( const mat3_t & m, const vec3_t & v ) {
	return { m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
		m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2] };
}

[[nodiscard]] mat3_t Inverse( const mat3_t & m ) {
	const double a = m[1][1] * m[2][2] - m[1][2] * m[2][1];
	const double b = m[1][2] * m[2][0] - m[1][0] * m[2][2];
	const double c = m[1][0] * m[2][1] - m[1][1] * m[2][0];
	const double inv = 1.0 / ( m[0][0] * a + m[0][1] * b + m[0][2] * c );
	return { vec3_t{ a * inv, ( m[0][2] * m[2][1] - m[0][1] * m[2][2] ) * inv, ( m[0][1] * m[1][2] - m[0][2] * m[1][1] ) * inv },
		vec3_t{ b * inv, ( m[0][0] * m[2][2] - m[0][2] * m[2][0] ) * inv, ( m[0][2] * m[1][0] - m[0][0] * m[1][2] ) * inv },
		vec3_t{ c * inv, ( m[0][1] * m[2][0] - m[0][0] * m[2][1] ) * inv, ( m[0][0] * m[1][1] - m[0][1] * m[1][0] ) * inv } };
}

// Krystek's rational fit of the Planckian locus in CIE 1960 uv, 1000 K to 15000 K.
[[nodiscard]] std::array<double, 2> PlanckianUv( double t ) {
	const double u = ( 0.860117757 + 1.54118254e-4 * t + 1.28641212e-7 * t * t ) / ( 1.0 + 8.42420235e-4 * t + 7.08145163e-7 * t * t );
	const double v = ( 0.317398726 + 4.22806245e-5 * t + 4.20481691e-8 * t * t ) / ( 1.0 - 2.89741816e-5 * t + 1.61456053e-7 * t * t );
	return { u, v };
}

// The CIE daylight locus in xy, 4000 K to 25000 K.
[[nodiscard]] std::array<double, 2> DaylightXy( double t ) {
	const double t2 = t * t;
	const double t3 = t2 * t;
	const double x = ( t <= 7000.0 ) ? -4.6070e9 / t3 + 2.9678e6 / t2 + 0.09911e3 / t + 0.244063
									 : -2.0064e9 / t3 + 1.9018e6 / t2 + 0.24748e3 / t + 0.237040;
	return { x, -3.0 * x * x + 2.870 * x - 0.275 };
}

// XYZ of the white of temp and tint, Y = 1. The tint moves across the isotherm by tint * 0.05 in uv.
[[nodiscard]] vec3_t WhiteXyz( double temp, double tint ) {
	temp = std::clamp( temp, 1000.0, 15000.0 );
	std::array<double, 2> uv = PlanckianUv( temp );
	double u = uv[0];
	double v = uv[1];
	if ( temp >= 4000.0 ) {
		const std::array<double, 2> xy = DaylightXy( temp );
		const double d = -2.0 * xy[0] + 12.0 * xy[1] + 3.0;
		u = 4.0 * xy[0] / d;
		v = 6.0 * xy[1] / d;
	}
	const std::array<double, 2> before = PlanckianUv( temp - 1.0 );
	const std::array<double, 2> after = PlanckianUv( temp + 1.0 );
	const double du = after[0] - before[0];
	const double dv = after[1] - before[1];
	const double length = std::sqrt( du * du + dv * dv );
	u += -dv / length * tint * 0.05;
	v += du / length * tint * 0.05;
	const double d = 2.0 * u - 8.0 * v + 4.0;
	const double x = 3.0 * u / d;
	const double y = 2.0 * v / d;
	return { x / y, 1.0, ( 1.0 - x - y ) / y };
}

constexpr mat3_t SRGB_TO_XYZ = { vec3_t{ 0.4124564, 0.3575761, 0.1804375 }, vec3_t{ 0.2126729, 0.7151522, 0.0721750 },
	vec3_t{ 0.0193339, 0.1191920, 0.9503041 } };
constexpr mat3_t BRADFORD = { vec3_t{ 0.8951, 0.2664, -0.1614 }, vec3_t{ -0.7502, 1.7135, 0.0367 }, vec3_t{ 0.0389, -0.0685, 1.0296 } };

void Put( float ( &out )[4], const idVec4 & v ) {
	out[0] = v.x;
	out[1] = v.y;
	out[2] = v.z;
	out[3] = v.w;
}

[[nodiscard]] idVec4 Times( const idVec4 & a, const idVec4 & b ) {
	return idVec4( a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w );
}

[[nodiscard]] bool IsNeutral( const colorGrade_t & grade ) {
	const colorGrade_t neutral;
	return grade.saturation == neutral.saturation && grade.contrast == neutral.contrast && grade.gamma == neutral.gamma
		&& grade.gain == neutral.gain && grade.offset == neutral.offset;
}

}	// namespace

std::array<std::array<float, 3>, 3> D3D12_WhiteBalance( float temp, float tint ) {
	const vec3_t source = Mul( BRADFORD, WhiteXyz( temp, tint ) );
	const vec3_t target = Mul( BRADFORD, WhiteXyz( 6500.0, 0.0 ) );
	const mat3_t scale = { vec3_t{ target[0] / source[0], 0.0, 0.0 }, vec3_t{ 0.0, target[1] / source[1], 0.0 },
		vec3_t{ 0.0, 0.0, target[2] / source[2] } };
	const mat3_t adapt = Mul( Inverse( BRADFORD ), Mul( scale, BRADFORD ) );
	const mat3_t full = Mul( Inverse( SRGB_TO_XYZ ), Mul( adapt, SRGB_TO_XYZ ) );
	std::array<std::array<float, 3>, 3> result = {};
	for ( int i = 0; i < 3; i++ ) {
		for ( int j = 0; j < 3; j++ ) {
			result[i][j] = static_cast<float>( full[i][j] );
		}
	}
	return result;
}

d3d12LutParms_t D3D12_LutParms( const postProcessSettings_t & settings ) {
	d3d12LutParms_t parms;
	const bool whiteBalance = settings.whiteTemp != 6500.0f || settings.whiteTint != 0.0f;
	const std::array<std::array<float, 3>, 3> matrix = whiteBalance ? D3D12_WhiteBalance( settings.whiteTemp, settings.whiteTint )
		: std::array<std::array<float, 3>, 3>{ { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
	for ( int i = 0; i < 3; i++ ) {
		for ( int j = 0; j < 3; j++ ) {
			parms.whiteBalance[i][j] = matrix[i][j];
		}
	}
	// the global values multiply each range's, the offsets add
	const colorGrade_t * const ranges[] = { &settings.shadows, &settings.midtones, &settings.highlights };
	for ( int r = 0; r < 3; r++ ) {
		const colorGrade_t & range = *ranges[r];
		Put( parms.grade[r * 5 + 0], Times( settings.global.saturation, range.saturation ) );
		Put( parms.grade[r * 5 + 1], Times( settings.global.contrast, range.contrast ) );
		Put( parms.grade[r * 5 + 2], Times( settings.global.gamma, range.gamma ) );
		Put( parms.grade[r * 5 + 3], Times( settings.global.gain, range.gain ) );
		Put( parms.grade[r * 5 + 4], settings.global.offset + range.offset );
	}
	const bool grade = !IsNeutral( settings.global ) || !IsNeutral( settings.shadows ) || !IsNeutral( settings.midtones )
		|| !IsNeutral( settings.highlights );
	const hdrTonemapper_t tonemapper = ( settings.tonemapper < hdrTonemapper_t::COUNT ) ? settings.tonemapper : hdrTonemapper_t::AGX;
	parms.options[0] = settings.shadowsMax;
	parms.options[1] = settings.highlightsMin;
	parms.options[2] = static_cast<float>( tonemapper );
	parms.options[3] = ( settings.agxLook != 0 ) ? 1.0f : 0.0f;
	parms.encoding[0] = d3d12Lut::K;
	parms.encoding[1] = d3d12Lut::Range();
	parms.encoding[2] = whiteBalance ? 1.0f : 0.0f;
	parms.encoding[3] = grade ? 1.0f : 0.0f;
	return parms;
}

namespace {

// b1 of internal/tonemap.hlsl.
struct tonemapParms_t {
	uint32							sceneSrv;
	uint32							lutSrv;
	uint32							lutSampler;
	uint32							dither;
	float							exposureScale;
	float							lutK;
	float							lutInvRange;
	float							unused;
};

}	// namespace

idD3D12HdrPipeline::idD3D12HdrPipeline( const idD3D12Device & owner, const idD3D12ComputeRootSignature & root,
	const idD3D12InternalPasses & internalPasses, idD3D12SamplerHeap & samplerHeap, idD3D12DescriptorPool & srvs ) :
	device( owner ),
	computeRoot( root ),
	passes( internalPasses ),
	samplers( samplerHeap ),
	srvPool( srvs ) {
}

void idD3D12HdrPipeline::CreateDefaults() {
	lutSampler = samplers.Acquire( D3D12_PackSampler( { samplerFilter_t::LINEAR, samplerAddress_t::CLAMP, 1.0f, 0.0f } ) );
	const d3d12HlslStage_t & lutStage = passes.Stage( "internal_combineLut" );
	combineLut = D3D12_CreateComputePipeline( device, computeRoot, lutStage.code, lutStage.reflection, L"doom3 combine LUT" );
	const d3d12HlslStage_t & resolveStage = passes.Stage( "internal_hdrResolve" );
	msaaResolve = D3D12_CreateComputePipeline( device, computeRoot, resolveStage.code, resolveStage.reflection, L"doom3 HDR MSAA resolve" );

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	desc.Width = d3d12Lut::SIZE;
	desc.Height = d3d12Lut::SIZE;
	desc.DepthOrArraySize = d3d12Lut::SIZE;
	desc.MipLevels = 1;
	desc.Format = d3d12TargetId_t::HDR_FORMAT;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	if ( !device.Check( device.Get().CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
			IID_PPV_ARGS( &lut ) ), "CreateCommittedResource (combine LUT)" ) ) {
		return;
	}
	lut->SetName( L"doom3 combine LUT" );
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = desc.Format;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture3D.MipLevels = 1;
	lutSrv = srvPool.Allocate();
	srvPool.CreateSrv( lutSrv, *lut.Get(), srv );
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
	uav.Format = desc.Format;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
	uav.Texture3D.WSize = d3d12Lut::SIZE;
	lutUav = srvPool.Allocate();
	srvPool.CreateUav( lutUav, *lut.Get(), uav );
}

ID3D12PipelineState * idD3D12HdrPipeline::Tonemap( UINT samples ) {
	idComPtr<ID3D12PipelineState> & pso = tonemap[std::countr_zero( samples ) & 3];
	if ( !pso ) {
		pso = passes.CreatePipeline( passes.Stage( "internal_tonemap" ).code, d3d12TargetId_t::Of( false, samples ).ColorView(), samples,
			static_cast<uint8>( D3D12_COLOR_WRITE_ENABLE_ALL ), L"doom3 tonemap" );
	}
	return pso.Get();
}

ID3D12PipelineState * idD3D12HdrPipeline::EncodePipeline( UINT samples ) {
	idComPtr<ID3D12PipelineState> & pso = encode[std::countr_zero( samples ) & 3];
	if ( !pso ) {
		pso = passes.CreatePipeline( passes.Stage( ( samples > 1 ) ? "internal_hdrEncode_msaa" : "internal_hdrEncode" ).code,
			d3d12TargetId_t::HDR_FORMAT, samples, static_cast<uint8>( D3D12_COLOR_WRITE_ENABLE_ALL ), L"doom3 HDR encode" );
	}
	return pso.Get();
}

// viewport inside the scene, empty when nothing of it is.
[[nodiscard]] static renderRect_t SceneRect( const idD3D12SceneTarget & scene, const renderRect_t & viewport ) {
	const int x0 = Max( viewport.x, 0 );
	const int y0 = Max( viewport.y, 0 );
	const int x1 = Min( viewport.x + viewport.width, scene.Width() );
	const int y1 = Min( viewport.y + viewport.height, scene.Height() );
	return { x0, y0, Max( x1 - x0, 0 ), Max( y1 - y0, 0 ) };
}

// b1 of internal/hdrEncode.hlsl.
struct encodeParms_t {
	uint32							sourceSrv;
	uint32							toGamma;
};

void idD3D12HdrPipeline::Encode( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, bool toGamma, const renderRect_t & viewport,
	idD3D12DebugGroups * groups ) {
	idD3D12SceneTarget::surface_t & hdrColor = scene.Color( true );
	const UINT samples = scene.Samples();
	ID3D12PipelineState * pso = EncodePipeline( samples );
	const renderRect_t rect = SceneRect( scene, viewport );
	if ( !hdrColor.resource || pso == nullptr || rect.width == 0 || rect.height == 0 ) {
		return;
	}
	const uint32 width = static_cast<uint32>( scene.Width() );
	const uint32 height = static_cast<uint32>( scene.Height() );
	const d3d12GraphResource_t sceneColor = graph.Import( "SceneColor", { hdrColor.resource.Get(), &hdrColor.state, hdrColor.srv, {},
		scene.Rtv( false, true ), width, height } );
	d3d12PoolDesc_t desc;
	desc.format = d3d12TargetId_t::HDR_FORMAT;
	desc.width = width;
	desc.height = height;
	desc.samples = static_cast<uint8>( samples );
	desc.renderTarget = ( samples > 1 );	// a multisampled resource must be a target
	const d3d12GraphResource_t copy = graph.Create( "SceneColor copy", desc );
	graph.AddPass( "Copy SceneColor", d3d12GraphPass_t::COPY, { { sceneColor, d3d12GraphAccess_t::COPY_SOURCE }, { copy, d3d12GraphAccess_t::COPY_DEST } },
		[sceneColor, copy]( idD3D12GraphContext & context ) {
			context.List().CopyResource( &context.Resource( copy ), &context.Resource( sceneColor ) );
		} );
	graph.AddPass( toGamma ? "Encode SceneColor" : "Decode SceneColor", d3d12GraphPass_t::RASTER,
		{ { copy, d3d12GraphAccess_t::SRV }, { sceneColor, d3d12GraphAccess_t::RTV, 0 } },
		[pso, copy, sceneColor, rect, toGamma]( idD3D12GraphContext & context ) {
			const encodeParms_t parms = { context.Srv( copy ), toGamma ? 1u : 0u };
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Draw( *pso, sceneColor, 0, rect, span );
		} );
	graph.Execute( groups );
}

void idD3D12HdrPipeline::Resolve( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, const postProcessSettings_t & settings,
	const renderRect_t & viewport, float preExposure, idD3D12DebugGroups * groups ) {
	idD3D12SceneTarget::surface_t & hdrColor = scene.Color( true );
	idD3D12SceneTarget::surface_t & output = scene.Color( false );
	ID3D12PipelineState * tonemapPso = Tonemap( scene.Samples() );
	const renderRect_t rect = SceneRect( scene, viewport );
	if ( !hdrColor.resource || !output.resource || !lut || !combineLut || !msaaResolve || tonemapPso == nullptr || rect.width == 0 || rect.height == 0 ) {
		return;
	}
	const uint32 width = static_cast<uint32>( scene.Width() );
	const uint32 height = static_cast<uint32>( scene.Height() );
	const float exposureScale = R_ManualExposure( settings ) / Max( preExposure, 1e-20f );

	const d3d12GraphResource_t sceneColor = graph.Import( "SceneColor", { hdrColor.resource.Get(), &hdrColor.state, hdrColor.srv, {}, {}, width, height } );
	const d3d12GraphResource_t target = graph.Import( "Output", { output.resource.Get(), &output.state, output.srv, {}, scene.Rtv( false ), width, height } );
	const d3d12GraphResource_t lutTexture = graph.Import( "CombineLUT", { lut.Get(), &lutState, lutSrv, lutUav, {}, d3d12Lut::SIZE, d3d12Lut::SIZE } );

	const d3d12LutParms_t parms = D3D12_LutParms( settings );
	if ( !lutParms || *lutParms != parms ) {
		lutParms = parms;
		lutBuilds++;
		graph.AddPass( "Combine LUT", d3d12GraphPass_t::COMPUTE, { { lutTexture, d3d12GraphAccess_t::UAV, 0 } }, [this, parms, lutTexture]( idD3D12GraphContext & context ) {
			const d3d12UploadSpan_t span = context.Parms( parms );
			const uint32 uav = context.Uav( lutTexture );
			context.Dispatch( combineLut, &span, d3d12Lut::SIZE, d3d12Lut::SIZE, d3d12Lut::SIZE, std::span( &uav, 1 ) );
		} );
	}

	d3d12GraphResource_t input = sceneColor;
	const UINT samples = scene.Samples();
	if ( samples > 1 ) {
		d3d12PoolDesc_t desc;
		desc.format = d3d12TargetId_t::HDR_FORMAT;
		desc.width = width;
		desc.height = height;
		desc.unorderedAccess = true;
		input = graph.Create( "SceneColor resolved", desc );
		graph.AddPass( "MSAA resolve", d3d12GraphPass_t::COMPUTE, { { sceneColor, d3d12GraphAccess_t::SRV }, { input, d3d12GraphAccess_t::UAV, 0 } },
			[this, sceneColor, input, samples, exposureScale, rect]( idD3D12GraphContext & context ) {
				const std::array<uint32, 8> constants = { context.Srv( sceneColor ), context.Uav( input ), samples, std::bit_cast<uint32>( exposureScale ),
					static_cast<uint32>( rect.x ), static_cast<uint32>( rect.y ), static_cast<uint32>( rect.width ), static_cast<uint32>( rect.height ) };
				context.Dispatch( msaaResolve, nullptr, static_cast<uint32>( rect.width ), static_cast<uint32>( rect.height ), 1, constants );
			} );
	}

	const tonemapParms_t tonemapBase = { 0, 0, lutSampler, settings.dither ? 1u : 0u, exposureScale, d3d12Lut::K, 1.0f / d3d12Lut::Range(), 0.0f };
	graph.AddPass( "Tonemap", d3d12GraphPass_t::RASTER,
		{ { input, d3d12GraphAccess_t::SRV }, { lutTexture, d3d12GraphAccess_t::SRV }, { target, d3d12GraphAccess_t::RTV, 0 } },
		[tonemapPso, tonemapBase, input, lutTexture, target, rect]( idD3D12GraphContext & context ) {
			tonemapParms_t tonemapParms = tonemapBase;
			tonemapParms.sceneSrv = context.Srv( input );
			tonemapParms.lutSrv = context.Srv( lutTexture );
			const d3d12UploadSpan_t span = context.Parms( tonemapParms );
			context.Draw( *tonemapPso, target, 0, rect, span );
		} );
	graph.Execute( groups );
}
