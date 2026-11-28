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

float d3d12Lut::CurveScale( hdrTonemapper_t tonemapper, uint8 look ) {
	switch ( tonemapper ) {
		case hdrTonemapper_t::AGX:		return std::exp2( ( look != 0 ) ? 1.27f : -0.31f );
		case hdrTonemapper_t::FILMIC:	return std::exp2( 0.75f );
		default:						return 1.0f;
	}
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
	parms.curve[0] = d3d12Lut::CurveScale( tonemapper, settings.agxLook );
	return parms;
}

namespace {

// b1 of internal/tonemap.hlsl.
struct tonemapParms_t {
	uint32							sceneSrv;
	uint32							lutSrv;
	uint32							lutSampler;
	uint32							dither;
	float							invPreExposure;
	float							lutK;
	float							lutInvRange;
	uint32							eyeSrv;
	uint32							visualize;
	float							histogramMinEV;
	float							binWidth;
	float							lowPercent;
	float							highPercent;
	int32							viewX;
	int32							viewY;
	int32							viewWidth;
	int32							viewHeight;
	uint32							bloomSrv;
	float							bloomIntensity;
	float							bloomScaleX;
	float							bloomScaleY;
	uint32							localExposure;
	uint32							gridSrv;
	uint32							blurredSrv;
	float							highlightContrast;
	float							shadowContrast;
	float							detailStrength;
	float							blurredBlend;
	float							gridMinLog;
	float							gridInvLogRange;
	float							gridScaleX;
	float							gridScaleY;
};

// b0 of internal/exposureHistogram.hlsl.
struct histogramParms_t {
	uint32							sceneSrv;
	uint32							histogramUav;
	uint32							x0;
	uint32							y0;
	uint32							width;
	uint32							height;
	float							invPreExposure;
	float							meteringMask;
	float							minEV;
	float							binsPerEV;
};

// b0 of internal/exposureAdapt.hlsl.
struct adaptParms_t {
	uint32							histogramUav;
	uint32							eyeUav;
	float							histogramMinEV;
	float							binWidth;
	float							lowPercent;
	float							highPercent;
	float							minEV100;
	float							maxEV100;
	float							speedUp;
	float							speedDown;
	float							seconds;
	uint32							reset;
	uint32							autoExposure;
	float							compensation;
	float							bias;
	float							manualExposure;
};

// b0 of internal/bloomDownsample.hlsl.
struct bloomDownsampleParms_t {
	uint32							sourceSrv;
	uint32							targetUav;
	uint32							linearSampler;
	uint32							karis;
	float							sourceOrigin[2];
	float							invSourceSize[2];
	float							sourceMin[2];
	float							sourceMax[2];
	uint32							targetSize[2];
};

// b0 of internal/bloomBlur.hlsl.
struct bloomBlurParms_t {
	static constexpr uint32			NO_SMALLER = 0xFFFFFFFF;
	uint32							sourceSrv;
	uint32							targetUav;
	uint32							smallerSrv;
	uint32							linearSampler;
	uint32							size[2];
	uint32							vertical;
	uint32							radius;
	float							tint[3];
	float							smallerScaleX;
	float							smallerScaleY;
	float							unused[3];
	float							weights[( d3d12Bloom::MAX_RADIUS + 4 ) / 4][4];
};

// b0 of internal/localExposureGrid.hlsl.
struct localGridParms_t {
	uint32							sceneSrv;
	uint32							gridUav;
	uint32							totalsUav;
	uint32							x0;
	uint32							y0;
	uint32							width;
	uint32							height;
	float							invPreExposure;
	float							minLog;
	float							binsPerLog;
	float							logRange;
};

// b0 of internal/localExposureBlur.hlsl.
struct localBlurParms_t {
	uint32							gridSrv;
	uint32							totalsSrv;
	uint32							gridUav;
	uint32							blurredUav;
	uint32							tilesX;
	uint32							tilesY;
	float							invTwoSigmaSquared;
	int32							radius;
};

// The grid's log2 luminance range: the histogram's EV100 less 3.
[[nodiscard]] float GridMinLog( const postProcessSettings_t & settings ) {
	return settings.histogramMinEV - 3.0f;
}

[[nodiscard]] float GridLogRange( const postProcessSettings_t & settings ) {
	return Max( settings.histogramMaxEV - settings.histogramMinEV, 1.0f );
}

// A raw view of a buffer of bytes.
template<typename VIEW> [[nodiscard]] VIEW RawView( uint32 bytes ) {
	VIEW view = {};
	view.Format = DXGI_FORMAT_R32_TYPELESS;
	view.Buffer.NumElements = bytes / 4;
	if constexpr ( std::is_same_v<VIEW, D3D12_SHADER_RESOURCE_VIEW_DESC> ) {
		view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	} else {
		view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
	}
	return view;
}

}	// namespace

