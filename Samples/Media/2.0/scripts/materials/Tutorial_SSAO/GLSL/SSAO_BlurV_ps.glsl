#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D ssaoTexture;
vulkan_layout( ogre_t1 ) uniform texture2D depthTexture;

vulkan( layout( ogre_s0 ) uniform sampler samplerState );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 texelSize;
	uniform vec2 projectionParams;

	// Jahshaka local patch (orthoview lane, ogre-patch 0019): x = 1 when the
	// camera is ORTHOGRAPHIC. See the branch below - none of this file's
	// position reconstruction is valid under an ortho frustum without it.
	uniform vec4 jahOrthoParams;
vulkan( }; )

const float offsets[9] = float[9]( -8.0, -6.0, -4.0, -2.0, 0.0, 2.0, 4.0, 6.0, 8.0 );

vulkan_layout( location = 0 )
out float fragColour;

float getLinearDepth(vec2 uv)
{
	float fDepth = texture( vkSampler2D( depthTexture, samplerState ), uv ).x;
	// Jahshaka local patch (orthoview lane): the ORTHO pair is the reciprocal
	// of this one - same reasoning, and the same two lines, as
	// SSAO_HS_ps.glsl's getScreenSpacePos. It matters HERE because the blur
	// weight is 1/|depth difference|: read the wrong way round, an ortho
	// frame's differences collapse toward zero, every weight becomes equal,
	// and the depth-aware blur silently degrades into a box blur that smears
	// the occlusion across every silhouette.
	if( jahOrthoParams.x > 0.5 )
		return abs( (fDepth - projectionParams.x) / projectionParams.y );
	float linearDepth = projectionParams.y / (fDepth - projectionParams.x);
	return linearDepth;
}

void main()
{
	float flDepth = getLinearDepth(inPs.uv0);
	
	float weights = 0.0;
	float result = 0.0;

	for (int i = 0; i < 9; ++i)
	{
		vec2 offset = vec2(0.0, texelSize.w*offsets[i]); //Vertical sample offsets
		vec2 samplePos = inPs.uv0 + offset;

		float slDepth = getLinearDepth(samplePos);

		float weight = (1.0 / (abs(flDepth - slDepth) + 0.0001)); //Calculate weight using depth

		result += texture( vkSampler2D( ssaoTexture, samplerState ), samplePos ).x*weight;

		weights += weight;
	}
	result /= weights;

	fragColour = result;

	//fragColour = texture(vkSampler2D( ssaoTexture, samplerState ), inPs.uv0).x; //Use this to disable blur
}
