#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D depthTexture;
vulkan_layout( ogre_t1 ) uniform texture2D gBuf_normals;
vulkan_layout( ogre_t2 ) uniform texture2D noiseTexture;

vulkan( layout( ogre_s0 ) uniform sampler samplerState0 );
vulkan( layout( ogre_s2 ) uniform sampler samplerState2 );

vulkan_layout( location = 0 )
in block
{
   vec2 uv0;
   vec3 cameraDir;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec2 projectionParams;
	uniform float invKernelSize;
	uniform float kernelRadius;
	uniform vec2 noiseScale;
	uniform mat4 projection;

	// Jahshaka local patch (orthoview lane, ogre-patch 0019): x = 1 when the
	// camera is ORTHOGRAPHIC. See the branch below - none of this file's
	// position reconstruction is valid under an ortho frustum without it.
	uniform vec4 jahOrthoParams;

	uniform vec4 sampleDirs[64];
vulkan( }; )

vulkan_layout( location = 0 )
out float fragColour;

vec3 getScreenSpacePos(vec2 uv, vec3 cameraNormal)
{
	float fDepth = texture( vkSampler2D( depthTexture, samplerState0 ), uv ).x;

	// Jahshaka local patch (upstream-reportable): AN ORTHOGRAPHIC CAMERA
	// RECONSTRUCTS DIFFERENTLY, and without this branch it reconstructs a
	// position that MOVES WITH THE CAMERA.
	//
	// `cameraNormal` is the quad's interpolated view-space far corner
	// (VIEW_SPACE_CORNERS). For a perspective frustum that is a RAY through
	// the pixel, so scaling it by the depth fraction lands on the surface. For
	// an orthographic one it is not a ray at all: OgreFrustum.cpp:884 takes
	// ratio = 1, so the far corners have the SAME xy as the near ones and that
	// xy already IS this pixel's view-space xy, at every depth. Scaling it by
	// depth makes the reconstructed position slide as the camera pans - which
	// in an editor's top/front/side view reads as the contact shadowing (and,
	// through the same defect in screen-space reflections, the highlights)
	// crawling over a scene that is not moving.
	//
	// The depth pair is the other half. `Frustum::getProjectionParamsAB`
	// returns a pair for which `B / (d - A)` is 1/t for an ortho frustum
	// (OgreFrustum.cpp:128-141), and MINUS 1/t without reverse depth, so the
	// expression is inverted and taken absolute - reverse-Z safe by
	// construction. The host multiplies B by the far plane for an ortho camera
	// exactly as it divides it for a perspective one, so both branches hand
	// back the same [0,1] fraction of the far plane and everything downstream
	// (the range check, the sample comparison, the blur weights) is unchanged.
	if( jahOrthoParams.x > 0.5 )
	{
		float orthoDepth = abs( (fDepth - projectionParams.x) / projectionParams.y );
		return vec3( cameraNormal.xy, cameraNormal.z * orthoDepth );
	}

	float linearDepth = projectionParams.y / (fDepth - projectionParams.x);
	return (cameraNormal * linearDepth);
}

vec3 reconstructNormal(vec3 posInView)
{
	vec3 dNorm = cross(normalize(dFdy(posInView)), normalize(dFdx(posInView)));
	return dNorm;
}

vec3 getRandomVec(vec2 uv)
{
	vec3 randomVec = texture( vkSampler2D( noiseTexture, samplerState2 ), uv * noiseScale ).xyz;
	return randomVec;
}

void main()
{
	// Jahshaka local patch (upstream-reportable): reject the far plane.
	// Nothing was rendered there, so there is no geometry to occlude and the
	// normals G-buffer was never written for those pixels (a sky quad writes
	// colour only) - the 64-tap march then compares a uniform depth against
	// itself, which is a knife edge, and returns ~half occlusion modulated
	// by the rotation noise. In a scene with a SKY that reads as dither noise
	// over the whole sky, ~45% darker. The tutorial's own scene has no sky,
	// so upstream never saw it. Both extremes are tested, so this is correct
	// with and without reverse depth (Vulkan defaults to reverse: far == 0).
	float jahRawDepth = texture( vkSampler2D( depthTexture, samplerState0 ), inPs.uv0 ).x;
	if( jahRawDepth <= 0.0 || jahRawDepth >= 1.0 )
	{
		fragColour = 1.0;   // fully unoccluded
		return;
	}
    vec3 viewPosition = getScreenSpacePos(inPs.uv0, inPs.cameraDir);
    //vec3 viewNormal = reconstructNormal(viewPosition);
	vec3 viewNormal = normalize( texture( vkSampler2D( gBuf_normals, samplerState0 ),inPs.uv0 ).xyz * 2.0 - 1.0 );
    vec3 randomVec = getRandomVec(inPs.uv0);
   
    vec3 tangent = normalize(randomVec - viewNormal * dot(randomVec, viewNormal));
    vec3 bitangent = cross(viewNormal, tangent);
    mat3 TBN = mat3(tangent, bitangent, viewNormal);
   
    float occlusion = 0.0;
    for(int i = 0; i < 8; ++i)
    {
		for(int a = 0; a < 8; ++a)
		{
			vec3 sNoise = sampleDirs[(a << 3u) + i].xyz;
         
			// get sample position
			vec3 oSample = TBN * sNoise; // From tangent to view-space
			oSample = viewPosition + oSample * kernelRadius;
        
			// project sample position
			vec4 offset = vec4(oSample, 1.0);
			offset = projection * offset; // from view to clip-space
			offset.xyz /= offset.w; // perspective divide
			offset.xyz = offset.xyz * 0.5 + 0.5; // transform to range 0.0 - 1.0
			offset.y = 1.0 - offset.y;

			float sampleDepth = getScreenSpacePos(offset.xy, inPs.cameraDir).z;

			float rangeCheck = smoothstep(0.0, 1.0, kernelRadius / abs(viewPosition.z - sampleDepth));
			occlusion += (sampleDepth >= oSample.z ? 1.0 : 0.0) * rangeCheck;
			
		}      
    }
    occlusion = 1.0 - (occlusion * invKernelSize);
   
    fragColour = occlusion;
}