uint32 d3d12Bloom::Levels( const postProcessSettings_t & settings, int width, int height ) {
	const uint32 wanted = ( settings.bloomQuality == 0 ) ? 0 : Min( settings.bloomQuality + 1u, MAX_LEVELS );
	const bool shown = settings.bloomIntensity > 0.0f || settings.visualize == 3;
	float luma = 0.0f;
	for ( uint32 level = 0; level < wanted; level++ ) {
		luma += settings.bloomTints[level] * idVec3( 0.2126f, 0.7152f, 0.0722f );
	}
	if ( !shown || !( luma > 0.0f ) || width <= 0 || height <= 0 ) {
		return 0;
	}
	const uint32 largest = static_cast<uint32>( Max( width + 1, height + 1 ) / 2 );
	return Min( wanted, static_cast<uint32>( std::bit_width( largest ) ) );		// mips down to 1 x 1
}

std::vector<float> d3d12Bloom::Weights( const postProcessSettings_t & settings, uint32 level, int viewportWidth ) {
	const float reach = settings.bloomSizes[level] * settings.bloomSizeScale * 0.01f * static_cast<float>( viewportWidth );
	const float sigma = reach / ( 3.0f * static_cast<float>( 2u << level ) );
	if ( !( sigma > 0.01f ) ) {
		return { 1.0f };
	}
	const uint32 radius = Min( static_cast<uint32>( std::ceil( 3.0f * sigma ) ), MAX_RADIUS );
	std::vector<float> weights( radius + 1 );
	double sum = 0.0;
	for ( uint32 t = 0; t <= radius; t++ ) {
		weights[t] = std::exp( -static_cast<float>( t * t ) / ( 2.0f * sigma * sigma ) );
		sum += ( t == 0 ) ? weights[t] : 2.0 * weights[t];
	}
	for ( float & weight : weights ) {
		weight = static_cast<float>( weight / sum );
	}
	return weights;
}

std::array<idVec3, d3d12Bloom::MAX_LEVELS> d3d12Bloom::Tints( const postProcessSettings_t & settings, uint32 levels ) {
	std::array<idVec3, MAX_LEVELS> tints;
	float luma = 0.0f;
	for ( uint32 level = 0; level < MAX_LEVELS; level++ ) {
		tints[level] = ( level < levels ) ? settings.bloomTints[level] : vec3_zero;
		luma += tints[level] * idVec3( 0.2126f, 0.7152f, 0.0722f );
	}
	for ( idVec3 & tint : tints ) {
		tint = ( luma > 0.0f ) ? tint / luma : vec3_zero;
	}
	return tints;
}

bool d3d12LocalExposure::Active( const postProcessSettings_t & settings ) {
	const auto changes = []( float value ) { return idMath::Fabs( value - 1.0f ) > 1e-3f; };
	return settings.localExposure && ( settings.visualize == 4 || changes( settings.localHighlightContrast ) || changes( settings.localShadowContrast )
		|| changes( settings.localDetailStrength ) );
}

