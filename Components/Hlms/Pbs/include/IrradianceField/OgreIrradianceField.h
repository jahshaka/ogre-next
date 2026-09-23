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

#ifndef _OgreIrradianceField_H_
#define _OgreIrradianceField_H_

#include "OgreHlmsPbsPrerequisites.h"

#include "Math/Simple/OgreAabb.h"
#include "OgreId.h"
#include "OgreIdString.h"
#include "OgrePixelFormatGpu.h"
#include "OgreShaderPrimitives.h"

#include "ogrestd/vector.h"

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class IrradianceFieldRaster;
    class IfdProbeVisualizer;

    struct _OgreHlmsPbsExport RasterParams
    {
        IdString       mWorkspaceName;
        PixelFormatGpu mPixelFormat;
        float          mCameraNear;
        float          mCameraFar;

        RasterParams();
    };

    struct _OgreHlmsPbsExport IrradianceFieldSettings
    {
        /** Number of rays per pixel in terms of mDepthProbeResolution.

            e.g. if mNumRaysPerPixel = 4, mDepthProbeResolution = 16 and mIrradianceResolution = 8,
            then the depth texture has 4 rays per pixel, but the irradiance texture will have
            16 rays per pixel
        */
        uint16 mNumRaysPerPixel;
        /// Square resolution of a single probe, depth variance, e.g. 8u means each probe is 8x8.
        uint8 mDepthProbeResolution;
        /// Square resolution of a single probe, irradiance e.g. 4u means each probe is 4x4.
        /// Must be mIrradianceResolution <= mDepthProbeResolution
        /// mDepthProbeResolution must be multiple of mIrradianceResolution
        uint8 mIrradianceResolution;

        /// Number of probes in all three XYZ axes.
        /// Must be power of two
        uint32 mNumProbes[3];

        /// Use rasterization to generate light & depth data, instead of voxelization
        RasterParams mRasterParams;

    protected:
        /// mSubsamples.size() == mNumRaysPerPixel
        /// Contains the offsets for generating the rays
        vector<Vector2>::type mSubsamples;

    public:
        IrradianceFieldSettings();

        void testValidity()
        {
            OGRE_ASSERT_LOW( mIrradianceResolution <= mDepthProbeResolution );
            OGRE_ASSERT_LOW( ( mDepthProbeResolution % mIrradianceResolution ) == 0u );
            for( size_t i = 0u; i < 3u; ++i )
            {
                OGRE_ASSERT_LOW( ( mNumProbes[i] & ( mNumProbes[i] - 1u ) ) == 0u &&
                                 "Num probes must be a power of 2" );
            }
        }

        bool isRaster() const;

        void createSubsamples();

        uint32 getTotalNumProbes() const;

        void getDepthProbeFullResolution( uint32 &outWidth, uint32 &outHeight ) const;
        void getIrradProbeFullResolution( uint32 &outWidth, uint32 &outHeight ) const;

        /// Returns mIrradianceResolution + 2u, since we need to reserverve 1 pixel border
        /// around the probe for proper interpolation.
        /// This means a 8x8 probe actually occupies 10x10.
        uint8 getBorderedIrradResolution() const;
        uint8 getBorderedDepthResolution() const;

        uint32 getNumRaysPerIrradiancePixel() const;

        Vector3 getNumProbes3f() const;

        const vector<Vector2>::type &getSubsamples() const { return mSubsamples; }
    };

    /**
    @class IrradianceField
        Implements an Irradiance Field with Depth, inspired on DDGI

        We use the voxelized results from VCT.
        Afterwards once we have Raytracing, we'll also allow to use Raytracing instead;
        since both are very similars (with VCT we shoot cones instead of rays)

    @see
        Snippet taken from Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields
        Zander Majercik, NVIDIA; Jean-Philippe Guertin, Université de Montréal;
        Derek Nowrouzezahrai, McGill University; Morgan McGuire, NVIDIA and McGill University
        http://jcgt.org/published/0008/02/01/

    @see
        https://github.com/OGRECave/ogre-next/issues/29
    */
    class _OgreHlmsPbsExport IrradianceField : public IdObject
    {
        friend class IrradianceFieldRaster;

    public:
        enum DebugVisualizationMode
        {
            DebugVisualizationColour,
            DebugVisualizationDepth,
            DebugVisualizationNone
        };

    public:
        /// The longest cascade chain the generation job walks (VctLighting supports
        /// fewer than this). The shader's parameter block is sized by it.
        static const uint32 kMaxChainCascades = 8u;

    protected:
        struct IrradianceFieldGenParams
        {
            float  invNumRaysPerPixel;
            float  invNumRaysPerIrradiancePixel;
            float  unused0;
            uint32 probesPerRow;  // Used by integration CS

            float  coneAngleTan;
            uint32 numProcessedProbes;
            // Jahshaka (PHOTON-READER-1): these two were the cone march's start bias
            // and its inverse, derived from cascade 0 alone. The generation job now
            // marches the whole cascade chain through the one voxel reader, which
            // takes each cascade's own inverse resolution from the chain block below.
            float unused1;
            float unused2;

            // float invFieldResolution;
            uint4 numProbes_threadsPerRow;

            float4x4 irrProbeToVctTransform;

            // Jahshaka (PHOTON-READER-1): THE CASCADE CHAIN the probe rays walk, in the
            // layout VctLighting::getCascadeChainParams writes and the pixel shader's
            // pass buffer carries. Appended, so the integration job's declaration of
            // this buffer (the members above) is still a prefix of it.
            float4 vctInvResMaxLod[kMaxChainCascades];
            float4 vctFromPrev[( kMaxChainCascades - 1u ) * 2u];
        };

        struct IfdBorderMirrorParams
        {
            uint32 probeBorderedRes;
            uint32 numPixelsInEdges;
            uint32 numTopBottomPixels;
            uint32 numGlobalThreadsForEdges;

            uint32 maxThreadId;
            uint32 threadsPerThreadRow;
            uint32 padding[2];
        };

        IrradianceFieldSettings mSettings;
        /// Number of probes processed so far.
        /// We process the entire field across multiple frames.
        uint32 mNumProbesProcessed;

        Vector3 mFieldOrigin;
        Vector3 mFieldSize;

        uint32 mDepthMaxIntegrationTapsPerPixel;
        uint32 mColourMaxIntegrationTapsPerPixel;

        VctLighting *mVctLighting;

        TextureGpu *mIrradianceTex;
        TextureGpu *mDepthVarianceTex;

        CompositorWorkspace *mGenerationWorkspace;
        HlmsComputeJob      *mGenerationJob;
        HlmsComputeJob      *mDepthIntegrationJob;
        HlmsComputeJob      *mColourIntegrationJob;
        HlmsComputeJob      *mDepthMirrorBorderJob;
        HlmsComputeJob      *mColourMirrorBorderJob;

        IrradianceFieldGenParams mIfGenParams;
        ConstBufferPacked       *mIfGenParamsBuffer;
        TexBufferPacked         *mDirectionsBuffer;
        TexBufferPacked         *mDepthTapsIntegrationBuffer;
        TexBufferPacked         *mColourTapsIntegrationBuffer;
        ConstBufferPacked       *mIfdDepthBorderMirrorParamsBuffer;
        ConstBufferPacked       *mIfdColourBorderMirrorParamsBuffer;

        IrradianceFieldRaster *mIfRaster;

        DebugVisualizationMode mDebugVisualizationMode;
        uint8                  mDebugTessellation;
        IfdProbeVisualizer    *mDebugIfdProbeVisualizer;

        Root         *mRoot;
        SceneManager *mSceneManager;
        bool          mAlreadyWarned;

        void fillDirections( float *RESTRICT_ALIAS outBuffer );

        static TexBufferPacked *setupIntegrationTaps( VaoManager *vaoManager, uint32 probeRes,
                                                      uint32 fullWidth, HlmsComputeJob *integrationJob,
                                                      ConstBufferPacked *ifGenParamsBuffer,
                                                      uint32            &outMaxIntegrationTapsPerPixel );
        static uint32           countNumIntegrationTaps( uint32 probeRes );

        /**
         * @brief fillIntegrationWeights
        @param outBuffer
            Buffer must NOT be write-combined because we write and then read back from it
        */
        static void fillIntegrationWeights( float2 *RESTRICT_ALIAS outBuffer, uint32 probeRes,
                                            uint32 maxTapsPerPixel );
        void        setIrradianceFieldGenParams();

        void setupBorderMirrorParams( uint32 borderedRes, uint32 fullWidth,
                                      ConstBufferPacked *ifdBorderMirrorParamsBuffer,
                                      HlmsComputeJob    *job );

        void setTextureToDebugVisualizer();

        /// Jahshaka (PHOTON-READER-1): binds EVERY cascade's light volumes to the
        /// generation job, from the pointers the chain holds NOW. Called before every
        /// dispatch: a cascade re-creates its volumes on a rebuild or a bounce
        /// ping-pong without telling its readers (the bounce job re-asserts its own
        /// bindings per dispatch for the same reason).
        void bindChainToGenerationJob();
        /// ...and the chain's parameters, from VctLighting's one definition of them.
        void fillChainParams();

    public:
        IrradianceField( Root *root, SceneManager *sceneManager );
        ~IrradianceField();

        void createTextures();
        void destroyTextures();

        /**
        @brief initialize
        @param settings
        @param fieldOrigin
        @param fieldSize
        @param vctLighting
            This value is ignored if IrradianceFieldSettings::usesRaster() returns true,
            and must be non-null if it returns false
         */
        void initialize( const IrradianceFieldSettings &settings, const Vector3 &fieldOrigin,
                         const Vector3 &fieldSize, VctLighting *vctLighting );

        /** Moves (and/or resizes) the field's volume WITHOUT re-creating its textures.

            initialize() is the only way to place the field upstream, and it calls
            createTextures(), which destroys and re-creates both atlases: a field that
            must follow the camera (e.g. one riding the innermost cascade of a
            camera-centred cascade chain) would therefore be born black on every step and
            re-converge from nothing, and its compositor workspace, directions buffer and
            integration taps would be rebuilt for a change none of them depend on.

            Everything that reads the placement reads it LIVE — fillConstBufferData()
            rebuilds the pixel transform per pass, IrradianceFieldRaster::renderProbes()
            derives each probe camera from it, and the generation params below hold the
            probe-to-voxel transform — so moving the volume is exactly these two members
            plus a re-derivation of those params.

            The probe COUNTS are settings and are not touched, so the atlases stay valid.
            The atlases keep the irradiance integrated at the PREVIOUS placement: the
            caller decides whether to reset() and re-converge progressively over it or to
            converge the whole field in one update() before the next pass reads it.
        @remarks
            Must not be called before initialize().
        @param fieldOrigin
            The volume's origin, in the same units and with the same meaning initialize()
            gives it (it is enlarged by one probe block per side here too).
        @param fieldSize
            The volume's size, same meaning as initialize()'s.
        */
        void setFieldVolume( const Vector3 &fieldOrigin, const Vector3 &fieldSize );

        /** Re-points the field at a VctLighting, and re-binds the generation job to the
            light voxel textures that object owns RIGHT NOW.

            VctLighting re-creates its light voxel textures whenever its voxelizer's
            textures change — on a LostResidency/Deleted notification (checkTextures()),
            and, since VctLighting::setVoxelizer() exists, whenever it is moved to a
            replacement voxelizer. The IrradianceField bound those textures ONCE, in
            initialize(), by pointer: after any such re-creation its generation job holds
            pointers to destroyed textures and the next update() integrates from freed
            GPU memory (or from whatever the allocator has since handed the address to).
            There is no notification from VctLighting to its readers, so this is the hook
            that answers it; passing the SAME VctLighting is meaningful and re-binds.

            Cheaper than initialize() and it keeps the atlases: nothing about the field's
            geometry has changed, only where its rays read radiance from.
        @remarks
            Must not be called before initialize(), nor on a raster-sourced field (which
            has no VctLighting and no generation job bindings to refresh) — both are no-ops.
        */
        void setVctLighting( VctLighting *vctLighting );

        /// If VctLighting was updated with minor changes (e.g. light position/direction changed,
        /// number of bounces setting changed) then call this function so update() process it
        /// again.
        ///
        /// If major changes happens to VctLighting, then call initialize() again
        void reset();

        void update( uint32 probesPerFrame = 200u );

        size_t getConstBufferSize() const;
        void fillConstBufferData( const Matrix4 &viewMatrix, float *RESTRICT_ALIAS passBufferPtr ) const;

        /**
        @param mode
        @param sceneManager
            Can be nullptr only if mode == IrradianceField::DebugVisualizationNone
        @param tessellation
            Value in range [3; 16]
            Note this value increases exponentially:
                tessellation = 3u -> 24 vertices (per sphere)
                tessellation = 4u -> 112 vertices
                tessellation = 5u -> 480 vertices
                tessellation = 6u -> 1984 vertices
                tessellation = 7u -> 8064 vertices
                tessellation = 8u -> 32512 vertices
                tessellation = 9u -> 130560 vertices
                tessellation = 16u -> 2.147.418.112 vertices
        */
        void  setDebugVisualization( IrradianceField::DebugVisualizationMode mode,
                                     SceneManager *sceneManager, uint8 tessellation );
        bool  getDebugVisualizationMode() const;
        uint8 getDebugTessellation() const;

        TextureGpu *getIrradianceTex() const { return mIrradianceTex; }
        TextureGpu *getDepthVarianceTex() const { return mDepthVarianceTex; }
    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
