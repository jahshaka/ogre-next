#version ogre_glsl_ver_330

vulkan_layout( location = 0 )
out vec4 fragColour;

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

//See Hable_John_Uncharted2_HDRLighting.pptx
//See http://filmicgames.com/archives/75
//See https://expf.wordpress.com/2010/05/04/reinhards_tone_mapping_operator/
/*const float A = 0.15;
const float B = 0.50;
const float C = 0.10;
const float D = 0.20;
const float E = 0.02;
const float F = 0.30;
const float W = 11.2;*/
const float A = 0.22;
const float B = 0.3;
const float C = 0.10;
const float D = 0.20;
const float E = 0.01;
const float F = 0.30;
const float W = 11.2;

vec3 FilmicTonemap( vec3 x )
{
   return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}
float FilmicTonemap( float x )
{
   return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}

vec3 fromSRGB( vec3 x )
{
	return x * x;
}

vulkan_layout( ogre_t0 ) uniform texture2D rt0;
vulkan_layout( ogre_t1 ) uniform texture2D lumRt;
vulkan_layout( ogre_t2 ) uniform texture2D bloomRt;

vulkan( layout( ogre_s0 ) uniform sampler samplerPoint );
vulkan( layout( ogre_s2 ) uniform sampler samplerBilinear );

// JAHSHAKA (patch 0079, lane DITHER-1): the dither that makes this quad's
// 8-bit write honest. This is THE place a floating-point picture becomes
// display codes in this engine -- every target this material ever writes is
// 8-bit UNORM (the window, the offscreen render target, the VR eye image, the
// LDR buffer SMAA works on, the looks stage's first buffer, the
// picture-in-picture inset) -- so the dither is unconditional here and needed
// nowhere else. Why, how big and why it is keyed on the pixel alone: the
// header, which is Jahshaka media and is found through the resource group.
#include "JahDither.glsl"
vulkan( layout( ogre_P0 ) uniform Params { )
	// THE SAFE DEFAULT IS ZERO, i.e. DITHERED: a zero-filled constant buffer
	// (a frame drawn before the host has pushed anything, and any target
	// this material reaches through a path that does not push) renders the
	// CORRECT picture rather than a banded one, and a garbage value is
	// clamped to "off", i.e. to the picture this engine drew before the
	// dither existed. Neither failure can produce noise.
	uniform float jahDitherOff;	// 0 = dither (normal), 1 = JAHSHAKA_NO_DITHER
	// JAHSHAKA (patch 0082, lane BLOOM-AMOUNT-1): HOW MUCH of the blurred
	// highlight this quad adds -- the World panel's Bloom Amount, 0 to 2,
	// with 1 the picture this engine has always drawn.
	//
	// IT IS THE AMOUNT MINUS ONE, and that spelling is the same safety rule
	// as jahDitherOff above: the value a constant buffer nobody has written
	// carries is zero, and zero here means ONE -- the unscaled picture --
	// rather than a frame with the bloom silently missing. A garbage value
	// is clamped to the document's own range.
	//
	// AND IT IS APPLIED HERE, NOT IN THE LADDER. The bright pass writes an
	// R10G10B10A2 UNORM target, so scaling it would CLIP at 1.0, and this
	// quad reads that target through fromSRGB (a square), so a scale over
	// there would arrive squared. At the composite the multiply is linear
	// in the radiance the bloom adds, which is what "twice the bloom" has
	// to mean.
	uniform float jahBloomAmountMinusOne;	// 0 = 1x (normal), -1 = none, 1 = 2x
vulkan( }; )

void main()
{
	float fInvLumAvg = texture( vkSampler2D( lumRt, samplerPoint ), vec2( 0.0, 0.0 ) ).x;

	vec4 vSample = texture( vkSampler2D( rt0, samplerPoint ), inPs.uv0 );

	vSample.xyz *= fInvLumAvg;
	vSample.xyz	+= fromSRGB( texture( vkSampler2D( bloomRt, samplerBilinear ),
									  inPs.uv0 ).xyz ) *
				   ( 16.0 * clamp( 1.0 + jahBloomAmountMinusOne, 0.0, 2.0 ) );
	vSample.xyz  = FilmicTonemap( vSample.xyz ) / FilmicTonemap( W );
	//vSample.xyz  = vSample.xyz / (1 + vSample.xyz); //Reinhard Simple
	vSample.xyz  = ( vSample.xyz - 0.5 ) * 1.25 + 0.5 + 0.11;

	// The INTEGER pixel, which is the dither's only key. gl_FragCoord.xy
	// carries the half-pixel centre offset, so the truncation is what
	// names the pixel; the y origin differs between the GL and Vulkan
	// paths and the noise does not care, since it is keyed on the pixel
	// and not on a direction.
	vSample.xyz = jahDither8( vSample.xyz, ivec2( gl_FragCoord.xy ),
							  1.0 - clamp( jahDitherOff, 0.0, 1.0 ) );

	fragColour = vSample;
}