idD3D12HdrPipeline::idD3D12HdrPipeline( const idD3D12Device & owner, idD3D12Submitter & submit, const idD3D12ComputeRootSignature & root,
	const idD3D12InternalPasses & internalPasses, idD3D12SamplerHeap & samplerHeap, idD3D12DescriptorPool & srvs ) :
	device( owner ),
	submitter( submit ),
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
	const d3d12HlslStage_t & histogramStage = passes.Stage( "internal_exposureHistogram" );
	exposureHistogram = D3D12_CreateComputePipeline( device, computeRoot, histogramStage.code, histogramStage.reflection, L"doom3 exposure histogram" );
	const d3d12HlslStage_t & adaptStage = passes.Stage( "internal_exposureAdapt" );
	exposureAdapt = D3D12_CreateComputePipeline( device, computeRoot, adaptStage.code, adaptStage.reflection, L"doom3 eye adaptation" );
	const d3d12HlslStage_t & downsampleStage = passes.Stage( "internal_bloomDownsample" );
	bloomDownsample = D3D12_CreateComputePipeline( device, computeRoot, downsampleStage.code, downsampleStage.reflection, L"doom3 bloom downsample" );
	const d3d12HlslStage_t & blurStage = passes.Stage( "internal_bloomBlur" );
	bloomBlur = D3D12_CreateComputePipeline( device, computeRoot, blurStage.code, blurStage.reflection, L"doom3 bloom blur" );
	const d3d12HlslStage_t & gridStage = passes.Stage( "internal_localExposureGrid" );
	localExposureGrid = D3D12_CreateComputePipeline( device, computeRoot, gridStage.code, gridStage.reflection, L"doom3 local exposure grid" );
	const d3d12HlslStage_t & localBlurStage = passes.Stage( "internal_localExposureBlur" );
	localExposureBlur = D3D12_CreateComputePipeline( device, computeRoot, localBlurStage.code, localBlurStage.reflection, L"doom3 local exposure blur" );

	// committed buffers start zeroed: the histogram empty; EyeAdaptation is set by the first resolve's reset
	histogram = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_DEFAULT, d3d12Exposure::BINS * 4, D3D12_RESOURCE_STATE_COMMON, L"doom3 exposure histogram",
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
	eyeAdaptation = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_DEFAULT, d3d12Exposure::EYE_BYTES, D3D12_RESOURCE_STATE_COMMON, L"doom3 eye adaptation",
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
	eyeReadback = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_READBACK, EYE_READBACKS * d3d12Exposure::EYE_BYTES, D3D12_RESOURCE_STATE_COPY_DEST,
		L"doom3 eye adaptation readback" );
	if ( histogram && eyeAdaptation && eyeReadback ) {
		histogramState.decays = true;
		eyeState.decays = true;
		histogramUav = srvPool.Allocate();
		srvPool.CreateUav( histogramUav, *histogram.Get(), RawView<D3D12_UNORDERED_ACCESS_VIEW_DESC>( d3d12Exposure::BINS * 4 ) );
		eyeSrv = srvPool.Allocate();
		srvPool.CreateSrv( eyeSrv, *eyeAdaptation.Get(), RawView<D3D12_SHADER_RESOURCE_VIEW_DESC>( d3d12Exposure::EYE_BYTES ) );
		eyeUav = srvPool.Allocate();
		srvPool.CreateUav( eyeUav, *eyeAdaptation.Get(), RawView<D3D12_UNORDERED_ACCESS_VIEW_DESC>( d3d12Exposure::EYE_BYTES ) );
	}

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
	const renderRect_t & viewport, float preExposure, const hdrExposureStep_t & step, idD3D12DebugGroups * groups ) {
	idD3D12SceneTarget::surface_t & hdrColor = scene.Color( true );
	idD3D12SceneTarget::surface_t & output = scene.Color( false );
	ID3D12PipelineState * tonemapPso = Tonemap( scene.Samples() );
	const renderRect_t rect = SceneRect( scene, viewport );
	if ( !hdrColor.resource || !output.resource || !lut || !combineLut || !msaaResolve || !exposureHistogram || !exposureAdapt || !eyeReadback
		|| tonemapPso == nullptr || rect.width == 0 || rect.height == 0 ) {
		return;
	}
	const uint32 width = static_cast<uint32>( scene.Width() );
	const uint32 height = static_cast<uint32>( scene.Height() );
	const float invPreExposure = 1.0f / Max( preExposure, 1e-20f );
	// the Karis weights see about what the tonemapper will: the scene is drawn with the last exposure
	const float lumaScale = settings.autoExposure ? 1.0f : R_ManualExposure( settings ) / Max( preExposure, 1e-20f );

	const d3d12GraphResource_t sceneColor = graph.Import( "SceneColor", { hdrColor.resource.Get(), &hdrColor.state, hdrColor.srv, {}, {}, width, height } );
	const d3d12GraphResource_t target = graph.Import( "Output", { output.resource.Get(), &output.state, output.srv, {}, scene.Rtv( false ), width, height } );
	const d3d12GraphResource_t lutTexture = graph.Import( "CombineLUT", { lut.Get(), &lutState, lutSrv, lutUav, {}, d3d12Lut::SIZE, d3d12Lut::SIZE } );
	const d3d12GraphResource_t histogramBuffer = graph.Import( "Exposure histogram", { histogram.Get(), &histogramState, {}, histogramUav, {},
		d3d12Exposure::BINS * 4, 1 } );
	const d3d12GraphResource_t eye = graph.Import( "EyeAdaptation", { eyeAdaptation.Get(), &eyeState, eyeSrv, eyeUav, {}, d3d12Exposure::EYE_BYTES, 1 } );

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
			[this, sceneColor, input, samples, lumaScale, rect]( idD3D12GraphContext & context ) {
				const std::array<uint32, 8> constants = { context.Srv( sceneColor ), context.Uav( input ), samples, std::bit_cast<uint32>( lumaScale ),
					static_cast<uint32>( rect.x ), static_cast<uint32>( rect.y ), static_cast<uint32>( rect.width ), static_cast<uint32>( rect.height ) };
				context.Dispatch( msaaResolve, nullptr, static_cast<uint32>( rect.width ), static_cast<uint32>( rect.height ), 1, constants );
			} );
	}

	const float binWidth = Max( settings.histogramMaxEV - settings.histogramMinEV, 1.0f ) / static_cast<float>( d3d12Exposure::BINS );
	const histogramParms_t histogramBase = { 0, 0, static_cast<uint32>( rect.x ), static_cast<uint32>( rect.y ), static_cast<uint32>( rect.width ),
		static_cast<uint32>( rect.height ), invPreExposure, idMath::ClampFloat( 0.0f, 1.0f, settings.meteringMask ), settings.histogramMinEV, 1.0f / binWidth };
	graph.AddPass( "Exposure histogram", d3d12GraphPass_t::COMPUTE, { { input, d3d12GraphAccess_t::SRV }, { histogramBuffer, d3d12GraphAccess_t::UAV } },
		[this, histogramBase, input, histogramBuffer]( idD3D12GraphContext & context ) {
			histogramParms_t histogramParms = histogramBase;
			histogramParms.sceneSrv = context.Srv( input );
			histogramParms.histogramUav = context.Uav( histogramBuffer );
			const d3d12UploadSpan_t span = context.Parms( histogramParms );
			context.Dispatch( exposureHistogram, &span, ( histogramParms.width + 1 ) / 2, ( histogramParms.height + 1 ) / 2 );
		} );
	const float lowPercent = idMath::ClampFloat( 0.0f, 0.99f, settings.lowPercent );
	const float highPercent = idMath::ClampFloat( lowPercent + 0.01f, 1.0f, settings.highPercent );
	const adaptParms_t adaptBase = { 0, 0, settings.histogramMinEV, binWidth, lowPercent, highPercent, settings.minEV100,
		Max( settings.maxEV100, settings.minEV100 ), Max( settings.speedUp, 0.0f ), Max( settings.speedDown, 0.0f ), Max( step.seconds, 0.0f ),
		( exposureReset || step.reset ) ? 1u : 0u, settings.autoExposure ? 1u : 0u, settings.exposureCompensation, settings.autoExposureBias,
		R_ManualExposure( settings ) };
	exposureReset = false;
	graph.AddPass( "Eye adaptation", d3d12GraphPass_t::COMPUTE, { { histogramBuffer, d3d12GraphAccess_t::UAV }, { eye, d3d12GraphAccess_t::UAV } },
		[this, adaptBase, histogramBuffer, eye]( idD3D12GraphContext & context ) {
			adaptParms_t adaptParms = adaptBase;
			adaptParms.histogramUav = context.Uav( histogramBuffer );
			adaptParms.eyeUav = context.Uav( eye );
			const d3d12UploadSpan_t span = context.Parms( adaptParms );
			context.Dispatch( exposureAdapt, &span, 1 );
		} );
	const uint32 slot = static_cast<uint32>( resolves % EYE_READBACKS );
	graph.AddPass( "EyeAdaptation readback", d3d12GraphPass_t::COPY, { { eye, d3d12GraphAccess_t::COPY_SOURCE } }, [this, eye, slot](
		idD3D12GraphContext & context ) {
		context.List().CopyBufferRegion( eyeReadback.Get(), slot * d3d12Exposure::EYE_BYTES, &context.Resource( eye ), 0, d3d12Exposure::EYE_BYTES );
	}, true );

	const uint32 bloomLevels = ( bloomDownsample && bloomBlur ) ? d3d12Bloom::Levels( settings, rect.width, rect.height ) : 0;
	const d3d12GraphResource_t bloom = ( bloomLevels > 0 ) ? AddBloom( graph, settings, input, width, height, rect, bloomLevels ) : d3d12GraphResource_t();
	const bool local = localExposureGrid && localExposureBlur && d3d12LocalExposure::Active( settings );
	const auto [grid, blurred] = local ? AddLocalExposure( graph, settings, input, rect, invPreExposure ) : std::pair<d3d12GraphResource_t, d3d12GraphResource_t>();
	const bool hidden = ( settings.visualize == 3 && bloomLevels == 0 ) || ( settings.visualize == 4 && !local );
	const uint32 visualize = hidden ? 0u : Min( static_cast<uint32>( settings.visualize ), 4u );
	const float bloomIntensity = ( bloomLevels > 0 ) ? idMath::ClampFloat( 0.0f, 1.0f, settings.bloomIntensity ) : 0.0f;
	const uint32 tiles[2] = { ( static_cast<uint32>( rect.width ) + d3d12LocalExposure::TILE - 1 ) / d3d12LocalExposure::TILE,
		( static_cast<uint32>( rect.height ) + d3d12LocalExposure::TILE - 1 ) / d3d12LocalExposure::TILE };
	const tonemapParms_t tonemapBase = { 0, 0, lutSampler, settings.dither ? 1u : 0u, invPreExposure, d3d12Lut::K, 1.0f / d3d12Lut::Range(), 0,
		visualize, settings.histogramMinEV, binWidth, lowPercent, highPercent, rect.x, rect.y, rect.width, rect.height, 0, bloomIntensity,
		1.0f / static_cast<float>( rect.width + ( rect.width & 1 ) ), 1.0f / static_cast<float>( rect.height + ( rect.height & 1 ) ), local ? 1u : 0u, 0, 0,
		Max( settings.localHighlightContrast, 0.0f ), Max( settings.localShadowContrast, 0.0f ), Max( settings.localDetailStrength, 0.0f ),
		idMath::ClampFloat( 0.0f, 1.0f, settings.localBlurredBlend ), GridMinLog( settings ), 1.0f / GridLogRange( settings ),
		1.0f / static_cast<float>( tiles[0] * d3d12LocalExposure::TILE ), 1.0f / static_cast<float>( tiles[1] * d3d12LocalExposure::TILE ) };
	std::vector<d3d12GraphUse_t> uses = { { input, d3d12GraphAccess_t::SRV }, { lutTexture, d3d12GraphAccess_t::SRV }, { eye, d3d12GraphAccess_t::SRV },
		{ target, d3d12GraphAccess_t::RTV, 0 } };
	if ( bloom.IsValid() ) {
		uses.push_back( { bloom, d3d12GraphAccess_t::SRV, 0 } );
	}
	if ( local ) {
		uses.push_back( { grid, d3d12GraphAccess_t::SRV } );
		uses.push_back( { blurred, d3d12GraphAccess_t::SRV } );
	}
	graph.AddPass( "Tonemap", d3d12GraphPass_t::RASTER, uses, [tonemapPso, tonemapBase, input, lutTexture, eye, target, bloom, grid, blurred, rect](
		idD3D12GraphContext & context ) {
		tonemapParms_t tonemapParms = tonemapBase;
		tonemapParms.sceneSrv = context.Srv( input );
		tonemapParms.lutSrv = context.Srv( lutTexture );
		tonemapParms.eyeSrv = context.Srv( eye );
		tonemapParms.bloomSrv = bloom.IsValid() ? context.Srv( bloom, 0 ) : 0;
		tonemapParms.gridSrv = grid.IsValid() ? context.Srv( grid ) : 0;
		tonemapParms.blurredSrv = blurred.IsValid() ? context.Srv( blurred ) : 0;
		const d3d12UploadSpan_t span = context.Parms( tonemapParms );
		context.Draw( *tonemapPso, target, 0, rect, span );
	} );
	graph.Execute( groups );
	// after Execute: a submit inside it closed the copy's list with an older fence, so this one is late, never early
	readbacks[slot] = { submitter.PendingFence(), resolves++, preExposure };
}

