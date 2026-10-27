#define PI 3.14159265359f

struct PS_INPUT
{
	float3 cameraDir	: TEXCOORD0;
};

Texture2DArray<float4> skyEquirectangular	: register(t0);
SamplerState samplerState					: register(s0);

float4 main
(
	PS_INPUT inPs,

	uniform float sliceIdx,
	uniform float4 skyGain	// Jahshaka (PHOTON-7 10-3 R5): the sky's gain, where the picture is formed
) : SV_Target0
{
	float3 cameraDir = normalize( inPs.cameraDir );
	float2 longlat;
	longlat.x = atan2( cameraDir.x, -cameraDir.z ) + PI;
	longlat.y = acos( cameraDir.y );
	float2 uv = longlat / float2( 2.0f * PI, PI );

	float4 c = skyEquirectangular.Sample( samplerState, float3( uv.xy, sliceIdx ) );
	c.xyz *= skyGain.xyz;
	return c;
}
