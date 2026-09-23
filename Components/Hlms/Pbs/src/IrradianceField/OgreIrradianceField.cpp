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

#include "IrradianceField/OgreIrradianceField.h"

#include "IrradianceField/OgreIfdProbeVisualizer.h"
#include "IrradianceField/OgreIrradianceFieldRaster.h"
#include "Vct/OgreVctLighting.h"
#include "Vct/OgreVctVoxelizerSourceBase.h"

#include "Compositor/OgreCompositorManager2.h"
#include "Compositor/OgreCompositorWorkspace.h"
#include "OgreRoot.h"

#include "OgreBitwise.h"
#include "OgreHlmsCompute.h"
#include "OgreHlmsComputeJob.h"
#include "OgreHlmsManager.h"
#include "OgreLogManager.h"
#include "OgreStringConverter.h"
#include "OgreTextureGpuManager.h"
#include "Vao/OgreConstBufferPacked.h"
#include "Vao/OgreTexBufferPacked.h"
#include "Vao/OgreVaoManager.h"

#define TODO_handle_leftover

namespace Ogre
{
    RasterParams::RasterParams() :
        mPixelFormat( PFG_RGBA8_UNORM_SRGB ),
        mCameraNear( 0.01f ),
        mCameraFar( 500.0f )
    {
    }
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    IrradianceFieldSettings::IrradianceFieldSettings() :
        mNumRaysPerPixel( 1u ),
        mDepthProbeResolution( 12u ),
        mIrradianceResolution( 6u )
    {
        for( size_t i = 0u; i < 3u; ++i )
            mNumProbes[i] = 32u;
        mNumProbes[1] = 8u;
    }
    //-------------------------------------------------------------------------
    bool IrradianceFieldSettings::isRaster() const { return mRasterParams.mWorkspaceName != IdString(); }
    //-------------------------------------------------------------------------
    void IrradianceFieldSettings::createSubsamples()
    {
        if( isRaster() )
            return;

        const size_t numRaysPerPixel = mNumRaysPerPixel;
        mSubsamples.resize( numRaysPerPixel );

        if( numRaysPerPixel == 1u )
            mSubsamples[0] = Vector2( 0.5f, 0.5f );
        else if( numRaysPerPixel == 2u )
        {
            mSubsamples[0] = Vector2( 0.75f, 0.75f );
            mSubsamples[1] = Vector2( 0.25f, 0.25f );
        }
        else if( numRaysPerPixel == 3u )
        {
            mSubsamples[0] = Vector2( 0.50f, 0.75f );
            mSubsamples[1] = Vector2( 0.25f, 0.25f );
            mSubsamples[2] = Vector2( 0.75f, 0.25f );
        }
        else
        {
            const float fGridSize = ceilf( sqrtf( (float)numRaysPerPixel ) );
            const float invGridSize = 1.0f / fGridSize;
            const size_t gridSize = static_cast<size_t>( fGridSize );
            const size_t numGridCells = gridSize * gridSize;

            for( size_t i = 0u; i < numGridCells && i < numRaysPerPixel; ++i )
            {
                mSubsamples[i].x = ( Real( i % ( gridSize ) ) + 0.5f ) * invGridSize;
                mSubsamples[i].y = ( Real( i / ( gridSize ) ) + 0.5f ) * invGridSize;
            }
        }
    }
    //-------------------------------------------------------------------------
    uint32 IrradianceFieldSettings::getTotalNumProbes() const
    {
        return mNumProbes[0] * mNumProbes[1] * mNumProbes[2];
    }
    //-------------------------------------------------------------------------
    void IrradianceFieldSettings::getDepthProbeFullResolution( uint32 &outWidth,
                                                               uint32 &outHeight ) const
    {
        // totalNumProbes is a power of 2, thus it can be expressed as 2ⁿ
        // Hence find the resolution where 2ᵃ * 2ᵇ = 2ⁿ
        const uint32 totalNumProbes = getTotalNumProbes();
        const uint32 exponent = Bitwise::ctz32( totalNumProbes );
        const uint8 borderedDepthResolution = getBorderedDepthResolution();
        outWidth = borderedDepthResolution * ( 1u << ( exponent >> 1u ) );
        outHeight = borderedDepthResolution * ( 1u << ( exponent - ( exponent >> 1u ) ) );
        OGRE_ASSERT_LOW( outWidth * outHeight ==
                         totalNumProbes * borderedDepthResolution * borderedDepthResolution );
    }
    //-------------------------------------------------------------------------
    void IrradianceFieldSettings::getIrradProbeFullResolution( uint32 &outWidth,
                                                               uint32 &outHeight ) const
    {
        const uint32 totalNumProbes = getTotalNumProbes();
        const uint32 exponent = Bitwise::ctz32( totalNumProbes );
        const uint8 borderedIrradResolution = getBorderedIrradResolution();
        outWidth = borderedIrradResolution * ( 1u << ( exponent >> 1u ) );
        outHeight = borderedIrradResolution * ( 1u << ( exponent - ( exponent >> 1u ) ) );
        OGRE_ASSERT_LOW( outWidth * outHeight ==
                         totalNumProbes * borderedIrradResolution * borderedIrradResolution );
    }
    //-------------------------------------------------------------------------
    uint8 IrradianceFieldSettings::getBorderedIrradResolution() const
    {
        return mIrradianceResolution + 2u;
    }
    //-------------------------------------------------------------------------
    uint8 IrradianceFieldSettings::getBorderedDepthResolution() const
    {
        return mDepthProbeResolution + 2u;
    }
    //-------------------------------------------------------------------------
    uint32 IrradianceFieldSettings::getNumRaysPerIrradiancePixel() const
    {
        return mDepthProbeResolution * mDepthProbeResolution * mNumRaysPerPixel /
               ( mIrradianceResolution * mIrradianceResolution );
    }
    //-------------------------------------------------------------------------
    Vector3 IrradianceFieldSettings::getNumProbes3f() const
    {
        return Vector3( static_cast<Real>( mNumProbes[0] ), static_cast<Real>( mNumProbes[1] ),
                        static_cast<Real>( mNumProbes[2] ) );
    }
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    IrradianceField::IrradianceField( Root *root, SceneManager *sceneManager ) :
        IdObject( Id::generateNewId<IrradianceField>() ),
        mNumProbesProcessed( 0u ),
        mFieldOrigin( Vector3::ZERO ),
        mFieldSize( Vector3::ZERO ),
        mDepthMaxIntegrationTapsPerPixel( 0u ),
        mColourMaxIntegrationTapsPerPixel( 0u ),
        mVctLighting( 0 ),
        mIrradianceTex( 0 ),
        mDepthVarianceTex( 0 ),
        mGenerationWorkspace( 0 ),
        mGenerationJob( 0 ),
        mDepthIntegrationJob( 0 ),
        mColourIntegrationJob( 0 ),
        mDepthMirrorBorderJob( 0 ),
        mColourMirrorBorderJob( 0 ),
        mIfGenParamsBuffer( 0 ),
        mDirectionsBuffer( 0 ),
        mDepthTapsIntegrationBuffer( 0 ),
        mColourTapsIntegrationBuffer( 0 ),
        mIfdDepthBorderMirrorParamsBuffer( 0 ),
        mIfdColourBorderMirrorParamsBuffer( 0 ),
        mIfRaster( 0 ),
        mDebugVisualizationMode( DebugVisualizationNone ),
        mDebugTessellation( 4u ),
        mDebugIfdProbeVisualizer( 0 ),
        mRoot( root ),
        mSceneManager( sceneManager ),
        mAlreadyWarned( false )
    {
#if OGRE_NO_JSON
        OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                     "To use IrradianceField, Ogre must be build with JSON support "
                     "and you must include the resources bundled at "
                     "Samples/Media/Compute",
                     "IrradianceField::IrradianceField" );
#endif
        VaoManager *vaoManager = mRoot->getRenderSystem()->getVaoManager();
        mIfGenParamsBuffer = vaoManager->createConstBuffer( sizeof( IrradianceFieldGenParams ),
                                                            BT_DYNAMIC_PERSISTENT, 0, false );

