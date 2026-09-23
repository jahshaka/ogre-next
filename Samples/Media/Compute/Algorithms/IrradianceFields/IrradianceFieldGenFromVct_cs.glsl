@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

#define ushort2 uvec2

@insertpiece( PreBindingsHeaderCS )

vulkan_layout( ogre_T0 ) uniform samplerBuffer directionsBuffer;

vulkan( layout( ogre_s1 ) uniform sampler probeSampler );

// Jahshaka (PHOTON-READER-1): EVERY cascade's volumes, the probe rays walk the
// whole chain. The same declaration as the bounce-injection job's: one array per
// volume kind, unit order iso, then X, Y, Z (IrradianceField::bindChainToGenerationJob).
// uses_array_bindings = hlms_num_vct_cascades - 1 (arrays of more than one binding)
@pset( vctTexUnit, 1 )
@psub( uses_array_bindings, hlms_num_vct_cascades, 1 )

vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbes[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )

@property( vct_anisotropic )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeX[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeY[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeZ[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
@end

layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
uniform restrict writeonly image2D irradianceField;

layout( vulkan( ogre_u1 ) vk_comma @insertpiece(uav1_pf_type) )
uniform restrict writeonly image2D irradianceFieldDepth;

shared float4 g_diffuseDepth[@value( threads_per_group_x ) * @value( threads_per_group_y ) * @value( threads_per_group_z )];
// Jahshaka (PHOTON-READER-1): each ray's escape fraction, reduced beside its depth.
shared float g_escape[@value( threads_per_group_x ) * @value( threads_per_group_y ) * @value( threads_per_group_z )];

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

@insertpiece( HeaderCS )

//in uvec3 gl_NumWorkGroups;
//in uvec3 gl_WorkGroupID;
//in uvec3 gl_LocalInvocationID;
//in uvec3 gl_GlobalInvocationID;
//in uint  gl_LocalInvocationIndex;

void main()
{
	@insertpiece( BodyCS )
}
