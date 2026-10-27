#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform textureCube skyCubemap;
vulkan( layout( ogre_s0 ) uniform sampler samplerState );

// Jahshaka (PHOTON-7 10-3 R5): THE SKY's GAIN - the scene's sky exposure, applied where the picture is formed, so
// every reader of the drawn sky (the capture's cube, its irradiance) carries it: the sky emits what it shows.
vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 skyGain;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
    vec3 cameraDir;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

void main()
{
	//Cubemaps are left-handed
	fragColour = texture( vkSamplerCube( skyCubemap, samplerState ),
						  vec3( inPs.cameraDir.xy, -inPs.cameraDir.z ) );
	fragColour.xyz *= skyGain.xyz;
}
