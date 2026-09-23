/*
-----------------------------------------------------------------------------
This source file is part of OGRE-Next
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-present Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/

#include "OgreStableHeaders.h"

#include "Vct/OgreVctLighting.h"

#include "Vct/OgreVctVoxelizerSourceBase.h"
#include "Vct/OgreVoxelVisualizer.h"

#include "OgreHlmsCompute.h"
#include "OgreHlmsComputeJob.h"
#include "OgreHlmsManager.h"
#include "OgreLight.h"
#include "OgreLwString.h"
#include "OgrePixelFormatGpuUtils.h"
#include "OgreRenderSystem.h"
#include "OgreSceneManager.h"
#include "OgreShaderPrimitives.h"
#include "OgreStringConverter.h"
#include "OgreTextureGpuManager.h"
#include "Vao/OgreConstBufferPacked.h"
#include "Vao/OgreVaoManager.h"

namespace Ogre
{
    struct ShaderVctLight
    {
        // Pre-mul by PI? -No because we lose a ton of precision
        //.w contains lightDistThreshold
        float diffuse[4];
        // For directional lights, pos.xyz contains -dir.xyz and pos.w = 0;
        // For the rest of lights, pos.xyz contains pos.xyz and pos.w = 1;
        float pos[4];
        // uvwPos.w contains the light type
        float uvwPos[4];

        // Used by area lights
        // points[0].w contains double sided info
        float points[4][4];
    };

    const uint16 VctLighting::msDistanceThresholdCustomParam = 3876u;

    static const IdString NumVctCascadesProp = "hlms_num_vct_cascades";

    static const size_t c_maxCascades = 8u;

    // JAHSHAKA PATCH 0080: THE FORMAT OF THE TOTAL VOLUME.
    //
    // The volume holds the fixed point of L = D + rho * G( L ). The DIRECT term D
    // is normalised to <= 1 by VctLighting::update's auto multiplier; the FIXED
    // POINT is not bounded by 1 at all -- it is D * sum( (rho*f)^i ), i.e. up to
    // D / (1 - rho*f), which for a white enclosure has no useful bound (measured:
    // 2.05x the direct term at albedo 0.79, 2.76x and climbing at albedo 1.0, 3.5x
    // in a shipped sample). An 8-bit UNORM store clips it, per channel, which
    // desaturates as it darkens and reads downstream exactly like a scene with
    // less bounce in it; and every fixed headroom that clears one room costs the
    // dark end, where the GI signal lives. So the total is a float. The direct
    // volume (patch 0076) keeps its 8-bit sRGB store: D <= 1 by construction.
    static PixelFormatGpu jahLightVoxelFormat() { return PFG_RGBA16_FLOAT; }
    static PixelFormatGpu jahLightVoxelUavFormat() { return PFG_RGBA16_FLOAT; }
    //-------------------------------------------------------------------------
    VctLighting::VctLighting( IdType id, VctVoxelizerSourceBase *voxelizer, bool bAnisotropic ) :
        IdObject( id ),
        mSamplerblockTrilinear( 0 ),
        mVoxelizer( voxelizer ),
        mVoxelizerTexturesChanged( false ),
        mVoxelizerListenersRemoved( false ),
        mLightInjectionJob( 0 ),
        mLightsConstBuffer( 0 ),
        mAnisoGeneratorStep0( 0 ),
        mLightVctBounceInject( 0 ),
        mLightBounce( 0 ),
        mLightDirect( 0 ),  // JAHSHAKA PATCH 0076
        mBakingMultiplier( 1.0f ),
        mInvBakingMultiplier( 1.0f ),
        mDefaultLightDistThreshold( 0.5f ),
        mAnisotropic( bAnisotropic ),
        mNumLights( 0 ),
        mRayMarchStepSize( 0 ),
        mVoxelCellSize( 0 ),
        mDirCorrectionRatioThinWallCounter( 0 ),
        mInvVoxelResolution( 0 ),
        mShaderParams( 0 ),
        mBounceVoxelCellSize( 0 ),
        mBounceInvVoxelResolution( 0 ),
        mBounceIterationDampening( 0 ),
        mBounceInvResMaxLod( 0 ),
        mBounceFromPreviousProbeToNext( 0 ),
        mBounceShaderParams( 0 ),
        mSpecularSdfQuality( 0.875f ),
        mMultiplier( 1.0f ),
        mDebugVoxelVisualizer( 0 )
    {
        memset( mLightVoxel, 0, sizeof( mLightVoxel ) );
        memset( mUpperHemisphere, 0, sizeof( mUpperHemisphere ) );
        memset( mLowerHemisphere, 0, sizeof( mLowerHemisphere ) );

        OGRE_ASSERT_LOW( mVoxelizer->getAlbedoVox() &&
                         "VctVoxelizer::build must've been called before creating VctLighting!" );

        mVoxelizer->getAlbedoVox()->addListener( this );
        mVoxelizer->getNormalVox()->addListener( this );
        mVoxelizerListenersRemoved = false;

        // VctVoxelizer should've already been initialized, thus no need
        // to check if JSON has been built or if the assets were added
        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
        mLightInjectionJob = hlmsCompute->findComputeJob( "VCT/LightInjection" );

        mShaderParams = &mLightInjectionJob->getShaderParams( "default" );
        mNumLights = mShaderParams->findParameter( "numLights" );
        mRayMarchStepSize = mShaderParams->findParameter( "rayMarchStepSize_bakingMultiplier" );
        mVoxelCellSize = mShaderParams->findParameter( "voxelCellSize" );
        mDirCorrectionRatioThinWallCounter =
            mShaderParams->findParameter( "dirCorrectionRatio_thinWallCounter" );
        mInvVoxelResolution = mShaderParams->findParameter( "invVoxelResolution" );

        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();
        VaoManager *vaoManager = renderSystem->getVaoManager();
        mLightsConstBuffer = vaoManager->createConstBuffer( sizeof( ShaderVctLight ) * 16u,
                                                            BT_DYNAMIC_PERSISTENT, 0, false );

        HlmsManager *hlmsManager = mVoxelizer->getHlmsManager();
        HlmsSamplerblock samplerblock;
        samplerblock.mMipFilter = FO_LINEAR;
        mSamplerblockTrilinear = hlmsManager->getSamplerblock( samplerblock );

        mLightVctBounceInject = hlmsCompute->findComputeJob( "VCT/LightVctBounceInject" );

        mBounceShaderParams = &mLightVctBounceInject->getShaderParams( "default" );

        mLocalBounceShaderParams.reserve( 7u );

        mBounceVoxelCellSize = addLocalBounceShaderParam( "voxelCellSize" );
        mBounceInvVoxelResolution = addLocalBounceShaderParam( "invVoxelResolution" );
        mBounceIterationDampening = addLocalBounceShaderParam( "iterationDampening" );
        mBounceInvResMaxLod = addLocalBounceShaderParam( "vctInvResMaxLod" );
        mBounceFromPreviousProbeToNext = addLocalBounceShaderParam( "fromPreviousProbeToNext" );

        createTextures();
    }
    //-------------------------------------------------------------------------
    VctLighting::~VctLighting()
    {
        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();
        VaoManager *vaoManager = renderSystem->getVaoManager();

        setDebugVisualization( false, 0 );

        if( mLightsConstBuffer )
        {
            if( mLightsConstBuffer->getMappingState() != MS_UNMAPPED )
                mLightsConstBuffer->unmap( UO_UNMAP_ALL );
            vaoManager->destroyConstBuffer( mLightsConstBuffer );
            mLightsConstBuffer = 0;
        }

        HlmsManager *hlmsManager = mVoxelizer->getHlmsManager();
        hlmsManager->destroySamplerblock( mSamplerblockTrilinear );
        mSamplerblockTrilinear = 0;

        if( !mVoxelizerListenersRemoved )
        {
            mVoxelizer->getAlbedoVox()->removeListener( this );
            mVoxelizer->getNormalVox()->removeListener( this );
            mVoxelizerListenersRemoved = true;
        }

        destroyTextures();
    }
    //-------------------------------------------------------------------------
    ShaderParams::Param *VctLighting::addLocalBounceShaderParam( const char *name )
    {
        mLocalBounceShaderParams.push_back( ShaderParams::Param() );
        ShaderParams::Param *retVal = &mLocalBounceShaderParams.back();
        retVal->name = name;
        return retVal;
    }
    //-------------------------------------------------------------------------
    void VctLighting::restoreSwappedTextures()
    {
        if( mLightVoxel[0] && mLightBounce )
        {
            char tmpBuffer[128];
            LwString texName( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );
            texName.a( "VctLightingBounce/Id", getId() );

            if( mLightBounce->getName() != texName.c_str() )
            {
                std::swap( mLightVoxel[0], mLightBounce );

                DescriptorSetTexture2::TextureSlot texSlot(
                    DescriptorSetTexture2::TextureSlot::makeEmpty() );
                texSlot.texture = mLightVoxel[0];
                mLightVctBounceInject->setTexture( 2, texSlot, mSamplerblockTrilinear );

                if( mAnisoGeneratorStep0 )
                {
                    texSlot.texture = mLightVoxel[0];
                    mAnisoGeneratorStep0->setTexture( 0, texSlot );
                }
            }
        }
    }
    //-------------------------------------------------------------------------
    float VctLighting::addLight( ShaderVctLight *RESTRICT_ALIAS vctLight, Light *light,
                                 const Vector3 &voxelOrigin, const Vector3 &invVoxelSize )
    {
        const ColourValue diffuseColour = light->getDiffuseColour() * light->getPowerScale();
        for( size_t i = 0; i < 3u; ++i )
            vctLight->diffuse[i] = static_cast<float>( diffuseColour[i] );

        const Vector4 *lightDistThreshold =
            light->getCustomParameterNoThrow( msDistanceThresholdCustomParam );
        vctLight->diffuse[3] = lightDistThreshold
                                   ? ( lightDistThreshold->x * lightDistThreshold->x )
                                   : ( mDefaultLightDistThreshold * mDefaultLightDistThreshold );

        Light::LightTypes lightType = light->getType();
        if( lightType == Light::LT_AREA_APPROX )
            lightType = Light::LT_AREA_LTC;

        Vector4 light4dVec = light->getAs4DVector();
        if( lightType != Light::LT_DIRECTIONAL )
            light4dVec -= Vector4( voxelOrigin, 0.0f );

        for( size_t i = 0; i < 4u; ++i )
            vctLight->pos[i] = static_cast<float>( light4dVec[i] );

        Vector3 uvwPos = light->getParentNode()->_getDerivedPosition();
        uvwPos = ( uvwPos - voxelOrigin ) * invVoxelSize;
        for( size_t i = 0; i < 3u; ++i )
            vctLight->uvwPos[i] = static_cast<float>( uvwPos[i] );
        vctLight->uvwPos[3] = static_cast<float>( lightType );

        Vector3 rectPoints[4];

        const Quaternion qRot = light->getParentNode()->_getDerivedOrientation();
        const Vector3 lightDir = qRot.zAxis();

        if( lightType == Light::LT_AREA_LTC )
        {
            const Vector3 lightPos( light4dVec.xyz() );
            const Vector2 rectSize = light->getDerivedRectSize() * 0.5f;
            Vector3 xAxis = qRot.xAxis() * rectSize.x;
            Vector3 yAxis = qRot.yAxis() * rectSize.y;

            rectPoints[0] = lightPos - xAxis - yAxis;
            rectPoints[1] = lightPos + xAxis - yAxis;
            rectPoints[2] = lightPos + xAxis + yAxis;
            rectPoints[3] = lightPos - xAxis + yAxis;
        }
        else
        {
            memset( rectPoints, 0, sizeof( rectPoints ) );

            if( lightType == Light::LT_SPOTLIGHT )
            {
                // float3 spotDirection
                rectPoints[0].x = -lightDir.x;
                rectPoints[0].y = -lightDir.y;
                rectPoints[0].z = -lightDir.z;

                // float3 spotParams
                const Radian innerAngle = light->getSpotlightInnerAngle();
                const Radian outerAngle = light->getSpotlightOuterAngle();
                rectPoints[1].x = 1.0f / ( cosf( innerAngle.valueRadians() * 0.5f ) -
                                           cosf( outerAngle.valueRadians() * 0.5f ) );
                rectPoints[1].y = cosf( outerAngle.valueRadians() * 0.5f );
                rectPoints[1].z = light->getSpotlightFalloff();
            }
        }

        const float isDoubleSided = light->getDoubleSided() ? 1.0f : 0.0f;
        for( size_t i = 0; i < 4u; ++i )
        {
            for( size_t j = 0; j < 3u; ++j )
                vctLight->points[i][j] = rectPoints[i][j];
            vctLight->points[i][3u] = isDoubleSided;
        }

        vctLight->points[1][3u] = static_cast<float>( lightDir.x );
        vctLight->points[2][3u] = static_cast<float>( lightDir.y );
        vctLight->points[3][3u] = static_cast<float>( lightDir.z );

        float maxValue;
        maxValue = std::max( diffuseColour.r, diffuseColour.g );
        maxValue = std::max( maxValue, diffuseColour.b );
        return maxValue;
    }
    //-------------------------------------------------------------------------
    void VctLighting::createTextures()
    {
        const bool allowsMultipleBounces = getAllowMultipleBounces();
        destroyTextures();

        TextureGpuManager *textureManager = mVoxelizer->getTextureGpuManager();

        // JAHSHAKA PATCH 0080: NOT Reinterpretable. The flag existed for the 8-bit
        // store, whose UAV view (RGBA8_UNORM) reinterpreted the sRGB texture; a float
        // store's UAV view IS its format. And the flag is not free: a reinterpretable
        // texture is created as its format FAMILY -- R16G16B16A16_UINT for 16F --
        // and the mip chain's linear blit (_autogenerateMipmaps) is then invalid on
        // an integer image (VUID-vkCmdBlitImage-filter-02001: the format has no
        // linear-filter feature), so the coarse mips the cone gather reads would be
        // undefined. The direct volume below keeps the flag with its 8-bit store.
        uint32 texFlags = TextureFlags::Uav;

        const bool bSdfQuality = shouldEnableSpecularSdfQuality();
        if( !mAnisotropic || bSdfQuality )
            texFlags |= TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps;

        const TextureGpu *albedoVox = mVoxelizer->getAlbedoVox();

        const uint32 width = albedoVox->getWidth();
        const uint32 height = albedoVox->getHeight();
        const uint32 depth = albedoVox->getDepth();

        const uint32 widthAniso = std::max( 1u, width );
        const uint32 heightAniso = std::max( 1u, height >> 1u );
        const uint32 depthAniso = std::max( 1u, depth >> 1u );

        const uint8 numMipsMain = ( mAnisotropic && !bSdfQuality )
                                      ? 1u
                                      : PixelFormatGpuUtils::getMaxMipmapCount( width, height, depth );
        // numMipsAniso needs one less mip; because the last mip must be 2x1x1, not 1x1x1
        const uint8 numMipsAniso =
            PixelFormatGpuUtils::getMaxMipmapCount( widthAniso, heightAniso, depthAniso ) - 1u;

        const size_t numTextures = mAnisotropic ? 4u : 1u;

        const char *names[] = {
            "Main",    //
            "X_axis",  //
            "Y_axis",  //
            "Z_axis"   //
        };

        for( size_t i = 0; i < numTextures; ++i )
        {
            char tmpBuffer[128];
            LwString texName( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );
            texName.a( "VctLighting_", names[i], "/Id", getId() );
            TextureGpu *texture = textureManager->createTexture(
                texName.c_str(), GpuPageOutStrategy::Discard, texFlags, TextureTypes::Type3D );
            if( i == 0u )
            {
                texture->setResolution( width, height, depth );
                texture->setNumMipmaps( numMipsMain );
            }
            else
            {
                texture->setResolution( widthAniso, heightAniso, depthAniso );
                texture->setNumMipmaps( numMipsAniso );
            }
            texture->setPixelFormat( jahLightVoxelFormat() );  // JAHSHAKA PATCH 0080
            texture->scheduleTransitionTo( GpuResidency::Resident );
            mLightVoxel[i] = texture;

            texFlags &= ( uint32 ) ~( TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps );
        }

        if( mAnisotropic )
        {
            // Setup the compute shaders for VctLighting::generateAnisotropicMips()
            HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
            mAnisoGeneratorStep0 = hlmsCompute->findComputeJob( "VCT/AnisotropicMipStep0" );

            char tmpBuffer[128];
            LwString jobName( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );

            // Step 0
            jobName.clear();
            jobName.a( "VCT/AnisotropicMipStep0/Id", getId() );
            mAnisoGeneratorStep0 = mAnisoGeneratorStep0->clone( jobName.c_str() );

            for( uint8 i = 0; i < 3u; ++i )
            {
                DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
                uavSlot.access = ResourceAccess::Write;
                uavSlot.texture = mLightVoxel[i + 1u];
                uavSlot.pixelFormat = jahLightVoxelUavFormat();  // JAHSHAKA PATCH 0080
                mAnisoGeneratorStep0->_setUavTexture( i, uavSlot );
            }

            DescriptorSetTexture2::TextureSlot texSlot(
                DescriptorSetTexture2::TextureSlot::makeEmpty() );
            texSlot.texture = mLightVoxel[0];
            mAnisoGeneratorStep0->setTexture( 0, texSlot );
            texSlot.texture = mVoxelizer->getNormalVox();
            mAnisoGeneratorStep0->setTexture( 1, texSlot );

            ShaderParams *shaderParams = &mAnisoGeneratorStep0->getShaderParams( "default" );
            // higherMipHalfWidth
            ShaderParams::Param *lowerMipResolutionParam = &shaderParams->mParams.back();
            // int32 resolution[4] = { static_cast<int32>( mLightVoxel[1]->getWidth() >> 1u ) };
            lowerMipResolutionParam->setManualValue(
                static_cast<int32>( mLightVoxel[1]->getWidth() >> 1u ) );
            shaderParams->setDirty();

            // Now setup step 1
            // numMipsOnStep1 is subtracted one because mip 0 got processed by step 0
            const uint8 numMipsOnStep1 = mLightVoxel[1]->getNumMipmaps() - 1u;
            mAnisoGeneratorStep1.resize( numMipsOnStep1 );

            HlmsComputeJob *baseJob = hlmsCompute->findComputeJob( "VCT/AnisotropicMipStep1" );

            for( uint8 i = 0; i < numMipsOnStep1; ++i )
            {
                jobName.clear();
                jobName.a( "VCT/AnisotropicMipStep1/Id", getId(), "/Mip", i + 1u );
                HlmsComputeJob *mipJob = baseJob->clone( jobName.c_str() );

                for( uint8 axis = 0; axis < 3u; ++axis )
                {
                    texSlot.texture = mLightVoxel[axis + 1u];
                    texSlot.mipmapLevel = i;
                    texSlot.numMipmaps = 1u;
                    mipJob->setTexture( axis, texSlot );

                    DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
                    uavSlot.access = ResourceAccess::Write;
                    uavSlot.texture = mLightVoxel[axis + 1u];
                    uavSlot.mipmapLevel = i + 1u;
                    uavSlot.pixelFormat = jahLightVoxelUavFormat();  // JAHSHAKA PATCH 0080
                    mipJob->_setUavTexture( axis, uavSlot );
                }

                shaderParams = &mipJob->getShaderParams( "default" );
                // higherMipHalfRes_lowerMipHalfWidth
                lowerMipResolutionParam = &shaderParams->mParams.back();
                int32 resolutions[4] = { //
                                         static_cast<int32>( mLightVoxel[1]->getWidth() >> ( i + 2u ) ),
                                         static_cast<int32>( mLightVoxel[1]->getHeight() >> ( i + 1u ) ),
                                         static_cast<int32>( mLightVoxel[1]->getDepth() >> ( i + 1u ) ),
                                         static_cast<int32>( mLightVoxel[1]->getWidth() >> ( i + 1u ) )
                };
                for( size_t j = 0; j < 4u; ++j )
                    resolutions[j] = std::max( 1, resolutions[j] );
                lowerMipResolutionParam->setManualValue( resolutions, 4u );
                shaderParams->setDirty();

                mAnisoGeneratorStep1[i] = mipJob;
            }
        }

        mLightInjectionJob->setProperty( "correct_area_light_shadows",
                                         albedoVox->getNumMipmaps() > 1u ? 1 : 0 );

        setAllowMultipleBounces( allowsMultipleBounces );

        if( mDebugVoxelVisualizer )
        {
            mDebugVoxelVisualizer->setTrackingVoxel( mLightVoxel[0], mLightVoxel[0], true );
            mDebugVoxelVisualizer->setVisible( true );
        }
    }
    //-------------------------------------------------------------------------
    void VctLighting::destroyTextures()
    {
        restoreSwappedTextures();

        TextureGpuManager *textureManager = mVoxelizer->getTextureGpuManager();
        for( size_t i = 0; i < sizeof( mLightVoxel ) / sizeof( mLightVoxel[0] ); ++i )
        {
            if( mLightVoxel[i] )
            {
                textureManager->destroyTexture( mLightVoxel[i] );
                mLightVoxel[i] = 0;
            }
        }

        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();

        if( mAnisoGeneratorStep0 )
        {
            hlmsCompute->destroyComputeJob( mAnisoGeneratorStep0->getName() );
            mAnisoGeneratorStep0 = 0;
        }

        FastArray<HlmsComputeJob *>::const_iterator itor = mAnisoGeneratorStep1.begin();
        FastArray<HlmsComputeJob *>::const_iterator end = mAnisoGeneratorStep1.end();

        while( itor != end )
        {
            hlmsCompute->destroyComputeJob( ( *itor )->getName() );
            ++itor;
        }

        mAnisoGeneratorStep1.clear();
        setAllowMultipleBounces( false );

        if( mDebugVoxelVisualizer )
            mDebugVoxelVisualizer->setVisible( false );
    }
    //-------------------------------------------------------------------------
    void VctLighting::checkTextures()
    {
        if( mVoxelizerTexturesChanged )
        {
            createTextures();
            // The textures have been re-created; the request has been served. Without
            // this the flag stays raised for the object's whole life (it is only ever
            // assigned false in the constructor), so a voxelizer that loses residency
            // ONCE makes every subsequent update() destroy and re-create the light voxel
            // textures, for ever.
            mVoxelizerTexturesChanged = false;
        }

        if( mVoxelizerListenersRemoved )
        {
            mVoxelizer->getAlbedoVox()->addListener( this );
            mVoxelizer->getNormalVox()->addListener( this );
            mVoxelizerListenersRemoved = false;
        }
    }
    //-------------------------------------------------------------------------
    void VctLighting::setVoxelizer( VctVoxelizerSourceBase *voxelizer )
    {
        if( !voxelizer || voxelizer == mVoxelizer )
            return;

        if( !mVoxelizerListenersRemoved )
        {
            mVoxelizer->getAlbedoVox()->removeListener( this );
            mVoxelizer->getNormalVox()->removeListener( this );
        }

        mVoxelizer = voxelizer;
        // The same two requests a LostResidency notification raises, served immediately:
        // re-create the light voxels from the new voxelizer's resolution and re-register
        // as a listener on its albedo/normal textures.
        mVoxelizerTexturesChanged = true;
        mVoxelizerListenersRemoved = true;
        checkTextures();
    }
    //-------------------------------------------------------------------------
    void VctLighting::setupBounceTextures( bool bSetSamplerRefs )
    {
        const size_t numExtraCascades = mExtraCascades.size();

        // JAHSHAKA PATCH 0076: +1 for `directVoxel`, the fixed point's D term. It is
        // bound LAST so that every existing slot index -- albedo, normal, this
        // cascade's probes, the extra cascades', the three anisotropic sets -- keeps
        // the number it had, in the C++ and in the shader's ogre_tN layout alike.
        uint8 numNeededTexUnits;
        if( mAnisotropic )
            numNeededTexUnits = 6u + 4u * static_cast<uint8>( numExtraCascades ) + 1u;
        else
            numNeededTexUnits = 3u + static_cast<uint8>( numExtraCascades ) + 1u;

        HlmsManager *hlmsManager = mVoxelizer->getHlmsManager();
        const RenderSystemCapabilities *caps = hlmsManager->getRenderSystem()->getCapabilities();
        const bool bSetSampler = !caps->hasCapability( RSC_SEPARATE_SAMPLERS_FROM_TEXTURES );
        // The samplerblocks are bound ONCE and never move; the textures move on every
        // bounce. So a re-assert (runBounce, below) writes the textures and leaves the
        // sampler reference counting alone, which is the only reason this call is cheap
        // enough to repeat per dispatch.
        const bool bSetSamplerNow = bSetSampler && bSetSamplerRefs;

        if( mLightVctBounceInject->getNumTexUnits() != numNeededTexUnits )
        {
            mLightVctBounceInject->setNumTexUnits( numNeededTexUnits );
            if( !bSetSampler )
                mLightVctBounceInject->setNumSamplerUnits( 3u );
        }

        setupGlslTextureUnits();

        DescriptorSetTexture2::TextureSlot texSlot( DescriptorSetTexture2::TextureSlot::makeEmpty() );
        texSlot.texture = mVoxelizer->getAlbedoVox();
        mLightVctBounceInject->setTexture( 0, texSlot );
        texSlot.texture = mVoxelizer->getNormalVox();
        mLightVctBounceInject->setTexture( 1, texSlot );
        texSlot.texture = mLightVoxel[0];
        mLightVctBounceInject->setTexture( 2, texSlot, mSamplerblockTrilinear );

        uint8 texSlotIdx = 3u;

        for( size_t cascadeIdx = 0u; cascadeIdx < numExtraCascades; ++cascadeIdx )
        {
            texSlot.texture = mExtraCascades[cascadeIdx]->mLightVoxel[0];
            mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
            if( bSetSamplerNow )
            {
                // Only OpenGL needs this sampler set
                hlmsManager->addReference( mSamplerblockTrilinear );
                mLightVctBounceInject->_setSamplerblock( texSlotIdx - 1u, mSamplerblockTrilinear );
            }
        }

        if( mAnisotropic )
        {
            for( uint8 i = 0u; i < 3u; ++i )
            {
                texSlot.texture = mLightVoxel[i + 1u];
                mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
                if( bSetSamplerNow )
                {
                    // Only OpenGL needs this sampler set
                    hlmsManager->addReference( mSamplerblockTrilinear );
                    mLightVctBounceInject->_setSamplerblock( texSlotIdx - 1u, mSamplerblockTrilinear );
                }

                for( size_t cascadeIdx = 0u; cascadeIdx < numExtraCascades; ++cascadeIdx )
                {
                    texSlot.texture = mExtraCascades[cascadeIdx]->mLightVoxel[i + 1u];
                    mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
                    if( bSetSamplerNow )
                    {
                        // Only OpenGL needs this sampler set
                        hlmsManager->addReference( mSamplerblockTrilinear );
                        mLightVctBounceInject->_setSamplerblock( texSlotIdx - 1u,
                                                                 mSamplerblockTrilinear );
                    }
                }
            }
        }

        // JAHSHAKA PATCH 0076: THE DIRECT TERM, at the last unit. Read with a plain
        // Load3D at the voxel being written, so it needs no sampler of its own (the
        // OpenGL path's samplerblock loop above deliberately skips it).
        texSlot.texture = mLightDirect;
        mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );

        DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
        uavSlot.access = ResourceAccess::Write;
        uavSlot.texture = mLightBounce;
        uavSlot.pixelFormat = jahLightVoxelUavFormat();  // JAHSHAKA PATCH 0080
        mLightVctBounceInject->_setUavTexture( 0, uavSlot );
    }
    //-------------------------------------------------------------------------
    void VctLighting::setupGlslTextureUnits()
    {
        const size_t numExtraCascades = mExtraCascades.size();
        size_t numNeededTexUnits;  // +1: `directVoxel` (JAHSHAKA PATCH 0076)
        if( mAnisotropic )
            numNeededTexUnits = 6u + 4u * numExtraCascades + 1u;
        else
            numNeededTexUnits = 3u + numExtraCascades + 1u;

        // This code assumes there's 2 textures at the beginning that always stays the same
        // the rest of them are dynamically generated.
        //
        // We also need to check if another VctLighting instance set a different number of cascades
        ShaderParams &glslShaderParams = mLightVctBounceInject->getShaderParams( "glsl" );
        if( glslShaderParams.mParams.size() != numNeededTexUnits ||
            glslShaderParams.mParams[3].mp.dataSizeBytes !=
                ( numExtraCascades + 1u ) * sizeof( uint32 ) )
        {
            glslShaderParams.mParams.resize( 2u );

            ShaderParams::Param param;
            int32 texSlotIdx = 2u;

            const char *names[4] = { "vctProbes", "vctProbeX", "vctProbeY", "vctProbeZ" };

            const uint32 numTextureVariables = mAnisotropic ? 4u : 1u;

            for( size_t i = 0u; i < numTextureVariables; ++i )
            {
                param.name = names[i];
                int32 textureUnitsTmp[16];
                for( size_t cascadeIdx = 0u; cascadeIdx < numExtraCascades + 1u; ++cascadeIdx )
                    textureUnitsTmp[cascadeIdx] = texSlotIdx++;
                param.setManualValue( textureUnitsTmp, static_cast<uint32>( numExtraCascades + 1u ) );
                glslShaderParams.mParams.push_back( param );
            }

            // JAHSHAKA PATCH 0076: the direct volume's own unit, after every probe
            // array -- the same order setupBounceTextures() binds them in.
            param.name = "directVoxel";
            param.setManualValue( texSlotIdx );
            glslShaderParams.mParams.push_back( param );

            glslShaderParams.setDirty();
        }
    }
    //-------------------------------------------------------------------------
    void VctLighting::generateAnisotropicMips()
    {
        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();
        renderSystem->debugAnnotationPush( "VctLighting Anisotropic Mips" );

        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();

        mAnisoGeneratorStep0->analyzeBarriers( mResourceTransitions );
        renderSystem->executeResourceTransition( mResourceTransitions );
        hlmsCompute->dispatch( mAnisoGeneratorStep0, 0, 0 );

        FastArray<HlmsComputeJob *>::const_iterator itor = mAnisoGeneratorStep1.begin();
        FastArray<HlmsComputeJob *>::const_iterator endt = mAnisoGeneratorStep1.end();

        while( itor != endt )
        {
            ( *itor )->analyzeBarriers( mResourceTransitions );
            renderSystem->executeResourceTransition( mResourceTransitions );
            hlmsCompute->dispatch( *itor, 0, 0 );
            ++itor;
        }
        renderSystem->debugAnnotationPop();
    }
    //-------------------------------------------------------------------------
    void VctLighting::runBounce()
    {
        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();
        renderSystem->debugAnnotationPush( "VctLighting Bounce" );

        mBounceVoxelCellSize->setManualValue( mVoxelizer->getVoxelCellSize() );
        mBounceInvVoxelResolution->setManualValue( 1.0f / mVoxelizer->getVoxelResolution() );
        // JAHSHAKA PATCH 0076: ONE, AND IT IS THE PHYSICS.
        //
        // The bounce adds `albedo * G` where G is the six-cone weighted mean of the
        // voxel radiance -- weights that sum to 1 over a cosine-ish set, i.e. an
        // ESTIMATE OF E / PI already (that is exactly why HlmsPbs consumes the same
        // gather as `envColourD` with no division of its own,
        // 200.BRDFs_piece_ps.any: Rd = envColourD * albedo). The bounced outgoing
        // radiance of a Lambertian surface is rho * E / pi = rho * G: no further
        // factor exists to apply.
        //
        // Upstream divided by pi here (and its commented-out line divided by an extra
        // 1 + n/2 per pass). Both were fudges against the runaway this patch fixes at
        // the cause: the job re-gathered the TOTAL and added to it, so the series was
        // binomial in (1 + rho*G) and every extra pass amplified the first bounce
        // instead of adding a dimmer one. With the Jacobi form -- new = direct +
        // rho * G( total ) -- the series contracts by rho * f per pass on its own,
        // and a dampening of 1/pi would simply make every bounce pi times too dark
        // (which is what upstream's "more bounces for coarser cascades" stabilisation,
        // deleted by PHOTON-M1, was compensating for).
        mBounceIterationDampening->setManualValue( 1.0f );

        const size_t numCascades = mExtraCascades.size() + 1u;

        OGRE_ASSERT_LOW( numCascades < c_maxCascades && "VctLighting: Up to 16 cascades are supported" );

        // THE CHAIN'S PARAMETERS COME FROM THEIR ONE DEFINITION (Jahshaka,
        // PHOTON-READER-1): getCascadeChainParams, the same arrays the pixel pass
        // buffer and the irradiance field's generation job read, because all three run
        // the one cone march. This used to be a second formula set - a scalar
        // 1/smallestRes on all three axes, and a hop weight of the cascades'
        // mMultiplier ratio WITHOUT their baking multipliers - equal to the first only
        // while every volume is cubic and every cascade bakes at the same D_max.
        float4 vctInvResMaxLod[c_maxCascades];
        float4 fromPreviousProbeToNext[c_maxCascades][2];
        getCascadeChainParams( &vctInvResMaxLod[0].x, &fromPreviousProbeToNext[0][0].x );

        {
            const int32 numCascadesI32 = static_cast<int32>( numCascades );
            if( mLightVctBounceInject->getProperty( NumVctCascadesProp ) != numCascadesI32 )
                mLightVctBounceInject->setProperty( NumVctCascadesProp, numCascadesI32 );
        }

        mBounceInvResMaxLod->setManualValue( &vctInvResMaxLod[0].x,
                                             static_cast<uint32>( numCascades * 4u ) );
        if( !mExtraCascades.empty() )
        {
            mBounceFromPreviousProbeToNext->setManualValueEx(
                &fromPreviousProbeToNext[0]->x, static_cast<uint32>( ( numCascades - 1u ) * 2u * 4u ) );
        }
        else
        {
            mBounceFromPreviousProbeToNext->setManualValue( 0.0f );
            mBounceFromPreviousProbeToNext->isDirty = false;
        }
        mBounceShaderParams->mParams.swap( mLocalBounceShaderParams );
        mBounceShaderParams->setDirty();

        // THE BINDINGS ARE RE-ASSERTED PER DISPATCH, and it is not belt and braces —
        // it is the only way they can be right. Two independent reasons:
        //
        // (1) THE TEXTURES MOVE UNDER THE JOB. This function ends with
        //     `std::swap( mLightVoxel[0], mLightBounce )`, and re-binds slot 2 (its own
        //     light voxel) right after it — but slots 3..N hold the EXTRA CASCADES'
        //     mLightVoxel[], written once in setupBounceTextures() and never again.
        //     Every one of those cascades runs its own runBounce() and swaps its own
        //     pointers, so after an ODD number of bounce iterations on a cascade, this
        //     job is reading the texture that cascade has just stopped writing: the
        //     cross-cascade term of the bounce integrates stale radiance, silently, with
        //     no log line and no validation error. An EVEN number happens to come back
        //     to where it started (a swap is an involution), which is why this survived:
        //     upstream's own cascade manager gives every cascade the same bounce count.
        //     A per-cascade count is not exotic — the stabilisation upstream documents
        //     (a coarser cascade gets more bounces - upstream's VctCascadedVoxelizer,
        //     which is not in this fork)
        //     produces 1/2/4/8 on a four-cascade chain at three total bounces, and
        //     odd counts on outer cascades at other totals (1/1/2/4 at two).
        //
        // (2) THE JOB IS SHARED BY NAME. "VCT/LightVctBounceInject" is found by name, so
        //     every VctLighting in the process uses ONE HlmsComputeJob while the
        //     bindings belong to whichever instance called setupBounceTextures() last —
        //     the same shared-job class as this file's albedo/normal listeners. Two
        //     live chains would inject one chain's voxels through the other's textures.
        //     Re-asserting here makes the bindings belong to the instance DISPATCHING.
        //
        // The cost is the slot writes themselves: the unit count is change-guarded, the
        // GLSL unit list is rewritten (cheap, and OpenGL-only), and `false` skips the
        // OpenGL-only samplerblock
        // reference counting, which would otherwise leak a reference per dispatch (the
        // samplers do not move — only the textures do).
        setupBounceTextures( false );

        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
        mLightVctBounceInject->analyzeBarriers( mResourceTransitions );
        renderSystem->executeResourceTransition( mResourceTransitions );
        hlmsCompute->dispatch( mLightVctBounceInject, 0, 0 );

        std::swap( mLightVoxel[0], mLightBounce );

        DescriptorSetTexture2::TextureSlot texSlot( DescriptorSetTexture2::TextureSlot::makeEmpty() );
        texSlot.texture = mLightVoxel[0];
        mLightVctBounceInject->setTexture( 2, texSlot, mSamplerblockTrilinear );

        DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
        uavSlot.access = ResourceAccess::Write;
        uavSlot.texture = mLightBounce;
        uavSlot.pixelFormat = jahLightVoxelUavFormat();  // JAHSHAKA PATCH 0080
        mLightVctBounceInject->_setUavTexture( 0, uavSlot );

        if( mAnisotropic )
        {
            texSlot.texture = mLightVoxel[0];
            mAnisoGeneratorStep0->setTexture( 0, texSlot );

            generateAnisotropicMips();
        }

        if( mLightVoxel[0]->getNumMipmaps() > 1u )
        {
            renderSystem->debugAnnotationPush( "VctLighting::runBounce regular mipmaps" );
            mLightVoxel[0]->_autogenerateMipmaps();
            renderSystem->endCopyEncoder();
            renderSystem->debugAnnotationPop();
        }

        mBounceShaderParams->mParams.swap( mLocalBounceShaderParams );

        renderSystem->debugAnnotationPop();
    }
    //-------------------------------------------------------------------------
    void VctLighting::reserveExtraCascades( size_t numExtraCascades )
    {
        mExtraCascades.reserve( numExtraCascades );
    }
    //-------------------------------------------------------------------------
    void VctLighting::addCascade( VctLighting *cascade )
    {
        mExtraCascades.push_back( cascade );

        // THE CHAIN DECIDES HOW MANY TEXTURES THE BOUNCE SHADER DECLARES, so the job has
        // to be told the moment the chain grows. runBounce() sets
        // hlms_num_vct_cascades from mExtraCascades.size() + 1 on every injection, and
        // the generated compute shader declares one light-voxel texture per cascade
        // (four with anisotropy) — but the job's TEXTURE UNIT COUNT and its bindings are
        // only ever derived in setupBounceTextures(), which upstream calls from
        // setAllowMultipleBounces() and resetTexturesFromBuildRelative(). Add a cascade
        // after enabling bounces and the shader asks for ogre_t6 while the root layout
        // still has six units: "'ogre_t6': unrecognized layout identifier", the compute
        // program fails to compile, and the whole VctLighting arm renders no GI at all
        // with nothing but that one log line to say so.
        //
        // Nothing documents an order for these two calls (VctLighting's header does not),
        // and upstream's own VctCascadedVoxelizer (not in this fork) chained BEFORE it enabled
        // bounces, which is why upstream never meets this. Now either order works.
        if( getAllowMultipleBounces() )
            setupBounceTextures();
    }
    //-------------------------------------------------------------------------
    void VctLighting::setAllowMultipleBounces( bool bAllowMultipleBounces )
    {
        if( getAllowMultipleBounces() == bAllowMultipleBounces )
            return;

        TextureGpuManager *textureManager = mVoxelizer->getTextureGpuManager();
        if( bAllowMultipleBounces )
        {
            // JAHSHAKA PATCH 0080: not Reinterpretable -- see createTextures.
            uint32 texFlags = TextureFlags::Uav;
            if( !mAnisotropic || shouldEnableSpecularSdfQuality() )
                texFlags |= TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps;

            char tmpBuffer[128];
            LwString texName( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );
            texName.a( "VctLightingBounce/Id", getId() );
            TextureGpu *texture = textureManager->createTexture(
                texName.c_str(), GpuPageOutStrategy::Discard, texFlags, TextureTypes::Type3D );

            texture->setResolution( mLightVoxel[0]->getWidth(), mLightVoxel[0]->getHeight(),
                                    mLightVoxel[0]->getDepth() );
            texture->setNumMipmaps( mLightVoxel[0]->getNumMipmaps() );
            texture->setPixelFormat( jahLightVoxelFormat() );  // JAHSHAKA PATCH 0080
            texture->scheduleTransitionTo( GpuResidency::Resident );
            mLightBounce = texture;

            // JAHSHAKA PATCH 0076: the direct term's own volume, born and buried with
            // the bounce texture -- it is the bounce iteration that needs it, and
            // nothing else reads it. ONE mip: the bounce job reads it with a Load3D at
            // the voxel it is writing (the fixed point's D term at this cell), never
            // filtered and never at a coarser level, so the mip chain the total needs
            // for the cone gather would be dead weight (+14 % of the volume).
            texName.clear();
            texName.a( "VctLightingDirect/Id", getId() );
            TextureGpu *directTex = textureManager->createTexture(
                texName.c_str(), GpuPageOutStrategy::Discard,
                TextureFlags::Uav | TextureFlags::Reinterpretable, TextureTypes::Type3D );
            directTex->setResolution( mLightVoxel[0]->getWidth(), mLightVoxel[0]->getHeight(),
                                      mLightVoxel[0]->getDepth() );
            directTex->setNumMipmaps( 1u );
            directTex->setPixelFormat( PFG_RGBA8_UNORM_SRGB );
            directTex->scheduleTransitionTo( GpuResidency::Resident );
            mLightDirect = directTex;
        }
        else
        {
            restoreSwappedTextures();
            textureManager->destroyTexture( mLightBounce );
            mLightBounce = 0;
            if( mLightDirect )
            {
                textureManager->destroyTexture( mLightDirect );  // JAHSHAKA PATCH 0076
                mLightDirect = 0;
            }
        }

        if( mLightVctBounceInject )
        {
            if( mAnisotropic )
            {
                mLightVctBounceInject->setProperty( "vct_anisotropic", 1 );
                mLightVctBounceInject->setNumTexUnits( 6u );
            }
            else
            {
                mLightVctBounceInject->setProperty( "vct_anisotropic", 0 );
                mLightVctBounceInject->setNumTexUnits( 3u );
            }
        }

        if( bAllowMultipleBounces )
            setupBounceTextures();
        else
            setupGlslTextureUnits();
    }
    //-------------------------------------------------------------------------
    bool VctLighting::getAllowMultipleBounces() const { return mLightBounce != 0; }
    //-------------------------------------------------------------------------
    void VctLighting::setBakingMultiplier( float bakingMult ) { mBakingMultiplier = bakingMult; }
    //-------------------------------------------------------------------------
    void VctLighting::update( SceneManager *sceneManager, uint32 numBounces, float thinWallCounter,
                              bool autoMultiplier, float rayMarchStepScale, uint32 _lightMask )
    {
        OGRE_ASSERT_LOW( rayMarchStepScale >= 1.0f );

        checkTextures();

        // "VCT/LightInjection" is a job SHARED by every VctLighting in the process
        // (they all find it by name), while this property is a property of the
        // VOXELIZER being injected -- whether its albedo has the mips the area-light
        // shadow correction reads. createTextures() writes it, so with more than one
        // VctLighting alive the value in force is whichever one created its textures
        // LAST, not the one about to dispatch. Re-asserted here, per injection, from
        // the voxelizer this lighting actually samples. setProperty() only invalidates
        // the PSO cache when the value changes, so re-asserting the same value costs
        // nothing.
        mLightInjectionJob->setProperty(
            "correct_area_light_shadows",
            mVoxelizer->getAlbedoVox()->getNumMipmaps() > 1u ? 1 : 0 );

        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();

        renderSystem->debugAnnotationPush( "VctLighting Update" );

        mLightInjectionJob->setConstBuffer( 0, mLightsConstBuffer );

        DescriptorSetTexture2::TextureSlot texSlot( DescriptorSetTexture2::TextureSlot::makeEmpty() );
        texSlot.texture = mVoxelizer->getAlbedoVox();
        mLightInjectionJob->setTexture( 0, texSlot );
        texSlot.texture = mVoxelizer->getNormalVox();
        mLightInjectionJob->setTexture( 1, texSlot );
        texSlot.texture = mVoxelizer->getEmissiveVox();
        mLightInjectionJob->setTexture( 2, texSlot );

        DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
        uavSlot.access = ResourceAccess::Write;
        uavSlot.texture = mLightVoxel[0];
        uavSlot.pixelFormat = jahLightVoxelUavFormat();  // JAHSHAKA PATCH 0080
        mLightInjectionJob->_setUavTexture( 0, uavSlot );

        // JAHSHAKA PATCH 0076: THE SAME DISPATCH WRITES THE DIRECT TERM TWICE -- once
        // into the running total the bounce gathers from, once into the volume the
        // bounce's fixed point needs as its D term. It is one extra image store per
        // voxel inside a job that already walks every light's shadow ray per voxel;
        // no copy, no second dispatch, no encoder switch.
        //
        // RE-ASSERTED PER INJECTION, both the property and the slot, for the same
        // reason `correct_area_light_shadows` is above: "VCT/LightInjection" is ONE
        // HlmsComputeJob shared by name by every VctLighting in the process, and the
        // state in force belongs to whoever touched it last. A cascade with bounces
        // and a volume without them would otherwise take each other's UAV count --
        // and an unbound u1 on a shader that declares it is a dead descriptor.
        // The count is change-guarded because setNumUavUnits() invalidates the PSO
        // cache hash unconditionally.
        const uint8 numUavsNeeded = mLightDirect ? 2u : 1u;
        if( mLightInjectionJob->getNumUavUnits() != numUavsNeeded )
            mLightInjectionJob->setNumUavUnits( numUavsNeeded );
        mLightInjectionJob->setProperty( "vct_keep_direct", mLightDirect ? 1 : 0 );
        if( mLightDirect )
        {
            uavSlot.texture = mLightDirect;
            uavSlot.pixelFormat = PFG_RGBA8_UNORM;
            mLightInjectionJob->_setUavTexture( 1, uavSlot );
        }

        float autoMultiplierValue = 0.0f;

        const Vector3 voxelOrigin = mVoxelizer->getVoxelOrigin();
        const Vector3 invVoxelRes = 1.0f / mVoxelizer->getVoxelResolution();
        const Vector3 invVoxelSize = 1.0f / mVoxelizer->getVoxelSize();

        ShaderVctLight *RESTRICT_ALIAS vctLight = reinterpret_cast<ShaderVctLight *>(
            mLightsConstBuffer->map( 0, mLightsConstBuffer->getNumElements() ) );
        uint32 numCollectedLights = 0;
        const uint32 maxNumLights =
            static_cast<uint32>( mLightsConstBuffer->getNumElements() / sizeof( ShaderVctLight ) );

        const uint32 lightMask = _lightMask & VisibilityFlags::RESERVED_VISIBILITY_FLAGS;

        ObjectMemoryManager &memoryManager = sceneManager->_getLightMemoryManager();
        const size_t numRenderQueues = memoryManager.getNumRenderQueues();

        for( size_t i = 0; i < numRenderQueues; ++i )
        {
            ObjectData objData;
            const size_t totalObjs = memoryManager.getFirstObjectData( objData, i );

            for( size_t j = 0; j < totalObjs && numCollectedLights < maxNumLights;
                 j += ARRAY_PACKED_REALS )
            {
                for( size_t k = 0; k < ARRAY_PACKED_REALS && numCollectedLights < maxNumLights; ++k )
                {
                    uint32 *RESTRICT_ALIAS visibilityFlags = objData.mVisibilityFlags;

                    if( visibilityFlags[k] & VisibilityFlags::LAYER_VISIBILITY &&
                        visibilityFlags[k] & lightMask )
                    {
                        Light *light = static_cast<Light *>( objData.mOwner[k] );
                        if( light->getType() == Light::LT_DIRECTIONAL ||
                            light->getType() == Light::LT_POINT ||
                            light->getType() == Light::LT_SPOTLIGHT ||
                            light->getType() == Light::LT_AREA_APPROX ||
                            light->getType() == Light::LT_AREA_LTC )
                        {
                            const float maxVal = addLight( vctLight, light, voxelOrigin, invVoxelSize );
                            autoMultiplierValue = std::max( autoMultiplierValue, maxVal );
                            ++vctLight;
                            ++numCollectedLights;
                        }
                    }
                }

                objData.advancePack();
            }
        }

        mLightsConstBuffer->unmap( UO_KEEP_PERSISTENT );

        autoMultiplierValue /= Math::PI;
        // A SCENE WITH NO VISIBLE LIGHTS HAS NOTHING TO NORMALISE AGAINST, and the
        // arithmetic below turned that into "the VCT arm contributes nothing": the
        // maximum radiance collected above is exactly 0, its inverse is +inf, so
        // mInvBakingMultiplier comes out 0 and the shader's
        // blendWeight = blendFade * blend * multiplier is 0 for every voxel. That used
        // to be invisible (no lights, nothing to inject) but it is not any more: a
        // bound VctLighting switches the PBS ambient pieces off inside its volume, so an
        // AMBIENT-LIT scene whose only lamp is switched off goes black the moment VCT is
        // enabled. The fallback for "no measurement available" is the same one
        // autoMultiplier == false asks for -- mBakingMultiplier -- so the zero case is
        // folded into that branch. Note the collection loop above gathers only
        // LAYER_VISIBILITY lights, so a hidden lamp is this case too.
        if( !autoMultiplier || autoMultiplierValue <= 0.0f )
            autoMultiplierValue = mBakingMultiplier;
        else
            autoMultiplierValue = 1.0f / autoMultiplierValue;
        mInvBakingMultiplier = 1.0f / autoMultiplierValue;

        const Vector3 voxelRes( Real( mLightVoxel[0]->getWidth() ), Real( mLightVoxel[0]->getHeight() ),
                                Real( mLightVoxel[0]->getDepth() ) );
        const Vector3 voxelCellSize( mVoxelizer->getVoxelCellSize() );

        Vector3 dirCorrection( 1.0f / voxelCellSize );
        dirCorrection /= std::max( std::max( fabsf( dirCorrection.x ), fabsf( dirCorrection.y ) ),
                                   fabsf( dirCorrection.z ) );

        mNumLights->setManualValue( numCollectedLights );
        mRayMarchStepSize->setManualValue(
            Vector4( rayMarchStepScale / voxelRes, autoMultiplierValue ) );
        mVoxelCellSize->setManualValue( voxelCellSize );
        mDirCorrectionRatioThinWallCounter->setManualValue( Vector4( dirCorrection, thinWallCounter ) );
        mInvVoxelResolution->setManualValue( invVoxelRes );
        mShaderParams->setDirty();

        renderSystem->endCopyEncoder();

        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
        mLightInjectionJob->analyzeBarriers( mResourceTransitions );
        renderSystem->executeResourceTransition( mResourceTransitions );
        hlmsCompute->dispatch( mLightInjectionJob, 0, 0 );

        if( mAnisotropic )
            generateAnisotropicMips();

        if( mLightVoxel[0]->getNumMipmaps() > 1u )
        {
            renderSystem->debugAnnotationPush( "VctLighting::update regular mipmaps" );
            mLightVoxel[0]->_autogenerateMipmaps();
            renderSystem->endCopyEncoder();
            renderSystem->debugAnnotationPop();
        }

        if( numBounces > 0u )
        {
            if( !getAllowMultipleBounces() )
            {
                OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                             "numBounces must be 0, else call setAllowMultipleBounces first!",
                             "VctLighting::update" );
            }
            for( uint32 i = 0u; i < numBounces; ++i )
                runBounce();
        }

        if( mDebugVoxelVisualizer )
            mDebugVoxelVisualizer->setTrackingVoxel( mLightVoxel[0], mLightVoxel[0], true );

        renderSystem->debugAnnotationPop();
    }
    //-------------------------------------------------------------------------
    bool VctLighting::needsAmbientHemisphere() const
    {
        return memcmp( mUpperHemisphere, mLowerHemisphere, sizeof( mUpperHemisphere ) ) != 0;
    }
    //-------------------------------------------------------------------------
    void VctLighting::resetTexturesFromBuildRelative()
    {
        if( mDebugVoxelVisualizer )
        {
            Node *visNode = mDebugVoxelVisualizer->getParentNode();
            visNode->setPosition( mVoxelizer->getVoxelOrigin() );
            visNode->setScale( mVoxelizer->getVoxelCellSize() );

            // The visualizer is static so force-update its transform manually
            visNode->_getFullTransformUpdated();
            mDebugVoxelVisualizer->getWorldAabbUpdated();
        }

        if( mVoxelizerTexturesChanged )
        {
            checkTextures();
            return;
        }

        if( getAllowMultipleBounces() )
            setupBounceTextures();

        if( mAnisotropic )
        {
            DescriptorSetTexture2::TextureSlot texSlot(
                DescriptorSetTexture2::TextureSlot::makeEmpty() );
            texSlot.texture = mVoxelizer->getNormalVox();
            mAnisoGeneratorStep0->setTexture( 1, texSlot );
        }
    }
    //-------------------------------------------------------------------------
    size_t VctLighting::getConstBufferSize() const
    {
        size_t retVal = 10u * 4u * sizeof( float );
        retVal += ( 4u + 4u * 2u ) * sizeof( float ) * mExtraCascades.size();
        return retVal;
    }
    //-------------------------------------------------------------------------
    void VctLighting::getCascadeChainParams( float *RESTRICT_ALIAS outInvResMaxLod,
                                             float *RESTRICT_ALIAS outFromPrev ) const
    {
        // Moved out of fillConstBufferData VERBATIM (Jahshaka, PHOTON-READER-1): the pass
        // buffer's chain block is exactly these two arrays, back to back, and the
        // irradiance field's generation job reads the same chain through the same march.
        const float finalMultiplier = mInvBakingMultiplier * mMultiplier;

        const size_t numCascades = mExtraCascades.size() + 1u;

        // float4 vctInvResolution_cascadeMaxLod;
        for( size_t i = 0u; i < numCascades; ++i )
        {
            const VctLighting *cascade;
            if( i == 0u )
                cascade = this;
            else
                cascade = mExtraCascades[i - 1u];

            const TextureGpu *cascadeLightVoxel = cascade->mLightVoxel[0];
            const uint32 widthCascade = cascadeLightVoxel->getWidth();
            const uint32 heightCascade = cascadeLightVoxel->getHeight();
            const uint32 depthCascade = cascadeLightVoxel->getDepth();

            uint8 cascadeNumMipmaps = 0u;

            if( cascade->mLightVoxel[1] )
            {
                // Anisotropic has the number of mipmaps calculated
                cascadeNumMipmaps = cascade->mLightVoxel[1]->getNumMipmaps();
            }
            else
            {
                cascadeNumMipmaps = cascadeLightVoxel->getNumMipmaps();
            }

            *outInvResMaxLod++ = 1.0f / static_cast<float>( widthCascade );
            *outInvResMaxLod++ = 1.0f / static_cast<float>( heightCascade );
            *outInvResMaxLod++ = 1.0f / static_cast<float>( depthCascade );
            if( i == numCascades - 1u )
            {
                *outInvResMaxLod++ = 256.0f;  // cascadeMaxLod
            }
            else
            {
                const VctLighting *nextCascade = mExtraCascades[i];

                const Vector3 cascadeVoxelCellSize = cascade->mVoxelizer->getVoxelCellSize();
                const Vector3 nextCascadeVoxelCellSize = nextCascade->mVoxelizer->getVoxelCellSize();

                const Vector3 currToNextFactor = nextCascadeVoxelCellSize / cascadeVoxelCellSize;
                const float maxFactor =
                    std::max( currToNextFactor.x, std::max( currToNextFactor.y, currToNextFactor.z ) );

                *outInvResMaxLod++ =
                    std::min<float>( Math::Log2( maxFactor ), cascadeNumMipmaps );  // cascadeMaxLod
            }
        }

        // float4 fromPreviousProbeToNext[numCascades - 1u][2]
        for( size_t i = 1u; i < numCascades; ++i )
        {
            const VctLighting *cascade = mExtraCascades[i - 1u];
            const VctLighting *prevCascade;
            if( i == 1u )
                prevCascade = this;
            else
                prevCascade = mExtraCascades[i - 2u];

            const Vector3 cascadeVoxelSize = cascade->mVoxelizer->getVoxelSize();
            const Vector3 vScale = prevCascade->mVoxelizer->getVoxelSize() / cascadeVoxelSize;
            const Vector3 vPos =
                ( prevCascade->mVoxelizer->getVoxelOrigin() - cascade->mVoxelizer->getVoxelOrigin() ) /
                cascadeVoxelSize;

            const float cascadeFinalMultiplier =
                cascade->mInvBakingMultiplier * cascade->mMultiplier / finalMultiplier;

            float cascadeNumMipmaps = 0u;

            if( cascade->mLightVoxel[1] )
            {
                // Anisotropic has the number of mipmaps calculated
                cascadeNumMipmaps = static_cast<float>( cascade->mLightVoxel[1]->getNumMipmaps() );
            }
            else
            {
                const TextureGpu *cascadeLightVoxel = cascade->mLightVoxel[0];
                cascadeNumMipmaps = static_cast<float>( cascadeLightVoxel->getNumMipmaps() );
            }

            *outFromPrev++ = static_cast<float>( vScale.x );
            *outFromPrev++ = static_cast<float>( vScale.y );
            *outFromPrev++ = static_cast<float>( vScale.z );
            *outFromPrev++ = cascadeFinalMultiplier;

            *outFromPrev++ = static_cast<float>( vPos.x );
            *outFromPrev++ = static_cast<float>( vPos.y );
            *outFromPrev++ = static_cast<float>( vPos.z );
            // HACK: This is so hacky it hurts: cascadeNumMipmaps^3 empirically looks reasonably
            // good for brightness. We need a better way to equalize specular. Specular
            // brightness equalization depends on:
            //      - Roughness (as it affects lighting)
            //      - Cell Size Volume
            *outFromPrev++ = 1.0f / ( cascadeNumMipmaps * cascadeNumMipmaps * cascadeNumMipmaps );
        }
    }
    //-------------------------------------------------------------------------
    void VctLighting::fillConstBufferData( const Matrix4 &viewMatrix,
                                           float *RESTRICT_ALIAS passBufferPtr ) const
    {
        const uint32 width = mLightVoxel[0]->getWidth();
        const uint32 height = mLightVoxel[0]->getHeight();
        const uint32 depth = mLightVoxel[0]->getDepth();

        const float smallestRes = static_cast<float>( std::min( std::min( width, height ), depth ) );

        const float maxMipmapCount = static_cast<float>(
            PixelFormatGpuUtils::getMaxMipmapCount( static_cast<uint32>( smallestRes ) ) );

        const float mipDiff = ( maxMipmapCount - 8.0f ) * 0.5f;

        const float finalMultiplier = mInvBakingMultiplier * mMultiplier;
        const float invFinalMultiplier = 1.0f / finalMultiplier;

        const size_t numCascades = mExtraCascades.size() + 1u;

        // float4 vctInvResolution_cascadeMaxLod[numCascades];
        // float4 fromPreviousProbeToNext[numCascades - 1u][2]
        // (Jahshaka, PHOTON-READER-1: one definition, shared with the irradiance field.)
        getCascadeChainParams( passBufferPtr, passBufferPtr + 4u * numCascades );
        passBufferPtr += 4u * numCascades + 8u * ( numCascades - 1u );

        // float specSdfMaxMip;
        // float specularSdfFactor;
        // float blendFade;
        // float multiplier;
        *passBufferPtr++ = 7.0f + mipDiff;
        // Where did 0.1875f & 0.3125f come from? Empirically obtained.
        // At 128x128x128, values in range [24; 40] gave good results.
        // Below 24, quality became unnacceptable.
        // Past 40, performance only went down without visible changes.
        // Thus 24 / 128 and 40 / 128 = 0.1875f and 0.3125f
        *passBufferPtr++ = Math::lerp( 0.1875f, 0.3125f, mSpecularSdfQuality ) * smallestRes;
        *passBufferPtr++ = 1.0f;
        *passBufferPtr++ = finalMultiplier;

        // float4 ambientUpperHemi
        *passBufferPtr++ = mUpperHemisphere[0] * invFinalMultiplier;
        *passBufferPtr++ = mUpperHemisphere[1] * invFinalMultiplier;
        *passBufferPtr++ = mUpperHemisphere[2] * invFinalMultiplier;
        *passBufferPtr++ = 0.0f;

        // float4 ambientLowerHemi
        *passBufferPtr++ = mLowerHemisphere[0] * invFinalMultiplier;
        *passBufferPtr++ = mLowerHemisphere[1] * invFinalMultiplier;
        *passBufferPtr++ = mLowerHemisphere[2] * invFinalMultiplier;
        *passBufferPtr++ = 0.0f;

        Matrix4 xform, invXForm;
        xform.makeTransform( -mVoxelizer->getVoxelOrigin() / mVoxelizer->getVoxelSize(),
                             1.0f / mVoxelizer->getVoxelSize(), Quaternion::IDENTITY );
        // xform = xform * viewMatrix.inverse();
        xform = xform.concatenateAffine( viewMatrix.inverseAffine() );
        invXForm = xform.inverseAffine();

        // float4 xform_row0;
        // float4 xform_row1;
        // float4 xform_row2;
        for( size_t i = 0; i < 12u; ++i )
            *passBufferPtr++ = static_cast<float>( xform[0][i] );

        // float4 invXform_row0;
        // float4 invXform_row1;
        // float4 invXform_row2;
        for( size_t i = 0; i < 12u; ++i )
            *passBufferPtr++ = static_cast<float>( invXForm[0][i] );
    }
    //-------------------------------------------------------------------------
    bool VctLighting::shouldEnableSpecularSdfQuality() const
    {
        return mVoxelizer->getAlbedoVox()->getWidth() > 32u &&
               mVoxelizer->getAlbedoVox()->getHeight() > 32u &&
               mVoxelizer->getAlbedoVox()->getDepth() > 32u;
    }
    //-------------------------------------------------------------------------
    void VctLighting::setDebugVisualization( bool bShow, SceneManager *sceneManager )
    {
        if( bShow == getDebugVisualizationMode() )
            return;

        if( !bShow )
        {
            SceneNode *sceneNode = mDebugVoxelVisualizer->getParentSceneNode();
            sceneNode->getParentSceneNode()->removeAndDestroyChild( sceneNode );
            OGRE_DELETE mDebugVoxelVisualizer;
            mDebugVoxelVisualizer = 0;
        }
        else
        {
            SceneNode *rootNode = sceneManager->getRootSceneNode( SCENE_STATIC );
            SceneNode *visNode = rootNode->createChildSceneNode( SCENE_STATIC );

            mDebugVoxelVisualizer = OGRE_NEW VoxelVisualizer(
                Ogre::Id::generateNewId<Ogre::MovableObject>(),
                &sceneManager->_getEntityMemoryManager( SCENE_STATIC ), sceneManager, 0u );

            mDebugVoxelVisualizer->setTrackingVoxel( mLightVoxel[0], mLightVoxel[0], true );

            visNode->setPosition( mVoxelizer->getVoxelOrigin() );
            visNode->setScale( mVoxelizer->getVoxelCellSize() );
            visNode->attachObject( mDebugVoxelVisualizer );
        }
    }
    //-------------------------------------------------------------------------
    bool VctLighting::getDebugVisualizationMode() const { return mDebugVoxelVisualizer != 0; }
    //-------------------------------------------------------------------------
    void VctLighting::setAnisotropic( bool bAnisotropic )
    {
        if( mAnisotropic != bAnisotropic )
        {
            mAnisotropic = bAnisotropic;
            createTextures();
        }
    }
    //-------------------------------------------------------------------------
    void VctLighting::setAmbient( const ColourValue &upperHemisphere,
                                  const ColourValue &lowerHemisphere )
    {
        for( size_t i = 0; i < 3u; ++i )
        {
            mUpperHemisphere[i] = static_cast<float>( upperHemisphere[i] );
            mLowerHemisphere[i] = static_cast<float>( lowerHemisphere[i] );
        }
    }
    //-------------------------------------------------------------------------
    TextureGpu **VctLighting::getLightVoxelTextures( const size_t cascadeIdx )
    {
        if( cascadeIdx == 0u )
            return mLightVoxel;
        else
            return mExtraCascades[cascadeIdx - 1u]->mLightVoxel;
    }
    //-------------------------------------------------------------------------
    void VctLighting::notifyTextureChanged( TextureGpu *texture, TextureGpuListener::Reason reason,
                                            void *extraData )
    {
        if( reason == TextureGpuListener::LostResidency || reason == TextureGpuListener::Deleted )
            mVoxelizerTexturesChanged = true;

        if( reason == TextureGpuListener::Deleted )
        {
            texture->removeListener( this );
            mVoxelizerListenersRemoved = true;
        }
    }
}  // namespace Ogre
