#version ogre_glsl_ver_330

vulkan_layout( location = 0 )
out float fragColour;

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

//Morton Order. Table generated in Python:
//def CompactBy1( x ):
//	x &= 0x55555555
//	x = (x ^ (x >>  1)) & 0x33333333
//	x = (x ^ (x >>  2)) & 0x0f0f0f0f
//	x = (x ^ (x >>  4)) & 0x00ff00ff
//	x = (x ^ (x >>  8)) & 0x0000ffff
//	return x
//def printMorton( val ):
//	x = CompactBy1( val )
//	y = CompactBy1( val >> 1 )
//	print( "\tvec2( %s, %s )," % (x, y) )
//
//for x in range(0, 16):
//	printMorton(x)

const vec2 c_offsets[16] = vec2[16]
(
	vec2( 0, 0 ), vec2( 1, 0 ), vec2( 0, 1 ), vec2( 1, 1 ),
	vec2( 2, 0 ), vec2( 3, 0 ), vec2( 2, 1 ), vec2( 3, 1 ),
	vec2( 0, 2 ), vec2( 1, 2 ), vec2( 0, 3 ), vec2( 1, 3 ),
	vec2( 2, 2 ), vec2( 3, 2 ), vec2( 2, 3 ), vec2( 3, 3 )
);

//Luminance coefficient taken from the DX SDK Docs
const vec3 c_luminanceCoeffs = vec3(0.2125f, 0.7154f, 0.0721f);
//Luminance vector for RGB colour in linear space (the usual coeffs are for gamma space colours)
//const vec3 c_luminanceCoeffs = vec3( 0.3086f, 0.6094f, 0.0820f );

vulkan_layout( ogre_t0 ) uniform texture2D rt0;
vulkan( layout( ogre_s0 ) uniform sampler samplerState );

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 tex0Size;
	uniform vec4 viewportSize;
vulkan( }; )

// JAHSHAKA patch 0042 - A SAMPLE THAT IS NOT A NUMBER MUST NOT POISON THE FRAME.
//
// This pass is the WHOLE SCENE's luminance measurement: it takes the mean of
// log( luminance ) over a sparse grid, and DownScale02/03 then average those
// means. A mean has no resistance at all - ONE unusable sample makes the
// frame's measurement unusable, and DownScale03 then has to guess an exposure
// for the whole picture.
//
// Unusable samples really do occur, measured (Jahshaka lane HDR-1, 2026-09-15,
// Ogre's ShadowMapFromCode port, a 0.35 deg per frame sun drag): 24 of 120
// dragging frames with SSR on and 9 with SSR off produced a NaN measurement,
// and the exposure lurched on every one of them. As the light turns, which
// texels are unrepresentable and which filter weights are zero changes every
// frame, so the measurement flickers between usable and unusable - which is the
// owner's 'materials shimmer while I drag the light', arriving as a grade that
// lurches while the light moves and settles the moment it stops.
//
// THE RULE: a sample's influence on the frame must be BOUNDED. See the two ways
// a sample stops being a luminance in the function below.
//
// NOT FIXED HERE, deliberately, because it is not this pass's business and
// because it MOVES EVERY EXISTING PICTURE: the scene target still stores +Inf,
// and the tonemapper still turns it into a black hole. That belongs upstream of
// the tonemapper (the specular lobe, or the roughness floor), and Jahshaka patch
// 0034's header records the measurement that rejected clamping it in the last
// pass. This patch fixes the METER, and changes no pixel of any frame whose
// samples were all finite and non-negative.
const float c_jahMaxRepresentableLum = 65504.0;	// the largest half float
const float c_jahMinLuminance = 0.0001;			// the epsilon the caller already adds
float jahUsableLuminance( float lum )
{
	// TWO WAYS A SAMPLE STOPS BEING A LUMINANCE, and log() turns both into a NaN
	// that a MEAN cannot survive:
	//   * NOT REPRESENTABLE. The scene target is RGBA16F and a punctual light's
	//     specular lobe goes as 1/(pi*alpha^2), so a near-mirror surface stores
	//     +Inf; the bilinear fetch above then weights that texel by zero on some
	//     frames and not others, and 0 * Inf is a NaN.
	//   * NEGATIVE. Luminance is non-negative by construction, but the scene
	//     target is a SUM of shading terms and a filtered fetch across a blown
	//     neighbourhood does come back below zero - measured, and the source of
	//     every NaN this pass produced on the fixture above. log() of it is a
	//     NaN. The caller already adds 0.0001 as its floor, so the lower bound
	//     only bites a sample that was already not a measurement.
	//
	// THE TEST IS ON THE BITS, NOT `lum == lum`, AND THAT IS NOT A STYLE CHOICE.
	// Jahshaka measured (lane HDR-1, NVIDIA 595.84, Vulkan/SPIR-V) that `x == x`
	// is FOLDED TO TRUE by the shader compiler on this stack: a deliberately
	// absurd value placed on the NaN branch of patch 0034's guard in DownScale03
	// never appeared in 120 frames that demonstrably carried NaN texels. An
	// IEEE-754 float is a NaN exactly when its exponent is all ones and its
	// mantissa is not zero, i.e. when the magnitude of its bit pattern exceeds
	// +Inf's; that is integer arithmetic and no optimiser may remove it.
	//
	// An unrepresentable sample reads as the brightest value the target CAN hold,
	// which is a statement about that one sample and is worth at most
	// log(65504*1024) = 18 against an ordinary 7 - 1.7e-4 of the frame's mean.
	// Every sample that IS a luminance is returned unchanged, bit for bit.
	uint jahBits = floatBitsToUint( lum ) & 0x7FFFFFFFu;	// magnitude only
	bool jahFinite = jahBits < 0x7F800000u;					// 0x7F800000 is +Inf
	return jahFinite ? clamp( lum, c_jahMinLuminance, c_jahMaxRepresentableLum )
					 : c_jahMaxRepresentableLum;
}

void main()
{
	//Compute how many pixels we have to skip because we can't sample them all
	//e.g we have a 4096x4096 viewport (rt0), and we're rendering to a 64x64 surface
	//We would need 64x64 samples, but we only sample 4x4, therefore we sample one
	//pixel and skip 15, then repeat. We perform:
	//(ViewportResolution / TargetResolution) / 4
	vec2 ratio = tex0Size.xy * viewportSize.zw * 0.25;

	vec3 vSample	= texture( vkSampler2D( rt0, samplerState ), inPs.uv0 ).xyz;
	float sampleLum	= dot( vSample, c_luminanceCoeffs ) + 0.0001;
	sampleLum = jahUsableLuminance( sampleLum );
	//float fLogLuminance = log( clamp( sampleLum, c_minLuminance, c_maxLuminance ) );
	float fLogLuminance = log( sampleLum * 1024.0 );

	for( int i=1; i<16; ++i )
	{
		//TODO: Precompute c_offsets[i] * ratio in CPU and upload it as c_offset, probably using a listener
		vSample		= texture( vkSampler2D( rt0, samplerState ),
							   inPs.uv0 + ((c_offsets[i] * ratio) * tex0Size.zw) ).xyz;
		sampleLum	= jahUsableLuminance( dot( vSample, c_luminanceCoeffs ) + 0.0001 );
		//fLogLuminance += log( clamp( sampleLum, c_minLuminance, c_maxLuminance ) );
		fLogLuminance += log( sampleLum * 1024.0 );
	}

	fLogLuminance *= 0.0625; // /= 16.0;

	fragColour = fLogLuminance;
}