d3d12GraphResource_t idD3D12HdrPipeline::AddBloom( idD3D12RenderGraph & graph, const postProcessSettings_t & settings, d3d12GraphResource_t input,
	uint32 inputWidth, uint32 inputHeight, const renderRect_t & rect, uint32 levels ) {
	static constexpr std::array<const char *, d3d12Bloom::MAX_LEVELS> DOWNSAMPLE = { "Bloom downsample 1", "Bloom downsample 2", "Bloom downsample 3",
		"Bloom downsample 4", "Bloom downsample 5", "Bloom downsample 6" };
	static constexpr std::array<const char *, d3d12Bloom::MAX_LEVELS> BLUR_X = { "Bloom blur X 1", "Bloom blur X 2", "Bloom blur X 3", "Bloom blur X 4",
		"Bloom blur X 5", "Bloom blur X 6" };
	static constexpr std::array<const char *, d3d12Bloom::MAX_LEVELS> BLUR_Y = { "Bloom blur Y 1", "Bloom blur Y 2", "Bloom blur Y 3", "Bloom blur Y 4",
		"Bloom blur Y 5", "Bloom blur Y 6" };
	d3d12PoolDesc_t desc;
	desc.format = d3d12Bloom::FORMAT;
	desc.width = static_cast<uint32>( rect.width + 1 ) / 2;
	desc.height = static_cast<uint32>( rect.height + 1 ) / 2;
	desc.mips = static_cast<uint16>( levels );
	desc.unorderedAccess = true;
	const d3d12GraphResource_t chain = graph.Create( "Bloom chain", desc );
	const d3d12GraphResource_t blurred = graph.Create( "Bloom blurred", desc );
	const d3d12GraphResource_t sum = graph.Create( "Bloom", desc );

	for ( uint32 level = 0; level < levels; level++ ) {
		const uint16 mip = static_cast<uint16>( level );
		bloomDownsampleParms_t parms = {};
		parms.linearSampler = lutSampler;
		parms.targetSize[0] = desc.MipWidth( mip );
		parms.targetSize[1] = desc.MipHeight( mip );
		if ( level == 0 ) {
			parms.karis = 1;
			parms.sourceOrigin[0] = static_cast<float>( rect.x );
			parms.sourceOrigin[1] = static_cast<float>( rect.y );
			parms.invSourceSize[0] = 1.0f / static_cast<float>( inputWidth );
			parms.invSourceSize[1] = 1.0f / static_cast<float>( inputHeight );
			parms.sourceMin[0] = static_cast<float>( rect.x ) + 0.5f;
			parms.sourceMin[1] = static_cast<float>( rect.y ) + 0.5f;
			parms.sourceMax[0] = static_cast<float>( rect.x + rect.width ) - 0.5f;
			parms.sourceMax[1] = static_cast<float>( rect.y + rect.height ) - 0.5f;
			graph.AddPass( DOWNSAMPLE[level], d3d12GraphPass_t::COMPUTE, { { input, d3d12GraphAccess_t::SRV }, { chain, d3d12GraphAccess_t::UAV, mip } },
				[this, parms, input, chain]( idD3D12GraphContext & context ) {
					bloomDownsampleParms_t bound = parms;
					bound.sourceSrv = context.Srv( input );
					bound.targetUav = context.Uav( chain, 0 );
					const d3d12UploadSpan_t span = context.Parms( bound );
					context.Dispatch( bloomDownsample, &span, bound.targetSize[0], bound.targetSize[1] );
				} );
			continue;
		}
		const float sourceWidth = static_cast<float>( desc.MipWidth( mip - 1 ) );
		const float sourceHeight = static_cast<float>( desc.MipHeight( mip - 1 ) );
		parms.invSourceSize[0] = 1.0f / sourceWidth;
		parms.invSourceSize[1] = 1.0f / sourceHeight;
		parms.sourceMin[0] = 0.5f;
		parms.sourceMin[1] = 0.5f;
		parms.sourceMax[0] = sourceWidth - 0.5f;
		parms.sourceMax[1] = sourceHeight - 0.5f;
		graph.AddPass( DOWNSAMPLE[level], d3d12GraphPass_t::COMPUTE, { { chain, d3d12GraphAccess_t::SRV, static_cast<uint16>( mip - 1 ) },
			{ chain, d3d12GraphAccess_t::UAV, mip } }, [this, parms, chain, mip]( idD3D12GraphContext & context ) {
				bloomDownsampleParms_t bound = parms;
				bound.sourceSrv = context.Srv( chain, static_cast<uint16>( mip - 1 ) );
				bound.targetUav = context.Uav( chain, mip );
				const d3d12UploadSpan_t span = context.Parms( bound );
				context.Dispatch( bloomDownsample, &span, bound.targetSize[0], bound.targetSize[1] );
			} );
	}

	const std::array<idVec3, d3d12Bloom::MAX_LEVELS> tints = d3d12Bloom::Tints( settings, levels );
	for ( uint32 level = levels; level-- > 0; ) {
		const uint16 mip = static_cast<uint16>( level );
		const bool smaller = level + 1 < levels;
		const std::vector<float> weights = d3d12Bloom::Weights( settings, level, rect.width );
		bloomBlurParms_t parms = {};
		parms.smallerSrv = bloomBlurParms_t::NO_SMALLER;
		parms.linearSampler = lutSampler;
		parms.size[0] = desc.MipWidth( mip );
		parms.size[1] = desc.MipHeight( mip );
		parms.radius = static_cast<uint32>( weights.size() - 1 );
		for ( size_t t = 0; t < weights.size(); t++ ) {
			parms.weights[t / 4][t % 4] = weights[t];
		}
		parms.tint[0] = tints[level].x;
		parms.tint[1] = tints[level].y;
		parms.tint[2] = tints[level].z;
		if ( smaller ) {
			parms.smallerScaleX = 0.5f / static_cast<float>( desc.MipWidth( mip + 1 ) );
			parms.smallerScaleY = 0.5f / static_cast<float>( desc.MipHeight( mip + 1 ) );
		}
		graph.AddPass( BLUR_X[level], d3d12GraphPass_t::COMPUTE, { { chain, d3d12GraphAccess_t::SRV, mip }, { blurred, d3d12GraphAccess_t::UAV, mip } },
			[this, parms, chain, blurred, mip]( idD3D12GraphContext & context ) {
				bloomBlurParms_t bound = parms;
				bound.sourceSrv = context.Srv( chain, mip );
				bound.targetUav = context.Uav( blurred, mip );
				const d3d12UploadSpan_t span = context.Parms( bound );
				context.Dispatch( bloomBlur, &span, bound.size[0], bound.size[1] );
			} );
		const auto vertical = [this, parms, blurred, sum, mip, smaller]( idD3D12GraphContext & context ) {
			bloomBlurParms_t bound = parms;
			bound.vertical = 1;
			bound.sourceSrv = context.Srv( blurred, mip );
			bound.targetUav = context.Uav( sum, mip );
			bound.smallerSrv = smaller ? context.Srv( sum, static_cast<uint16>( mip + 1 ) ) : bloomBlurParms_t::NO_SMALLER;
			const d3d12UploadSpan_t span = context.Parms( bound );
			context.Dispatch( bloomBlur, &span, bound.size[1], bound.size[0] );
		};
		if ( smaller ) {
			graph.AddPass( BLUR_Y[level], d3d12GraphPass_t::COMPUTE, { { blurred, d3d12GraphAccess_t::SRV, mip },
				{ sum, d3d12GraphAccess_t::SRV, static_cast<uint16>( mip + 1 ) }, { sum, d3d12GraphAccess_t::UAV, mip } }, vertical );
		} else {
			graph.AddPass( BLUR_Y[level], d3d12GraphPass_t::COMPUTE, { { blurred, d3d12GraphAccess_t::SRV, mip }, { sum, d3d12GraphAccess_t::UAV, mip } },
				vertical );
		}
	}
	return sum;
}

