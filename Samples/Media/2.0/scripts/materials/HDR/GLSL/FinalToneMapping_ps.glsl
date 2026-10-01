#version ogre_glsl_ver_330

vulkan_layout( location = 0 )
out vec4 fragColour;

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

// JAHSHAKA (IMAGE-1): THE FILM CURVE IS UNREAL ENGINE'S FILMIC TONEMAPPER, the
// ACES-based curve Unreal Engine 4.15 shipped (Engine/Shaders/Private/
// TonemapCommon.ush, FilmToneMap; its RRT glow and red modifier and the AP0/AP1
// matrices from ACES.ush, after the Academy's ACES 1.0 Reference Rendering
// Transform). Five parameters with Unreal's defaults: Slope 0.88, Toe 0.55,
// Shoulder 0.26, Black clip 0, White clip 0.04. The curve is built so an input
// of 0.18 leaves at 0.18, so an 18 percent card at the exposure that puts it
// at 0.18 displays as code 118 after the sRGB encode below. It replaced Hable's
// curve with Ogre's sample constants and its contrast and lift tail, which
// clipped to white 3.68 stops over the grey card.
//
// The C++ reference is iris::lens::filmCurve (irisgl cameralens.cpp); the
// tonemap suite compares the two at nine exposures.

// The colour spaces, written as mat3 of ROWS so that v * M is M applied to v.
// sRGB (linear, D65) to ACEScg (AP1, D60) through the Bradford D65 to D60
// adaptation, and back; each row normalised so that white maps to white
// exactly (the published constants miss by 2e-4).
const mat3 jahSrgbToAp1 = mat3( 0.6131486203, 0.3394883591, 0.0473630206,
								0.0702074147, 0.9163424763, 0.0134501090,
								0.0206231422, 0.1095899890, 0.8697868689 );
const mat3 jahAp1ToSrgb = mat3( 1.7050473375, -0.6217891459, -0.0832581917,
								-0.1302575067, 1.1408060644, -0.0105485577,
								-0.0240032831, -0.1289688126, 1.1529720957 );
const mat3 jahAp1ToAp0 = mat3( 0.6954522414, 0.1406786965, 0.1638690622,
							   0.0447945634, 0.8596711185, 0.0955343182,
							   -0.0055258826, 0.0040252103, 1.0015006723 );
const mat3 jahAp0ToAp1 = mat3( 1.4514393161, -0.2365107469, -0.2149285693,
							   -0.0765537734, 1.1762296998, -0.0996759264,
							   0.0083161484, -0.0060324498, 0.9977163014 );
// AP1's luminance weights (the Y row of AP1 to XYZ).
const vec3 jahAp1Luma = vec3( 0.2722287168, 0.6740817658, 0.0536895174 );

float jahRgbToSaturation( vec3 rgb )
{
	float mi = min( min( rgb.r, rgb.g ), rgb.b );
	float ma = max( max( rgb.r, rgb.g ), rgb.b );
	return ( max( ma, 1e-10 ) - max( mi, 1e-10 ) ) / max( ma, 1e-2 );
}

float jahRgbToYc( vec3 rgb )
{
	// ycRadiusWeight 1.75, the RRT's
	float chroma = sqrt( rgb.b * ( rgb.b - rgb.g ) + rgb.g * ( rgb.g - rgb.r ) + rgb.r * ( rgb.r - rgb.b ) );
	return ( rgb.b + rgb.g + rgb.r + 1.75 * chroma ) / 3.0;
}

float jahSigmoidShaper( float x )
{
	float t = max( 1.0 - abs( 0.5 * x ), 0.0 );
	float y = 1.0 + sign( x ) * ( 1.0 - t * t );
	return 0.5 * y;
}

float jahGlowFwd( float ycIn, float glowGainIn, float glowMid )
{
	if( ycIn <= 2.0 / 3.0 * glowMid )
		return glowGainIn;
	if( ycIn >= 2.0 * glowMid )
		return 0.0;
	return glowGainIn * ( glowMid / ycIn - 0.5 );
}

float jahRgbToHue( vec3 rgb )
{
	if( rgb.r == rgb.g && rgb.g == rgb.b )
		return 0.0;
	float hue = 57.2957795131 * atan( 1.7320508076 * ( rgb.g - rgb.b ), 2.0 * rgb.r - rgb.g - rgb.b );
	return hue < 0.0 ? hue + 360.0 : hue;
}

