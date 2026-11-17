// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
float4 idGLClipToD3D( float4 p ) { return float4( p.x, -p.y, ( p.z + p.w ) * 0.5, p.w ); }
float4 tex2D( Texture2D t, SamplerState s, float2 uv ) { return t.Sample( s, uv ); }
float4 tex2Dproj( Texture2D t, SamplerState s, float3 uvw ) { return t.Sample( s, uvw.xy / uvw.z ); }
float4 tex2Dproj( Texture2D t, SamplerState s, float4 uvw ) { return t.Sample( s, uvw.xy / uvw.w ); }
float4 tex2Dlod( Texture2D t, SamplerState s, float4 uv ) { return t.SampleLevel( s, uv.xy, uv.w ); }
float4 tex2Dbias( Texture2D t, SamplerState s, float4 uv ) { return t.SampleBias( s, uv.xy, uv.w ); }
float4 texCUBE( TextureCube t, SamplerState s, float3 dir ) { return t.Sample( s, dir ); }
cbuffer idFragmentParms : register( b1 ) {
	float4 rpColor;
};
Texture2D samp0_t : register( t0 );
SamplerState samp0_s : register( s0 );
struct PS_IN {
	float4 position;
	float2 texcoord0;
};
struct PS_OUT {
	float4 color;
};
void idMain (PS_IN fragment , inout PS_OUT result )  {
result . color = tex2D ( samp0_t, samp0_s , fragment . texcoord0 ) * rpColor ; 
}
struct idVaryings {
	float4 position : SV_Position;
	float4 t0 : TEXCOORD0;
};
float4 psMain( idVaryings v ) : SV_Target0 {
	PS_IN fragment;
	fragment.position = float4( v.position.xyz, 1.0 / v.position.w );
	fragment.texcoord0 = v.t0.xy;
	PS_OUT result = (PS_OUT)0;
	idMain( fragment, result );
	return result.color;
}