std::pair<d3d12GraphResource_t, d3d12GraphResource_t> idD3D12HdrPipeline::AddLocalExposure( idD3D12RenderGraph & graph,
	const postProcessSettings_t & settings, d3d12GraphResource_t input, const renderRect_t & rect, float invPreExposure ) {
	using namespace d3d12LocalExposure;
	d3d12PoolDesc_t gridDesc;
	gridDesc.kind = d3d12PoolDesc_t::kind_t::TEXTURE_3D;
	gridDesc.format = FORMAT;
	gridDesc.width = ( static_cast<uint32>( rect.width ) + TILE - 1 ) / TILE;
	gridDesc.height = ( static_cast<uint32>( rect.height ) + TILE - 1 ) / TILE;
	gridDesc.depth = static_cast<uint16>( BINS );
	gridDesc.unorderedAccess = true;
	d3d12PoolDesc_t totalsDesc = gridDesc;
	totalsDesc.kind = d3d12PoolDesc_t::kind_t::TEXTURE_2D;
	totalsDesc.depth = 1;
	const d3d12GraphResource_t grid = graph.Create( "Local exposure grid", gridDesc );
	const d3d12GraphResource_t totals = graph.Create( "Local exposure totals", totalsDesc );
	const d3d12GraphResource_t blurredGrid = graph.Create( "Local exposure grid blurred", gridDesc );
	const d3d12GraphResource_t blurred = graph.Create( "Local exposure blurred luminance", totalsDesc );

	const float logRange = GridLogRange( settings );
	const localGridParms_t gridBase = { 0, 0, 0, static_cast<uint32>( rect.x ), static_cast<uint32>( rect.y ), static_cast<uint32>( rect.width ),
		static_cast<uint32>( rect.height ), invPreExposure, GridMinLog( settings ), static_cast<float>( BINS ) / logRange, logRange };
	graph.AddPass( "Local exposure grid", d3d12GraphPass_t::COMPUTE, { { input, d3d12GraphAccess_t::SRV }, { grid, d3d12GraphAccess_t::UAV, 0 },
		{ totals, d3d12GraphAccess_t::UAV, 0 } }, [this, gridBase, input, grid, totals, gridDesc]( idD3D12GraphContext & context ) {
			localGridParms_t parms = gridBase;
			parms.sceneSrv = context.Srv( input );
			parms.gridUav = context.Uav( grid, 0 );
			parms.totalsUav = context.Uav( totals, 0 );
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Dispatch( localExposureGrid, &span, gridDesc.width * 16, gridDesc.height * 16 );	// a group of 16 x 16 per tile
		} );

	// the kernel is 6 sigma
	const float sigma = Max( settings.localBlurredKernel * 0.01f * static_cast<float>( rect.width ) / ( 6.0f * static_cast<float>( TILE ) ), 0.25f );
	const int32 radius = Min( static_cast<int32>( std::ceil( 3.0f * sigma ) ), static_cast<int32>( Max( gridDesc.width, gridDesc.height ) ) );
	const localBlurParms_t blurBase = { 0, 0, 0, 0, gridDesc.width, gridDesc.height, 1.0f / ( 2.0f * sigma * sigma ), radius };
	graph.AddPass( "Local exposure blur", d3d12GraphPass_t::COMPUTE, { { grid, d3d12GraphAccess_t::SRV }, { totals, d3d12GraphAccess_t::SRV },
		{ blurredGrid, d3d12GraphAccess_t::UAV, 0 }, { blurred, d3d12GraphAccess_t::UAV, 0 } },
		[this, blurBase, grid, totals, blurredGrid, blurred]( idD3D12GraphContext & context ) {
			localBlurParms_t parms = blurBase;
			parms.gridSrv = context.Srv( grid );
			parms.totalsSrv = context.Srv( totals );
			parms.gridUav = context.Uav( blurredGrid, 0 );
			parms.blurredUav = context.Uav( blurred, 0 );
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Dispatch( localExposureBlur, &span, parms.tilesX, parms.tilesY, BINS + 1 );
		} );
	return { blurredGrid, blurred };
}

