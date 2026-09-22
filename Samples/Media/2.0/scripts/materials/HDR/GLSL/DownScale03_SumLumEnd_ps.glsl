#version ogre_glsl_ver_330

vulkan_layout( location = 0 )
out float fragColour;

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

const vec2 c_offsets[4] = vec2[4]
(
	vec2( -1.0, -1.0 ), vec2( 1.0, -1.0 ),
	vec2( -1.0,  1.0 ), vec2( 1.0,  1.0 )
);

vulkan_layout( ogre_t0 ) uniform texture2D lumRt;
vulkan( layout( ogre_s0 ) uniform sampler samplerBilinear );

vulkan_layout( ogre_t1 ) uniform texture2D oldLumRt;
vulkan( layout( ogre_s1 ) uniform sampler samplerPoint );

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec3 exposure;
	uniform float timeSinceLast;
	uniform vec4 tex0Size;
vulkan( }; )

void main()
{
	float fLumAvg = texture( vkSampler2D( lumRt, samplerBilinear ),
							 inPs.uv0 + c_offsets[0] * tex0Size.zw ).x;

	for( int i=1; i<4; ++i )
	{
		fLumAvg += texture( vkSampler2D( lumRt, samplerBilinear ),
							inPs.uv0 + c_offsets[i] * tex0Size.zw ).x;
	}

	fLumAvg *= 0.25; // /= 4.0;

	// JAHSHAKA: A NaN HERE IS PERMANENT, AND NOTHING ELSE IN THIS CHAIN IS.
	//
	// This pass writes a 1x1 keep_content texture and the mix below reads that
	// same texture back every frame, so the adapted luminance is a recurrence
	// with no other input. An +Inf measurement is survivable — clamp() pins it
	// to exposure.z and the next frame recovers — but a NaN is not: clamp() is
	// min(max(x,lo),hi), max(NaN,lo) returns NaN on every driver we measured,
	// exp(NaN) is NaN, and mix(NaN, oldLum, w) is NaN, so ONE unusable frame is
	// the exposure for the rest of the workspace's life. Measured in Jahshaka
	// (2026-09-14): entering the Player on a bright scene froze the adapted
	// luminance from frame one for 600+ frames, and only a workspace rebuild —
	// the one thing that re-clears this texture — brought it back.
	//
	// `x == x` is false for a NaN and true for everything else, Inf included,
	// so a chain that was already producing numbers produces exactly the same
	// numbers. exposure.y is the log-luminance floor the clamp below already
	// uses, i.e. what this pass would have written for a dark frame.
	// JAHSHAKA patch 0042 - AN UNUSABLE MEASUREMENT MUST MOVE THE EXPOSURE NOWHERE.
	//
	// Patch 0034 made a NaN survivable (it can no longer latch this 1x1 history
	// for ever) by reading it as exposure.y. That is the LOG-LUMINANCE FLOOR, so
	// an unusable frame was read as 'the darkest scene this chain admits' - and
	// exposure.x / exp( exposure.y ) is the LARGEST exposure the chain can
	// produce. A measurement failure therefore yanked the grade towards its
	// brightest limit, which is the loudest thing it could possibly do.
	//
	// Measured (lane HDR-1, 2026-09-15, ShadowMapFromCode, 0.35 deg/frame sun
	// drag, the adaptation history read back per frame): every anomalous frame
	// reconstructed to newLum = exposure.x / exp( exposure.y ) to four figures,
	// against a neighbourhood median of 0.84 - and when exposureMin was widened
	// from -2.5 to -6 the same frames moved to the new constant exactly, tracking
	// exposure.y. That is this fallback, not a scene that got dark.
	//
	// A missing measurement carries NO information about the scene, so the only
	// answer that cannot invent a grade change is the one already on the books:
	// hold the adapted luminance for this frame and adapt normally on the next.
	// The dark-frame reading survives for the ONE case it was written for - a
	// history that is ALSO unusable, where there is nothing to hold.
	//
	// ON THE BITS, NOT `x == x`: measured on this stack (NVIDIA 595.84,
	// Vulkan/SPIR-V), patch 0034's `x == x` tests are FOLDED TO TRUE by the
	// shader compiler and never fire; what produced 0034's constant was the
	// DRIVER's clamp( NaN, lo, hi ) returning lo, the same number by accident. A
	// float is a NaN exactly when the magnitude of its bit pattern exceeds +Inf's,
	// which is integer arithmetic and survives every optimiser. (An Inf reads as
	// 'no measurement' too: with DownScale01 bounded above, fLumAvg cannot reach
	// one from a real frame.)
	//
	// Every finite frame takes the same arithmetic it took before, bit for bit.
	bool jahHaveMeasurement = ( ( floatBitsToUint( fLumAvg ) & 0x7FFFFFFFu ) < 0x7F800000u );
	float newLum = exposure.x / exp( clamp( jahHaveMeasurement ? fLumAvg : exposure.y,
											exposure.y, exposure.z ) );
	float oldLum = texture( vkSampler2D( oldLumRt, samplerPoint ), vec2( 0.0, 0.0 ) ).x;
	bool jahHaveHistory = ( ( floatBitsToUint( oldLum ) & 0x7FFFFFFFu ) < 0x7F800000u );
	oldLum = jahHaveHistory ? oldLum : newLum;
	newLum = ( !jahHaveMeasurement && jahHaveHistory ) ? oldLum : newLum;

	//Adapt luminicense based 75% per second.
	fragColour = mix( newLum, oldLum, pow( 0.25, timeSinceLast ) );
}
