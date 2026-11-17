// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
float4 idGLClipToD3D( float4 p ) { return float4( p.x, -p.y, ( p.z + p.w ) * 0.5, p.w ); }
float4 tex2D( Texture2D t, SamplerState s, float2 uv ) { return t.Sample( s, uv ); }
float4 tex2Dproj( Texture2D t, SamplerState s, float3 uvw ) { return t.Sample( s, uvw.xy / uvw.z ); }
float4 tex2Dproj( Texture2D t, SamplerState s, float4 uvw ) { return t.Sample( s, uvw.xy / uvw.w ); }
float4 tex2Dlod( Texture2D t, SamplerState s, float4 uv ) { return t.SampleLevel( s, uv.xy, uv.w ); }
float4 tex2Dbias( Texture2D t, SamplerState s, float4 uv ) { return t.SampleBias( s, uv.xy, uv.w ); }
float4 texCUBE( TextureCube t, SamplerState s, float3 dir ) { return t.Sample( s, dir ); }
Texture2D samp0_t : register( t0 );
SamplerState samp0_s : register( s0 );
struct PS_IN {
	float4 position;
	float2 texcoord0;
	float4 texcoord1;
	float4 color;
};
struct PS_OUT {
	float4 color;
};
void idMain (PS_IN fragment , inout PS_OUT result )  {
float4 color = ( tex2D ( samp0_t, samp0_s , fragment . texcoord0 ) * fragment . color ) + fragment . texcoord1 ; 
result . color . xyz = color . xyz * color . w ; 
result . color . w = color . w ; 
}
struct idVaryings {
	float4 position : SV_Position;
	float4 t0 : TEXCOORD0;
	float4 t1 : TEXCOORD1;
	float4 c0 : COLOR0;
};
float4 psMain( idVaryings v ) : SV_Target0 {
	PS_IN fragment;
	fragment.position = float4( v.position.xyz, 1.0 / v.position.w );
	fragment.texcoord0 = v.t0.xy;
	fragment.texcoord1 = v.t1;
	fragment.color = v.c0;
	PS_OUT result = (PS_OUT)0;
	idMain( fragment, result );
	return result.color;
}
