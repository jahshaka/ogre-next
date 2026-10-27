struct PS_INPUT
{
	float3 cameraDir	: TEXCOORD0;
};

TextureCube<float4> skyCubemap	: register(t0);
SamplerState samplerState		: register(s0);

float4 main
(
	PS_INPUT inPs,

	uniform float4 skyGain	// Jahshaka (PHOTON-7 10-3 R5): the sky's gain, where the picture is formed
) : SV_Target0
{
	//Cubemaps are left-handed
	float4 c = skyCubemap.Sample( samplerState, float3( inPs.cameraDir.xy, -inPs.cameraDir.z ) );
	c.xyz *= skyGain.xyz;
	return c;
}