        mIfdDepthBorderMirrorParamsBuffer =
            vaoManager->createConstBuffer( sizeof( IfdBorderMirrorParams ), BT_DEFAULT, 0, false );
        mIfdColourBorderMirrorParamsBuffer =
            vaoManager->createConstBuffer( sizeof( IfdBorderMirrorParams ), BT_DEFAULT, 0, false );

        HlmsCompute *hlmsCompute = mRoot->getHlmsManager()->getComputeHlms();
        mGenerationJob = hlmsCompute->findComputeJobNoThrow( "IrradianceField/Gen" );

        if( !mGenerationJob )
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                         "To use IrradianceField, you must include the resources bundled at "
                         "Samples/Media/Compute\n"
                         "Could not find IrradianceField/Gen",
                         "IrradianceField::IrradianceField" );
        }

        mDepthIntegrationJob = hlmsCompute->findComputeJob( "IrradianceField/Integration/Depth" );
        mColourIntegrationJob = hlmsCompute->findComputeJob( "IrradianceField/Integration/Colour" );

        mDepthMirrorBorderJob = hlmsCompute->findComputeJob( "IrradianceField/BorderMirror/Depth" );
        mColourMirrorBorderJob = hlmsCompute->findComputeJob( "IrradianceField/BorderMirror/Colour" );
    }
    //-------------------------------------------------------------------------
    IrradianceField::~IrradianceField()
    {
        setDebugVisualization( DebugVisualizationNone, 0, mDebugTessellation );
        destroyTextures();

        delete mIfRaster;
        mIfRaster = 0;

        VaoManager *vaoManager = mRoot->getRenderSystem()->getVaoManager();
        if( mIfGenParamsBuffer->getMappingState() != MS_UNMAPPED )
            mIfGenParamsBuffer->unmap( UO_UNMAP_ALL );
        vaoManager->destroyConstBuffer( mIfGenParamsBuffer );
        mIfGenParamsBuffer = 0;
    }
    //-------------------------------------------------------------------------
    void IrradianceField::fillDirections( float *RESTRICT_ALIAS outBuffer )
    {
        OGRE_ASSERT_LOW( !mSettings.isRaster() );

        float *RESTRICT_ALIAS updateData = reinterpret_cast<float * RESTRICT_ALIAS>( outBuffer );
#if OGRE_DEBUG_MODE >= OGRE_DEBUG_LOW
        const float *RESTRICT_ALIAS updateDataStart = updateData;
#endif

        const Vector2 *subsamples = &mSettings.getSubsamples()[0];

        const size_t numRaysPerPixel = mSettings.mNumRaysPerPixel;
        const size_t depthProbeRes = mSettings.mDepthProbeResolution;
        const size_t irradProbeRes = mSettings.mIrradianceResolution;

        const size_t colourToDepthRatio = depthProbeRes / irradProbeRes;

        for( size_t irradY = 0u; irradY < irradProbeRes; ++irradY )
        {
            const size_t y = irradY * colourToDepthRatio;
            for( size_t irradX = 0u; irradX < irradProbeRes; ++irradX )
            {
                const size_t x = irradX * colourToDepthRatio;

                for( size_t blockY = 0u; blockY < colourToDepthRatio; ++blockY )
                {
                    for( size_t blockX = 0u; blockX < colourToDepthRatio; ++blockX )
                    {
                        for( size_t rayIdx = 0u; rayIdx < numRaysPerPixel; ++rayIdx )
                        {
                            Vector2 uvOct = Vector2( Real( x + blockX ),  //
                                                     Real( y + blockY ) ) +
                                            subsamples[rayIdx];
                            uvOct /= static_cast<float>( depthProbeRes );

                            Vector3 directionVector = Math::octahedronMappingDecode( uvOct );

                            *updateData++ = static_cast<float>( directionVector.x );
                            *updateData++ = static_cast<float>( directionVector.y );
                            *updateData++ = static_cast<float>( directionVector.z );
                            *updateData++ = 0.0f;
                        }
                    }
                }
            }
        }

        OGRE_ASSERT_LOW( (size_t)( updateData - updateDataStart ) <=
                         ( depthProbeRes * depthProbeRes * numRaysPerPixel * 4u ) );
    }
    //-------------------------------------------------------------------------
    TexBufferPacked *IrradianceField::setupIntegrationTaps( VaoManager *vaoManager, uint32 probeRes,
                                                            uint32 fullWidth,
                                                            HlmsComputeJob *integrationJob,
                                                            ConstBufferPacked *ifGenParamsBuffer,
                                                            uint32 &outMaxIntegrationTapsPerPixel )
    {
        const uint32 maxIntegrationTapsPerPixel = countNumIntegrationTaps( probeRes );
        const size_t bufferSize = probeRes * probeRes * maxIntegrationTapsPerPixel * sizeof( float2 );
        float2 *integrationTapsBuffer =
            reinterpret_cast<float2 *>( OGRE_MALLOC_SIMD( bufferSize, MEMCATEGORY_GEOMETRY ) );
        FreeOnDestructor dataPtr( integrationTapsBuffer );

        fillIntegrationWeights( integrationTapsBuffer, probeRes, maxIntegrationTapsPerPixel );

        TexBufferPacked *retVal = vaoManager->createTexBuffer( PFG_RG32_FLOAT, bufferSize, BT_DEFAULT,
                                                               integrationTapsBuffer, false );

        integrationJob->setConstBuffer( 0, ifGenParamsBuffer );
        DescriptorSetTexture2::BufferSlot bufferSlot( DescriptorSetTexture2::BufferSlot::makeEmpty() );
        bufferSlot.buffer = retVal;
        integrationJob->setTexBuffer( 0, bufferSlot );
        integrationJob->setProperty( "num_taps", static_cast<int32>( maxIntegrationTapsPerPixel ) );
        integrationJob->setProperty( "probe_resolution", static_cast<int32>( probeRes ) );
        integrationJob->setProperty( "full_width", static_cast<int32>( fullWidth ) );

        integrationJob->setThreadsPerGroup( probeRes, probeRes, 1u );

        outMaxIntegrationTapsPerPixel = maxIntegrationTapsPerPixel;

        return retVal;
    }
    //-------------------------------------------------------------------------
    uint32 IrradianceField::countNumIntegrationTaps( uint32 probeRes )
    {
        uint32 maxNumTaps = 0u;

        for( size_t y = 0u; y < probeRes; ++y )
        {
            for( size_t x = 0u; x < probeRes; ++x )
            {
                Vector2 uvOct = Vector2( (Real)x, (Real)y );
                uvOct /= static_cast<float>( probeRes );
                Vector3 directionVector = Math::octahedronMappingDecode( uvOct );

                uint32 numTaps = 0u;

                for( size_t otherY = 0u; otherY < probeRes; ++otherY )
                {
                    for( size_t otherX = 0u; otherX < probeRes; ++otherX )
                    {
                        Vector2 otherUv = Vector2( (Real)otherX, (Real)otherY );
                        otherUv /= static_cast<float>( probeRes );
                        Vector3 otherDir = Math::octahedronMappingDecode( otherUv );

                        const Real dotProduct = directionVector.dotProduct( otherDir );
                        if( dotProduct > 0 )
                            ++numTaps;
                    }
                }

                maxNumTaps = std::max( numTaps, maxNumTaps );
            }
        }

        return maxNumTaps;
    }
    //-------------------------------------------------------------------------
    void IrradianceField::fillIntegrationWeights( float2 *RESTRICT_ALIAS outBuffer, uint32 probeRes,
                                                  uint32 maxTapsPerPixel )
    {
        float2 *RESTRICT_ALIAS updateData = reinterpret_cast<float2 * RESTRICT_ALIAS>( outBuffer );
#if OGRE_DEBUG_MODE >= OGRE_DEBUG_LOW
        const float2 *RESTRICT_ALIAS updateDataStart = updateData;
#endif

        for( size_t y = 0u; y < probeRes; ++y )
        {
            for( size_t x = 0u; x < probeRes; ++x )
            {
                Vector2 uvOct = Vector2( (Real)x, (Real)y );
                uvOct /= static_cast<float>( probeRes );
                Vector3 directionVector = Math::octahedronMappingDecode( uvOct );

                float2 *RESTRICT_ALIAS updateDataCheckpoint = updateData;

                float accumWeight = 0.0f;

                for( size_t otherY = 0u; otherY < probeRes; ++otherY )
                {
                    for( size_t otherX = 0u; otherX < probeRes; ++otherX )
                    {
                        Vector2 otherUv = Vector2( (Real)otherX, (Real)otherY );
                        otherUv /= static_cast<float>( probeRes );
                        Vector3 otherDir = Math::octahedronMappingDecode( otherUv );

                        const Real dotProduct = directionVector.dotProduct( otherDir );
                        if( dotProduct > 0 )
                        {
                            updateData->x = static_cast<float>( otherY * probeRes + otherX );
                            updateData->y = dotProduct;
                            ++updateData;
                            accumWeight += dotProduct;
                        }
                    }
                }

                const uint32 numTaps = static_cast<uint32>( updateData - updateDataCheckpoint );

                OGRE_ASSERT_LOW( accumWeight > 0 &&
                                 "accumWeight can't be 0. It must've at least evalute to itself!" );

                // Normalize weights
                updateData = updateDataCheckpoint;
                const float invAccumWeight = 1.0f / accumWeight;
                for( size_t i = 0u; i < numTaps; ++i )
                {
                    updateData->y *= invAccumWeight;
                    ++updateData;
                }

                const uint32 maxIntegrationTapsPerPixel = maxTapsPerPixel;
                for( size_t i = numTaps; i < maxIntegrationTapsPerPixel; ++i )
                {
                    updateData->x = float( y * probeRes + x );
                    updateData->y = 0;
                    ++updateData;
                }
            }
        }

        OGRE_ASSERT_LOW( (size_t)( updateData - updateDataStart ) <=
                         ( probeRes * probeRes * maxTapsPerPixel ) );
    }
    //-------------------------------------------------------------------------
    void IrradianceField::setIrradianceFieldGenParams()
    {
        if( mSettings.isRaster() )
        {
            // Avoid Valgrind from complaining when we copy the whole struct to GPU for the integrator
            silent_memset( &mIfGenParams, 0, sizeof( mIfGenParams ) );
            return;
        }

        const uint32 numRaysPerPixel = mSettings.mNumRaysPerPixel;
        const uint32 depthProbeRes = mSettings.mDepthProbeResolution;
        const uint32 irradProbeRes = mSettings.mIrradianceResolution;
        const uint32 numRaysPerIrradiancePixel = mSettings.getNumRaysPerIrradiancePixel();

        const uint32 numRaysPerProbe = depthProbeRes * depthProbeRes * numRaysPerPixel;

        mIfGenParams.invNumRaysPerPixel = 1.0f / float( numRaysPerPixel );
        mIfGenParams.invNumRaysPerIrradiancePixel = 1.0f / float( numRaysPerIrradiancePixel );
        // Uploaded with the rest of the struct on every update, so it must hold
        // something: the voxel branch never wrote it (only the raster branch's
        // memset did), so the generation job read an uninitialised float here.
        mIfGenParams.unused0 = 0.0f;

        mIfGenParams.coneAngleTan = Math::Tan( Math::TWO_PI / static_cast<float>( numRaysPerProbe ) );
        mIfGenParams.numProcessedProbes = 0u;
        mIfGenParams.unused1 = 0.0f;
        mIfGenParams.unused2 = 0.0f;
        fillChainParams();
        fillEnvironmentParams();

        mIfGenParams.numProbes_threadsPerRow.x = mSettings.mNumProbes[0];
        mIfGenParams.numProbes_threadsPerRow.y = mSettings.mNumProbes[1];
        mIfGenParams.numProbes_threadsPerRow.z = mSettings.mNumProbes[2];
        mIfGenParams.numProbes_threadsPerRow.w = 0u;

        const VctVoxelizerSourceBase *voxelizer = mVctLighting->getVoxelizer();
        Matrix4 irrProbeToVctTransform;
        irrProbeToVctTransform.makeTransform(
            ( mFieldOrigin - voxelizer->getVoxelOrigin() ) / voxelizer->getVoxelSize(),
            ( mFieldSize / voxelizer->getVoxelSize() ) / mSettings.getNumProbes3f(),
            Quaternion::IDENTITY );
        mIfGenParams.irrProbeToVctTransform = irrProbeToVctTransform;

        mGenerationJob->setProperty( "num_rays_per_probe", static_cast<int32>( numRaysPerProbe ) );

        mGenerationJob->setProperty( "num_rays_per_irrad_pixel",
                                     static_cast<int32>( numRaysPerIrradiancePixel ) );
        mGenerationJob->setProperty( "irrad_resolution", static_cast<int32>( irradProbeRes ) );
        mGenerationJob->setProperty( "num_irrad_pixels_per_probe",
                                     static_cast<int32>( irradProbeRes * irradProbeRes ) );
        mGenerationJob->setProperty( "irrad_full_width",
                                     static_cast<int32>( mIrradianceTex->getWidth() ) );

        mGenerationJob->setProperty( "depth_resolution", static_cast<int32>( depthProbeRes ) );
        mGenerationJob->setProperty( "depth_full_width",
                                     static_cast<int32>( mDepthVarianceTex->getWidth() ) );
        mGenerationJob->setProperty( "colour_to_depth_resolution_ratio",
                                     static_cast<int32>( depthProbeRes / irradProbeRes ) );

        mGenerationJob->setProperty( "reduction_iterations",
                                     static_cast<int32>( numRaysPerIrradiancePixel / numRaysPerPixel ) );
        mGenerationJob->setProperty( "num_rays_per_depth_pixel", static_cast<int32>( numRaysPerPixel ) );
    }
    //-------------------------------------------------------------------------
    void IrradianceField::setupBorderMirrorParams( uint32 borderedRes, uint32 fullWidth,
                                                   ConstBufferPacked *ifdBorderMirrorParamsBuffer,
                                                   HlmsComputeJob *job )
    {
        const uint32 totalNumProbes = mSettings.getTotalNumProbes();
        IfdBorderMirrorParams mirrorParams;
        memset( &mirrorParams, 0, sizeof( mirrorParams ) );
        mirrorParams.probeBorderedRes = borderedRes;
        mirrorParams.numPixelsInEdges = ( borderedRes - 2u ) * 2u;
        mirrorParams.numTopBottomPixels = ( borderedRes - 2u );
        mirrorParams.numGlobalThreadsForEdges = mirrorParams.numPixelsInEdges * totalNumProbes;
        mirrorParams.maxThreadId = ( mirrorParams.numPixelsInEdges + 1u ) * totalNumProbes;

        const uint32 threadsPerGroup = 128u;
        const uint32 totalThreads =
            (uint32)alignToNextMultiple( mirrorParams.maxThreadId, threadsPerGroup );
        const uint32 numWorkGroups = totalThreads / threadsPerGroup;
        const uint32 numThreadGroupsY = numWorkGroups / 65535u + 1u;
        const uint32 numThreadGroupsX = numWorkGroups / numThreadGroupsY;

        mirrorParams.threadsPerThreadRow = numThreadGroupsX;
        ifdBorderMirrorParamsBuffer->upload( &mirrorParams, 0,
                                             ifdBorderMirrorParamsBuffer->getTotalSizeBytes() );

        job->setProperty( "full_width", static_cast<int32>( fullWidth ) );
        job->setConstBuffer( 0, ifdBorderMirrorParamsBuffer );

        job->setThreadsPerGroup( 128u, 1u, 1u );
        job->setNumThreadGroups( numThreadGroupsX, numThreadGroupsY, 1u );
    }
    //-------------------------------------------------------------------------
    void IrradianceField::initialize( const IrradianceFieldSettings &settings,
                                      const Vector3 &fieldOrigin, const Vector3 &fieldSize,
                                      VctLighting *vctLighting )
    {
        mSettings = settings;
        mSettings.createSubsamples();

        OGRE_ASSERT_LOW( ( vctLighting || mSettings.isRaster() ) &&
                         "vctLighting param must be provided when not using rasterization" );
        if( !mSettings.isRaster() )
            mVctLighting = vctLighting;
        else
            mVctLighting = 0;
        mFieldOrigin = fieldOrigin;
        mFieldSize = fieldSize;

        // Enlarge our bounds because at the borders we have
        // limited information thus there's often a hard line
        Vector3 probeBlockSize = mFieldSize / mSettings.getNumProbes3f();
        mFieldOrigin -= probeBlockSize;
        mFieldSize += probeBlockSize * 2.0f;

        mAlreadyWarned = false;
        mNumProbesProcessed = 0u;
        createTextures();
        setIrradianceFieldGenParams();

        if( mSettings.isRaster() )
        {
            if( !mIfRaster )
                mIfRaster = OGRE_NEW IrradianceFieldRaster( this );
            mIfRaster->createWorkspace();
        }
        else
        {
            delete mIfRaster;
            mIfRaster = 0;
        }
    }
    //-------------------------------------------------------------------------
    void IrradianceField::createTextures()
    {
        destroyTextures();

        TextureGpuManager *textureManager = mRoot->getRenderSystem()->getTextureGpuManager();

        mIrradianceTex = textureManager->createTexture(
            "IrradianceField" + StringConverter::toString( getId() ), GpuPageOutStrategy::Discard,
            TextureFlags::Uav, TextureTypes::Type2D );
        mDepthVarianceTex = textureManager->createTexture(
            "IrradianceFieldDepth" + StringConverter::toString( getId() ), GpuPageOutStrategy::Discard,
            TextureFlags::Uav, TextureTypes::Type2D );

        uint32 irradWidth, irradHeight;
        mSettings.getIrradProbeFullResolution( irradWidth, irradHeight );
        mIrradianceTex->setResolution( irradWidth, irradHeight );
        // Jahshaka (PHOTON-ENV-1): FLOAT, not upstream's R10G10B10A2_UNORM. The atlas
        // holds the probe's irradiance in the voxels' stored units (radiance over the
        // decode multiplier D_max / pi, D_max the brightest LIGHT), and since the sky
        // entered it - every escaping probe ray reads the environment - a sky
        // brighter than that ceiling (any sky in a scene with no lamp brighter than
        // it) clipped at 1.0. Half floats hold it; the cost is 4 bytes a texel here,
        // against the 8 the depth atlas gives back below.
        mIrradianceTex->setPixelFormat( PFG_RGBA16_FLOAT );

        uint32 depthWidth, depthHeight;
        mSettings.getDepthProbeFullResolution( depthWidth, depthHeight );
        mDepthVarianceTex->setResolution( depthWidth, depthHeight );
        // Upstream's two channels, the depth moments. (PHOTON-READER-1 widened it to
        // four for the probe rays' escape fraction, read by the pixel as a sky
        // visibility; PHOTON-ENV-1 put the sky itself into the irradiance atlas and
        // that channel lost its only reader.)
        mDepthVarianceTex->setPixelFormat( PFG_RG32_FLOAT );

        mIrradianceTex->scheduleTransitionTo( GpuResidency::Resident );
        mDepthVarianceTex->scheduleTransitionTo( GpuResidency::Resident );

        VaoManager *vaoManager = textureManager->getVaoManager();

        mDepthTapsIntegrationBuffer = setupIntegrationTaps(
            vaoManager, mSettings.mDepthProbeResolution, depthWidth, mDepthIntegrationJob,
            mIfGenParamsBuffer, mDepthMaxIntegrationTapsPerPixel );
        mColourTapsIntegrationBuffer = setupIntegrationTaps(
            vaoManager, mSettings.mIrradianceResolution, irradWidth, mColourIntegrationJob,
            mIfGenParamsBuffer, mColourMaxIntegrationTapsPerPixel );

        setupBorderMirrorParams( mSettings.getBorderedDepthResolution(), mDepthVarianceTex->getWidth(),
                                 mIfdDepthBorderMirrorParamsBuffer, mDepthMirrorBorderJob );
        setupBorderMirrorParams( mSettings.getBorderedIrradResolution(), mIrradianceTex->getWidth(),
                                 mIfdColourBorderMirrorParamsBuffer, mColourMirrorBorderJob );

        if( mDebugIfdProbeVisualizer )
        {
            setTextureToDebugVisualizer();
            mDebugIfdProbeVisualizer->setVisible( true );

            // Field AABB may have changed
            SceneNode *sceneNode = mDebugIfdProbeVisualizer->getParentSceneNode();
            sceneNode->setPosition( mFieldOrigin );
            sceneNode->setScale( mFieldSize / mSettings.getNumProbes3f() );
            sceneNode->getCreator()->notifyStaticDirty( sceneNode );
        }

        if( !mVctLighting )
            return;

        const size_t updateDataSize = sizeof( float ) * 4u * mSettings.mNumRaysPerPixel *
                                      mSettings.mDepthProbeResolution * mSettings.mDepthProbeResolution;
        float *directionsBuffer =
            reinterpret_cast<float *>( OGRE_MALLOC_SIMD( updateDataSize, MEMCATEGORY_GEOMETRY ) );
        FreeOnDestructor dataPtr( directionsBuffer );
        fillDirections( directionsBuffer );
        mDirectionsBuffer = vaoManager->createTexBuffer( PFG_RGBA32_FLOAT, updateDataSize, BT_DEFAULT,
                                                         directionsBuffer, false );

        mGenerationJob->setConstBuffer( 0, mIfGenParamsBuffer );

        bindChainToGenerationJob();

        CompositorManager2 *compositorManager = mRoot->getCompositorManager2();
        CompositorChannelVec channels;
        channels.push_back( mIrradianceTex );
        channels.push_back( mDepthVarianceTex );
        mGenerationWorkspace = compositorManager->addWorkspace( mSceneManager, channels, 0,
                                                                "IrradianceField/Gen/Workspace", false );
    }
    //-------------------------------------------------------------------------
    void IrradianceField::destroyTextures()
    {
        if( mDebugIfdProbeVisualizer )
            mDebugIfdProbeVisualizer->setVisible( false );

        TextureGpuManager *textureManager = mRoot->getRenderSystem()->getTextureGpuManager();

        if( mGenerationWorkspace )
        {
            CompositorManager2 *compositorManager = mRoot->getCompositorManager2();
            compositorManager->removeWorkspace( mGenerationWorkspace );
            mGenerationWorkspace = 0;
        }
        if( mIrradianceTex )
        {
            textureManager->destroyTexture( mIrradianceTex );
            mIrradianceTex = 0;
        }
        if( mDepthVarianceTex )
        {
            textureManager->destroyTexture( mDepthVarianceTex );
            mDepthVarianceTex = 0;
        }
        VaoManager *vaoManager = textureManager->getVaoManager();
        if( mDirectionsBuffer )
        {
            vaoManager->destroyTexBuffer( mDirectionsBuffer );
            mDirectionsBuffer = 0;
        }
        if( mDepthTapsIntegrationBuffer )
        {
            vaoManager->destroyTexBuffer( mDepthTapsIntegrationBuffer );
            mDepthTapsIntegrationBuffer = 0;
        }
        if( mColourTapsIntegrationBuffer )
        {
            vaoManager->destroyTexBuffer( mColourTapsIntegrationBuffer );
            mColourTapsIntegrationBuffer = 0;
        }
    }
    //-------------------------------------------------------------------------
    void IrradianceField::setFieldVolume( const Vector3 &fieldOrigin, const Vector3 &fieldSize )
    {
        mFieldOrigin = fieldOrigin;
        mFieldSize = fieldSize;

        // The same enlargement initialize() applies, for the same reason (limited
        // information at the borders), so that a moved field is placed exactly as a
        // field initialized at this volume would have been.
        Vector3 probeBlockSize = mFieldSize / mSettings.getNumProbes3f();
        mFieldOrigin -= probeBlockSize;
        mFieldSize += probeBlockSize * 2.0f;

        if( mDebugIfdProbeVisualizer )
        {
            SceneNode *sceneNode = mDebugIfdProbeVisualizer->getParentSceneNode();
            sceneNode->setPosition( mFieldOrigin );
            sceneNode->setScale( mFieldSize / mSettings.getNumProbes3f() );
            sceneNode->getCreator()->notifyStaticDirty( sceneNode );
        }

        // A raster field's probe cameras are derived from mFieldOrigin/mFieldSize on
        // every renderProbes() call, so there is nothing else to do for it; the voxel
        // source's probe-to-voxel transform lives in the generation params and must be
        // re-derived (the voxelizer may have moved too, which is the whole point).
        if( !mSettings.isRaster() && mVctLighting )
            setIrradianceFieldGenParams();
    }
    //-------------------------------------------------------------------------
    void IrradianceField::bindChainToGenerationJob()
    {
        OGRE_ASSERT_LOW( mVctLighting && !mSettings.isRaster() );

        // The job's texture units, in the order the shader declares them (the same
        // order VctLighting::setupBounceTextures binds the bounce job's): unit 0 the
        // directions buffer, then every cascade's isotropic volume, then - anisotropic
        // tiers - every cascade's X, every cascade's Y, every cascade's Z.
        const uint32 numCascades =
            std::min<uint32>( static_cast<uint32>( mVctLighting->getNumCascades() ),
                              kMaxChainCascades );
        const bool bIsAnisotropic = mVctLighting->isAnisotropic();
        const uint32 numVolumes = bIsAnisotropic ? 4u : 1u;

        const int32 numCascadesI32 = static_cast<int32>( numCascades );
        if( mGenerationJob->getProperty( "hlms_num_vct_cascades" ) != numCascadesI32 )
            mGenerationJob->setProperty( "hlms_num_vct_cascades", numCascadesI32 );
        const int32 anisoI32 = bIsAnisotropic ? 1 : 0;
        if( mGenerationJob->getProperty( "vct_anisotropic" ) != anisoI32 )
            mGenerationJob->setProperty( "vct_anisotropic", anisoI32 );

        // Jahshaka (PHOTON-ENV-1): + the environment cube, last, while the lighting
        // has one (the job property jah_env declares it).
        TextureGpu *envCube = mVctLighting->getEnvironmentCube();
        {
            const int32 envOn = envCube ? 1 : 0;
            if( mGenerationJob->getProperty( "jah_env" ) != envOn )
                mGenerationJob->setProperty( "jah_env", envOn );
        }
        const uint8 numTexUnits =
            static_cast<uint8>( 1u + numVolumes * numCascades + ( envCube ? 1u : 0u ) );
        if( mGenerationJob->getNumTexUnits() != numTexUnits )
            mGenerationJob->setNumTexUnits( numTexUnits );

        DescriptorSetTexture2::BufferSlot bufferSlot( DescriptorSetTexture2::BufferSlot::makeEmpty() );
        bufferSlot.buffer = mDirectionsBuffer;
        mGenerationJob->setTexBuffer( 0, bufferSlot );

        // ONE sampler, bound with the first volume (the shader declares one): the other
        // units carry no samplerblock, exactly as the bounce job's extra cascades do.
        DescriptorSetTexture2::TextureSlot texSlot( DescriptorSetTexture2::TextureSlot::makeEmpty() );
        uint8 unit = 1u;
        for( uint32 v = 0u; v < numVolumes; ++v )
        {
            for( uint32 c = 0u; c < numCascades; ++c )
            {
                texSlot.texture = mVctLighting->getLightVoxelTextures( c )[v];
                if( unit == 1u )
                {
                    mGenerationJob->setTexture( unit, texSlot,
                                                mVctLighting->getBindTrilinearSamplerblock() );
                }
                else
                    mGenerationJob->setTexture( unit, texSlot, 0, false );
                ++unit;
            }
        }
        if( envCube )
        {
            texSlot.texture = envCube;
            mGenerationJob->setTexture( unit, texSlot, 0, false );
        }
    }
    //-------------------------------------------------------------------------
    void IrradianceField::fillEnvironmentParams()
    {
        // In CASCADE 0's STORED units: the march returns the rays' colour in them and
        // the pixel decodes the atlas with cascade 0's multiplier, so the sky an
        // escaping ray adds is divided by the same number (the bounce job does the
        // same for its own volume, VctLighting::runBounce).
        const float finalMultiplier = mVctLighting->getFinalMultiplier();
        const float invFinal = finalMultiplier > 0.0f ? 1.0f / finalMultiplier : 0.0f;
        TextureGpu *envCube = mVctLighting->getEnvironmentCube();
        const float *gain = mVctLighting->getEnvironmentGain();
        const float *sh = mVctLighting->getEnvironmentSh();
        mIfGenParams.envGainMips =
            float4( Vector4( gain[0] * invFinal, gain[1] * invFinal, gain[2] * invFinal,
                             envCube ? Real( envCube->getNumMipmaps() ) : Real( 1 ) ) );
        for( size_t i = 0u; i < 9u; ++i )
        {
            mIfGenParams.envSh[i] = float4( Vector4( sh[i * 3u + 0u] * invFinal,
                                                     sh[i * 3u + 1u] * invFinal,
                                                     sh[i * 3u + 2u] * invFinal, 0.0f ) );
        }
    }
    //-------------------------------------------------------------------------
    void IrradianceField::fillChainParams()
    {
        const size_t numCascades = mVctLighting->getNumCascades();
        float invResMaxLod[4u * 16u];
        float fromPrev[8u * 16u];
        memset( invResMaxLod, 0, sizeof( invResMaxLod ) );
        memset( fromPrev, 0, sizeof( fromPrev ) );
        if( numCascades <= 16u )
            mVctLighting->getCascadeChainParams( invResMaxLod, fromPrev );
        const size_t n = std::min<size_t>( numCascades, kMaxChainCascades );
        for( size_t i = 0u; i < kMaxChainCascades; ++i )
        {
            const float *src = &invResMaxLod[4u * i];
            mIfGenParams.vctInvResMaxLod[i] = i < n ? float4( Vector4( src[0], src[1], src[2], src[3] ) )
                                                    : float4( Vector4::ZERO );
        }
        for( size_t i = 0u; i < ( kMaxChainCascades - 1u ) * 2u; ++i )
        {
            const float *src = &fromPrev[4u * i];
            mIfGenParams.vctFromPrev[i] = i < ( n - 1u ) * 2u ? float4( Vector4( src[0], src[1], src[2], src[3] ) )
                                                              : float4( Vector4::ZERO );
        }
    }
    //-------------------------------------------------------------------------
    void IrradianceField::setVctLighting( VctLighting *vctLighting )
    {
        if( mSettings.isRaster() || !vctLighting || !mGenerationJob )
            return;

        mVctLighting = vctLighting;

        // The bindings createTextures() made, re-made against the textures the lighting
        // owns now. Everything else it created (the atlases, the directions buffer, the
        // integration taps, the workspace) describes the FIELD and is unaffected.
        bindChainToGenerationJob();

        // The generation params carry the voxel volume's placement and the cone start
        // bias derived from its resolution: both belong to the lighting that was just
        // bound.
        setIrradianceFieldGenParams();
    }
    //-------------------------------------------------------------------------
    void IrradianceField::reset() { mNumProbesProcessed = 0u; }
    //-------------------------------------------------------------------------
    void IrradianceField::update( uint32 probesPerFrame )
    {
        const uint32 totalNumProbes = mSettings.getTotalNumProbes();
        if( mNumProbesProcessed >= totalNumProbes )
            return;

        IrradianceFieldGenParams *ifGenParams = reinterpret_cast<IrradianceFieldGenParams *>(
            mIfGenParamsBuffer->map( 0, mIfGenParamsBuffer->getNumElements() ) );

        probesPerFrame = std::min( totalNumProbes - mNumProbesProcessed, probesPerFrame );
        // OGRE_ASSERT_LOW( ( ( probesPerFrame & 0x01u ) == 0u ) && "probesPerFrame must be even!" );

        const uint32 numRaysPerIrradiancePixel = mSettings.getNumRaysPerIrradiancePixel();
        const uint32 threadsPerGroup = (uint32)alignToNextMultiple( 128u, numRaysPerIrradiancePixel );
        mGenerationJob->setThreadsPerGroup( threadsPerGroup, 1u, 1u );

        if( threadsPerGroup % 64u && !mAlreadyWarned )
        {
            LogManager::getSingleton().logMessage(
                "PERFORMANCE WARNING: mSettings.getNumRaysPerIrradiancePixel() is not a multiple of 64. "
                "This lowers the performance of IrradianceField::update. Tweak mDepthProbeResolution, "
                "mIrradianceResolution, or mNumRaysPerPixel until it is" );
            mAlreadyWarned = true;
        }

        const uint32 numRaysPerPixel = mSettings.mNumRaysPerPixel;
        const uint32 depthResolution = mSettings.mDepthProbeResolution;

        const uint32 numRays = probesPerFrame * depthResolution * depthResolution * numRaysPerPixel;

        OGRE_ASSERT_LOW( ( numRays % threadsPerGroup ) == 0u || mSettings.isRaster() );

        const uint32 numWorkGroups = numRays / threadsPerGroup;

        // There's a leftover the first dispatch is not currently handling,
        // i.e. numThreadGroupsX * numThreadGroupsY * threadsPerGroup != numRays
        // i.e. numIntegrationTGroupsY * numIntegrationTGroupsX != probesPerFrame
        TODO_handle_leftover;
        // Most GPUs allow up to 65535 thread groups per dimension
        const uint32 numThreadGroupsY = numWorkGroups / 65535u + 1u;
        const uint32 numThreadGroupsX = numWorkGroups / numThreadGroupsY;
        mGenerationJob->setNumThreadGroups( numThreadGroupsX, numThreadGroupsY, 1u );

        const uint32 numIntegrationTGroupsY = probesPerFrame / 65535u + 1u;
        const uint32 numIntegrationTGroupsX = probesPerFrame / numIntegrationTGroupsY;
        mDepthIntegrationJob->setNumThreadGroups( numIntegrationTGroupsX, numIntegrationTGroupsY, 1u );
        mColourIntegrationJob->setNumThreadGroups( numIntegrationTGroupsX, numIntegrationTGroupsY, 1u );

        if( !mSettings.isRaster() && mVctLighting )
        {
            // THE CHAIN AS IT IS NOW (Jahshaka, PHOTON-READER-1): every cascade's volumes
            // and placement, re-read per dispatch. The outer cascades scroll and rebuild
            // on their own schedule and nothing notifies the field; the bindings are
            // descriptor writes that are no-ops when nothing changed, and the parameters
            // are a few hundred bytes.
            bindChainToGenerationJob();
            fillChainParams();
            fillEnvironmentParams();
        }

        mIfGenParams.numProcessedProbes = mNumProbesProcessed;
        mIfGenParams.numProbes_threadsPerRow.w = numThreadGroupsX * threadsPerGroup;
        mIfGenParams.probesPerRow = numIntegrationTGroupsX * 1u;  // There's one probe per group
        *ifGenParams = mIfGenParams;

        mIfGenParamsBuffer->unmap( UO_KEEP_PERSISTENT );

        if( !mSettings.isRaster() )
        {
            mGenerationWorkspace->_beginUpdate( false );
            mGenerationWorkspace->_update();
            mGenerationWorkspace->_endUpdate( false );
        }
        else
        {
            mIfRaster->renderProbes( probesPerFrame );
        }

        mNumProbesProcessed += probesPerFrame;
    }
    //-------------------------------------------------------------------------
    size_t IrradianceField::getConstBufferSize() const
    {
        // THE LAST float4 USED TO BE MISSING. fillConstBufferData() writes an
        // IrradianceFieldRenderParams: a float4x3 (12 floats), then numProbesAggregated
        // plus two paddings (4), then the depth pair (4), then the IRRADIANCE pair (4) —
        // 24 floats, 96 bytes — and the struct the generated shader declares
        // (Hlms/Pbs/Any/IrradianceField_piece_ps.any) is the same 24. This function
        // returned 20, so HlmsPbs reserved 16 bytes too few for the pass buffer and
        // advanced its write pointer by 20 floats after a 24-float write: the next
        // occupant of the pass buffer (any HlmsListener, or nothing at all in the
        // samples, which simply overrun their map) lands on top of the irradiance
        // atlas parameters and every irradiance UV collapses onto texel 0 — DDGI goes
        // to an almost-black constant, with no error anywhere.
        return sizeof( float ) * ( 4u * 3u + 4u + 4u + 4u );
    }
    //-------------------------------------------------------------------------
    void IrradianceField::fillConstBufferData( const Matrix4 &viewMatrix,
                                               float *RESTRICT_ALIAS passBufferPtr ) const
    {
        struct IrradianceFieldRenderParams
        {
            float4x3 viewToIrradianceFieldRows;

            float2 numProbesAggregated;
            float padding0;
            float padding1;

            float depthBorderedRes;
            float depthFullWidth;
            float2 depthInvFullResolution;

            float irradBorderedRes;
            float irradFullWidth;
            float2 irradInvFullResolution;
        };

        const Vector3 numProbes( (Real)mSettings.mNumProbes[0],  //
                                 (Real)mSettings.mNumProbes[1],  //
                                 (Real)mSettings.mNumProbes[2] );
        const Vector3 finalSize = numProbes / mFieldSize;

        Matrix4 xform;
        xform.makeTransform( -mFieldOrigin * finalSize, finalSize, Quaternion::IDENTITY );
        xform = xform.concatenateAffine( viewMatrix.inverseAffine() );

        const float fDepthFullWidth = static_cast<float>( mDepthVarianceTex->getWidth() );
        const float fDepthFullHeight = static_cast<float>( mDepthVarianceTex->getHeight() );

        const float fIrradFullWidth = static_cast<float>( mIrradianceTex->getWidth() );
        const float fIrradFullHeight = static_cast<float>( mIrradianceTex->getHeight() );

        IrradianceFieldRenderParams *RESTRICT_ALIAS renderParams =
            reinterpret_cast<IrradianceFieldRenderParams * RESTRICT_ALIAS>( passBufferPtr );

        renderParams->viewToIrradianceFieldRows = xform;
        renderParams->numProbesAggregated.x = numProbes.x;
        renderParams->numProbesAggregated.y = numProbes.x * numProbes.y;
        renderParams->padding0 = 0;
        renderParams->padding1 = 0;

        renderParams->depthBorderedRes = static_cast<float>( mSettings.getBorderedDepthResolution() );
        renderParams->depthFullWidth = fDepthFullWidth;
        renderParams->depthInvFullResolution.x = 1.0f / fDepthFullWidth;
        renderParams->depthInvFullResolution.y = 1.0f / fDepthFullHeight;

        renderParams->irradBorderedRes = static_cast<float>( mSettings.getBorderedIrradResolution() );
        renderParams->irradFullWidth = fIrradFullWidth;
        renderParams->irradInvFullResolution.x = 1.0f / fIrradFullWidth;
        renderParams->irradInvFullResolution.y = 1.0f / fIrradFullHeight;
    }
    //-------------------------------------------------------------------------
    void IrradianceField::setDebugVisualization( DebugVisualizationMode mode, SceneManager *sceneManager,
                                                 uint8 tessellation )
    {
        if( mDebugIfdProbeVisualizer )
        {
            SceneNode *sceneNode = mDebugIfdProbeVisualizer->getParentSceneNode();
            sceneNode->getParentSceneNode()->removeAndDestroyChild( sceneNode );
            OGRE_DELETE mDebugIfdProbeVisualizer;
            mDebugIfdProbeVisualizer = 0;
        }

        mDebugVisualizationMode = mode;
        mDebugTessellation = tessellation;

        if( mode != DebugVisualizationNone )
        {
            SceneNode *rootNode = sceneManager->getRootSceneNode( SCENE_STATIC );
            SceneNode *visNode = rootNode->createChildSceneNode( SCENE_STATIC );

            mDebugIfdProbeVisualizer = OGRE_NEW IfdProbeVisualizer(
                Ogre::Id::generateNewId<Ogre::MovableObject>(),
                &sceneManager->_getEntityMemoryManager( SCENE_STATIC ), sceneManager, 0u );

            setTextureToDebugVisualizer();

            visNode->setPosition( mFieldOrigin );
            visNode->setScale( mFieldSize / mSettings.getNumProbes3f() );
            visNode->attachObject( mDebugIfdProbeVisualizer );
        }
    }
    //-------------------------------------------------------------------------
    bool IrradianceField::getDebugVisualizationMode() const { return mDebugVisualizationMode; }
    //-------------------------------------------------------------------------
    uint8 IrradianceField::getDebugTessellation() const { return mDebugTessellation; }
    //-------------------------------------------------------------------------
    void IrradianceField::setTextureToDebugVisualizer()
    {
        TextureGpu *trackedTex =
            mDebugVisualizationMode == DebugVisualizationColour ? mIrradianceTex : mDepthVarianceTex;
        const uint8 borderedRes = mDebugVisualizationMode == DebugVisualizationColour
                                      ? mSettings.getBorderedIrradResolution()
                                      : mSettings.getBorderedDepthResolution();
        Vector2 rangeMult( 1.0f );
        if( mDebugVisualizationMode == DebugVisualizationDepth )
        {
            // TODO: Find something better than a hardcoded 500
            rangeMult.x = 500.0f;
            rangeMult.y = rangeMult.x * rangeMult.x;
        }
        rangeMult = 2.0f / rangeMult;
        mDebugIfdProbeVisualizer->setTrackingIfd( mSettings, mFieldSize, borderedRes, trackedTex,
                                                  rangeMult, mDebugTessellation );
    }
}  // namespace Ogre