// A slot's fence is its last copy's, so a slot read is never being written.
void idD3D12HdrPipeline::Poll() {
	if ( device.IsLost() ) {
		return;
	}
	const eyeReadback_t * newest = nullptr;
	for ( const eyeReadback_t & readback : readbacks ) {
		if ( readback.fence != 0 && readback.fence <= submitter.WaitedFence() && ( newest == nullptr || readback.order > newest->order ) ) {
			newest = &readback;
		}
	}
	if ( newest == nullptr ) {
		return;
	}
	const uint32 slot = static_cast<uint32>( newest - readbacks.data() );
	const eyeReadback_t taken = *newest;
	for ( eyeReadback_t & readback : readbacks ) {
		if ( readback.fence != 0 && readback.order <= taken.order ) {
			readback.fence = 0;
		}
	}
	const SIZE_T offset = static_cast<SIZE_T>( slot ) * d3d12Exposure::EYE_BYTES;
	const D3D12_RANGE read = { offset, offset + d3d12Exposure::EYE_BYTES };
	void * mapped = nullptr;
	if ( !device.Check( eyeReadback->Map( 0, &read, &mapped ), "Map (eye adaptation readback)" ) ) {
		return;
	}
	const byte * eye = static_cast<const byte *>( mapped ) + offset;
	float values[4];
	d3d12ExposureReadback_t result;
	memcpy( values, eye, sizeof( values ) );
	memcpy( result.histogram.data(), eye + d3d12Exposure::HISTOGRAM_OFFSET, sizeof( result.histogram ) );
	const D3D12_RANGE written = { 0, 0 };
	eyeReadback->Unmap( 0, &written );
	result.exposure = { values[0], values[1], values[2], values[3], taken.preExposure };
	lastExposure = result;
}
