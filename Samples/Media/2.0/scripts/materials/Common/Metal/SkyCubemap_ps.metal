#include <metal_stdlib>
using namespace metal;

struct PS_INPUT
{
	float3 cameraDir;
};

fragment float4 main_metal
(
	PS_INPUT inPs [[stage_in]],
	texturecube<float> skyCubemap [[texture(0)]],
	sampler samplerState [[sampler(0)]],

	// Jahshaka (PHOTON-7 10-3 R5): the sky's gain, where the picture is formed
	constant float4 &skyGain [[buffer(PARAMETER_SLOT)]]
)
{
	//Cubemaps are left-handed
	float4 c = skyCubemap.sample( samplerState, float3( inPs.cameraDir.xy, -inPs.cameraDir.z ) ).xyzw;
	return float4( c.xyz * skyGain.xyz, c.w );
}