// slope, toe, shoulder, black clip, white clip: the five film parameters.
vec3 jahFilmToneMap( vec3 colorAp1, float slope, float toe, float shoulder, float blackClip,
					 float whiteClip )
{
	vec3 colorAp0 = colorAp1 * jahAp1ToAp0;

	// The RRT's glow module
	float saturation = jahRgbToSaturation( colorAp0 );
	float ycIn = jahRgbToYc( colorAp0 );
	float s = jahSigmoidShaper( ( saturation - 0.4 ) / 0.2 );
	colorAp0 *= 1.0 + jahGlowFwd( ycIn, 0.05 * s, 0.08 );

	// The RRT's red modifier (scale 0.82, pivot 0.03, hue 0, width 135)
	float hue = jahRgbToHue( colorAp0 );
	float centeredHue = hue > 180.0 ? hue - 360.0 : hue;
	float hueWeight = smoothstep( 0.0, 1.0, 1.0 - abs( 2.0 * centeredHue / 135.0 ) );
	hueWeight *= hueWeight;
	colorAp0.r += hueWeight * saturation * ( 0.03 - colorAp0.r ) * ( 1.0 - 0.82 );

	// ACEScg primaries as the working space, pre-desaturated
	vec3 w = max( colorAp0 * jahAp0ToAp1, vec3( 0.0 ) );
	w = mix( vec3( dot( w, jahAp1Luma ) ), w, 0.96 );

	float toeScale = 1.0 + blackClip - toe;
	float shoulderScale = 1.0 + whiteClip - shoulder;
	const float inMatch = 0.18;
	const float outMatch = 0.18;
	float toeMatch;
	if( toe > 0.8 )
	{
		// 0.18 is on the straight segment
		toeMatch = ( 1.0 - toe - outMatch ) / slope + log( inMatch ) * 0.4342944819;
	}
	else
	{
		// 0.18 is on the toe: the toe passes through ( inMatch, outMatch )
		float bt = ( outMatch + blackClip ) / toeScale - 1.0;
		toeMatch = log( inMatch ) * 0.4342944819 -
				   0.5 * log( ( 1.0 + bt ) / ( 1.0 - bt ) ) * ( toeScale / slope );
	}
	float straightMatch = ( 1.0 - toe ) / slope - toeMatch;
	float shoulderMatch = shoulder / slope - straightMatch;

	vec3 logColor = log( max( w, vec3( 1e-10 ) ) ) * 0.4342944819;	// log10
	vec3 straightColor = slope * ( logColor + straightMatch );
	vec3 toeColor = -blackClip + ( 2.0 * toeScale ) /
					( 1.0 + exp( ( -2.0 * slope / toeScale ) * ( logColor - toeMatch ) ) );
	vec3 shoulderColor = ( 1.0 + whiteClip ) - ( 2.0 * shoulderScale ) /
						 ( 1.0 + exp( ( 2.0 * slope / shoulderScale ) * ( logColor - shoulderMatch ) ) );
	toeColor = mix( straightColor, toeColor, lessThan( logColor, vec3( toeMatch ) ) );
	shoulderColor = mix( straightColor, shoulderColor, greaterThan( logColor, vec3( shoulderMatch ) ) );

	vec3 t = clamp( ( logColor - toeMatch ) / ( shoulderMatch - toeMatch ), 0.0, 1.0 );
	t = shoulderMatch < toeMatch ? 1.0 - t : t;
	t = ( 3.0 - 2.0 * t ) * t * t;
	vec3 toneColor = mix( toeColor, shoulderColor, t );

	// post-desaturate
	toneColor = mix( vec3( dot( toneColor, jahAp1Luma ) ), toneColor, 0.93 );
	return max( toneColor, vec3( 0.0 ) );
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

// JAHSHAKA (fork feab041c6 (was 0079), lane DITHER-1): the dither that makes this quad's
// 8-bit write honest. This is THE place a floating-point picture becomes
// display codes in this engine -- every target this material ever writes is
// 8-bit UNORM (the window, the offscreen render target, the VR eye image, the
// LDR buffer SMAA works on, the looks stage's first buffer, the
// picture-in-picture inset) -- so the dither is unconditional here and needed
// nowhere else. Why, how big and why it is keyed on the pixel alone: the
// header, which is Jahshaka media and is found through the resource group.
#include "JahDither.glsl"
// JAHSHAKA (SRGB-ENCODE-1): the display encode. The film curve and its grade
// tail below produce LINEAR values; the targets are plain UNORM (the header
// says why and where else the encode runs), so the exact sRGB OETF is applied
// here, once, BEFORE the dither -- the dither then rounds display codes, which
// is the quantiser it was designed for.
#include "JahSrgb.glsl"
vulkan( layout( ogre_P0 ) uniform Params { )
	// THE SAFE DEFAULT IS ZERO, i.e. DITHERED: a zero-filled constant buffer
	// (a frame drawn before the host has pushed anything, and any target
	// this material reaches through a path that does not push) renders the
	// CORRECT picture rather than a banded one, and a garbage value is
	// clamped to "off", i.e. to the picture this engine drew before the
	// dither existed. Neither failure can produce noise.
	uniform float jahDitherOff;	// 0 = dither (normal), 1 = JAHSHAKA_NO_DITHER
	// JAHSHAKA (fork feab041c6 (was 0082), lane BLOOM-AMOUNT-1): HOW MUCH of the blurred
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
	// JAHSHAKA (IMAGE-1): THE IMAGE BLOCK, the world camera's settings (a
	// camera may override each). The same safety rule as the two above: every
	// field is written as its OFFSET FROM THE DEFAULT, so the zero-filled
	// buffer of a frame nobody has pushed renders the default picture.
	//   jahImage0 = contrast - 1, saturation - 1, shadows (stops), highlights (stops)
	//   jahImage1.x = vignette (0 none, 1 the full cos^4 falloff)
	//   jahFilm0 = slope - 0.88, toe - 0.55, shoulder - 0.26, black clip
	//   jahFilm1.x = white clip - 0.04
	//   jahWhite0..2 = the white balance's 3x3 (rows, linear sRGB) minus identity
	uniform vec4 jahImage0;
	uniform vec4 jahImage1;
	uniform vec4 jahFilm0;
	uniform vec4 jahFilm1;
	uniform vec4 jahWhite0;
	uniform vec4 jahWhite1;
	uniform vec4 jahWhite2;
vulkan( }; )

void main()
{
	float fInvLumAvg = texture( vkSampler2D( lumRt, samplerPoint ), vec2( 0.0, 0.0 ) ).x;

	vec4 vSample = texture( vkSampler2D( rt0, samplerPoint ), inPs.uv0 );

	vSample.xyz *= fInvLumAvg;
	vSample.xyz	+= fromSRGB( texture( vkSampler2D( bloomRt, samplerBilinear ),
									  inPs.uv0 ).xyz ) *
				   ( 16.0 * clamp( 1.0 + jahBloomAmountMinusOne, 0.0, 2.0 ) );

	// THE IMAGE BLOCK (IMAGE-1), in the order Unreal's post chain runs it.
	vec3 c = vSample.xyz;
	// 1. VIGNETTE: the natural cos^4 falloff of a lens, on the scene's light,
	//    with the frame's corner at 45 degrees off axis.
	{
		vec2 v = ( inPs.uv0 - vec2( 0.5 ) ) * 1.41421356;
		float tan2 = dot( v, v );
		float cos4 = 1.0 / ( ( 1.0 + tan2 ) * ( 1.0 + tan2 ) );
		c *= mix( 1.0, cos4, clamp( jahImage1.x, 0.0, 1.0 ) );
	}
	// 2. WHITE BALANCE: a chromatic adaptation in linear sRGB (the engine
	//    builds the matrix from the temperature and the tint).
	c = max( vec3( dot( c, vec3( 1.0, 0.0, 0.0 ) + jahWhite0.xyz ),
				   dot( c, vec3( 0.0, 1.0, 0.0 ) + jahWhite1.xyz ),
				   dot( c, vec3( 0.0, 0.0, 1.0 ) + jahWhite2.xyz ) ), vec3( 0.0 ) );
	// 3. INTO ACEScg, then the colour correction: saturation about the pixel's
	//    luminance, contrast as a power about the grey card (0.18), and the
	//    shadows and highlights as gains in stops on Unreal's two luminance
	//    masks (shadows below 0.09, highlights from 0.5 to 1).
	vec3 w = c * jahSrgbToAp1;
	{
		float luma = dot( w, jahAp1Luma );
		w = max( mix( vec3( luma ), w, max( 1.0 + jahImage0.y, 0.0 ) ), vec3( 0.0 ) );
		w = pow( w * ( 1.0 / 0.18 ), vec3( max( 1.0 + jahImage0.x, 0.0 ) ) ) * 0.18;
		float shadowWeight = 1.0 - smoothstep( 0.0, 0.09, luma );
		float highlightWeight = smoothstep( 0.5, 1.0, luma );
		w *= exp2( jahImage0.z * shadowWeight + jahImage0.w * highlightWeight );
	}
	// 4. THE FILM CURVE, and back to linear sRGB.
	w = jahFilmToneMap( w, max( 0.88 + jahFilm0.x, 0.01 ), 0.55 + jahFilm0.y, 0.26 + jahFilm0.z,
						jahFilm0.w, 0.04 + jahFilm1.x );
	vSample.xyz = max( w * jahAp1ToSrgb, vec3( 0.0 ) );

	// THE DISPLAY ENCODE (SRGB-ENCODE-1): linear film output to display codes.
	// An 18 percent card leaves the film curve at 0.18 and reaches code 118.
	vSample.xyz = jahSrgbEncode( vSample.xyz );

	// The INTEGER pixel, which is the dither's only key. gl_FragCoord.xy
	// carries the half-pixel centre offset, so the truncation is what
	// names the pixel; the y origin differs between the GL and Vulkan
	// paths and the noise does not care, since it is keyed on the pixel
	// and not on a direction.
	vSample.xyz = jahDither8( vSample.xyz, ivec2( gl_FragCoord.xy ),
							  1.0 - clamp( jahDitherOff, 0.0, 1.0 ) );

	fragColour = vSample;
}
