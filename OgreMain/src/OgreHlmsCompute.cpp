/*
-----------------------------------------------------------------------------
This source file is part of OGRE-Next
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2014 Torus Knot Software Ltd

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

#include "OgreHlmsCompute.h"

#include "CommandBuffer/OgreCommandBuffer.h"
#include "Compositor/OgreCompositorShadowNode.h"
#include "Hash/MurmurHash3.h"
#include "OgreFileSystem.h"
#include "OgreHighLevelGpuProgram.h"
#include "OgreHighLevelGpuProgramManager.h"
#include "OgreHlmsAsyncCompiler.h"
#include "OgreHlmsComputeJob.h"
#include "OgreHlmsManager.h"
#include "OgreLogManager.h"
#include "OgreRootLayout.h"
#include "OgreSceneManager.h"
#include "OgreRenderQueue.h"
#include "Vao/OgreConstBufferPacked.h"
#include "Vao/OgreTexBufferPacked.h"
#include "Vao/OgreUavBufferPacked.h"

#if OGRE_ARCH_TYPE == OGRE_ARCHITECTURE_32
#    define OGRE_HASH128_FUNC MurmurHash3_x86_128
#else
#    define OGRE_HASH128_FUNC MurmurHash3_x64_128
#endif

#include <fstream>

namespace Ogre
{
    const IdString ComputeProperty::ThreadsPerGroupX = IdString( "threads_per_group_x" );
    const IdString ComputeProperty::ThreadsPerGroupY = IdString( "threads_per_group_y" );
    const IdString ComputeProperty::ThreadsPerGroupZ = IdString( "threads_per_group_z" );
    const IdString ComputeProperty::NumThreadGroupsX = IdString( "num_thread_groups_x" );
    const IdString ComputeProperty::NumThreadGroupsY = IdString( "num_thread_groups_y" );
    const IdString ComputeProperty::NumThreadGroupsZ = IdString( "num_thread_groups_z" );

    const IdString ComputeProperty::TypedUavLoad = IdString( "typed_uav_load" );

    const IdString ComputeProperty::NumTextureSlots = IdString( "num_texture_slots" );
    const IdString ComputeProperty::MaxTextureSlot = IdString( "max_texture_slot" );
    const char *ComputeProperty::Texture = "texture";

    const IdString ComputeProperty::NumUavSlots = IdString( "num_uav_slots" );
    const IdString ComputeProperty::MaxUavSlot = IdString( "max_uav_slot" );
    const char *ComputeProperty::Uav = "uav";

    // Must be sorted from best to worst
    const String BestD3DComputeShaderTargets[3] = { "cs_5_0", "cs_4_1", "cs_4_0" };

    bool HlmsCompute::msInAsyncWorkspace = false;
    //-----------------------------------------------------------------------------------
    HlmsCompute::HlmsCompute( AutoParamDataSource *autoParamDataSource ) :
        Hlms( HLMS_COMPUTE, "compute", 0, 0 ),
        mAutoParamDataSource( autoParamDataSource ),
        mComputeShaderTarget( 0 ),
        mAsyncProgramCounter( 0u ),
        mDeferredDispatch( false ),
        mNumDeferredDispatches( 0u )
    {
    }
    //-----------------------------------------------------------------------------------
    HlmsCompute::~HlmsCompute()
    {
        destroyAllComputeJobs();
        if( mHlmsManager )
        {
            mHlmsManager->unregisterComputeHlms();
            mHlmsManager = 0;
        }
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::_changeRenderSystem( RenderSystem *newRs )
    {
        Hlms::_changeRenderSystem( newRs );

        if( mRenderSystem )
        {
            const RenderSystemCapabilities *capabilities = mRenderSystem->getCapabilities();

            if( mShaderProfile == "hlsl" || mShaderProfile == "hlslvk" )
            {
                for( size_t j = 0; j < 3 && !mComputeShaderTarget; ++j )
                {
                    if( capabilities->isShaderProfileSupported( BestD3DComputeShaderTargets[j] ) )
                        mComputeShaderTarget = &BestD3DComputeShaderTargets[j];
                }
            }
        }
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::processPieces( const StringVector &pieceFiles )
    {
        ResourceGroupManager &resourceGroupMgr = ResourceGroupManager::getSingleton();

        StringVector::const_iterator itor = pieceFiles.begin();
        StringVector::const_iterator endt = pieceFiles.end();

        while( itor != endt )
        {
            String filename = *itor;

            // If it has an explicit extension, only open it if it matches the current
            // render system's. If it doesn't, then we add it ourselves.
            String::size_type pos = filename.find_last_of( '.' );
            if( pos == String::npos ||
                ( filename.compare( pos + 1, String::npos, mShaderFileExt ) != 0 &&
                  filename.compare( pos + 1, String::npos, "any" ) != 0 &&
                  filename.compare( pos + 1, String::npos, "metal" ) != 0 &&
                  filename.compare( pos + 1, String::npos, "glsl" ) != 0 &&
                  filename.compare( pos + 1, String::npos, "hlsl" ) != 0 ) )
            {
                filename += mShaderFileExt;
            }

            DataStreamPtr inFile = resourceGroupMgr.openResource( filename );

            String inString;
            String outString;

            inString.resize( inFile->size() );
            inFile->read( &inString[0], inFile->size() );

            this->parseMath( inString, outString, kNoTid );
            while( outString.find( "@foreach" ) != String::npos )
            {
                this->parseForEach( outString, inString, kNoTid );
                inString.swap( outString );
            }
            this->parseProperties( outString, inString, kNoTid );
            this->parseUndefPieces( inString, outString, kNoTid );
            this->collectPieces( outString, inString, kNoTid );
            this->parseCounter( inString, outString, kNoTid );

            ++itor;
        }
    }
    //-----------------------------------------------------------------------------------
    HlmsComputePso HlmsCompute::compileShader( HlmsComputeJob *job, uint32 finalHash )
    {
        // Assumes mSetProperties is already set
        // mSetProperties[kNoTid].clear();
        {
            // Add RenderSystem-specific properties
            IdStringVec::const_iterator itor = mRsSpecificExtensions.begin();
            IdStringVec::const_iterator endt = mRsSpecificExtensions.end();

            while( itor != endt )
                setProperty( kNoTid, *itor++, 1 );
        }

        GpuProgramPtr shader;
        // Generate the shader

        // Collect pieces
        mT[kNoTid].pieces.clear();

        // Start with the pieces sent by the user
        mT[kNoTid].pieces = job->mPieces;

        const String sourceFilename = job->mSourceFilename + mShaderFileExt;

        ResourceGroupManager &resourceGroupMgr = ResourceGroupManager::getSingleton();
        DataStreamPtr inFile = resourceGroupMgr.openResource( sourceFilename );

        if( mShaderProfile == "glsl" || mShaderProfile == "glslvk" )  // TODO: String comparision
        {
            setProperty( kNoTid, HlmsBaseProp::GL3Plus,
                         mRenderSystem->getNativeShadingLanguageVersion() );
        }

        setProperty( kNoTid, HlmsBaseProp::Syntax, static_cast<int32>( mShaderSyntax.getU32Value() ) );
        setProperty( kNoTid, HlmsBaseProp::Hlsl,
                     static_cast<int32>( HlmsBaseProp::Hlsl.getU32Value() ) );
        setProperty( kNoTid, HlmsBaseProp::Glsl,
                     static_cast<int32>( HlmsBaseProp::Glsl.getU32Value() ) );
        setProperty( kNoTid, HlmsBaseProp::Glslvk,
                     static_cast<int32>( HlmsBaseProp::Glslvk.getU32Value() ) );
        setProperty( kNoTid, HlmsBaseProp::Hlslvk,
                     static_cast<int32>( HlmsBaseProp::Hlslvk.getU32Value() ) );
        setProperty( kNoTid, HlmsBaseProp::Metal,
                     static_cast<int32>( HlmsBaseProp::Metal.getU32Value() ) );

#if OGRE_PLATFORM == OGRE_PLATFORM_APPLE_IOS
        setProperty( kNoTid, HlmsBaseProp::iOS, 1 );
#endif
#if OGRE_PLATFORM == OGRE_PLATFORM_APPLE
        setProperty( kNoTid, HlmsBaseProp::macOS, 1 );
#endif
        setProperty( kNoTid, HlmsBaseProp::Full32,
                     static_cast<int32>( HlmsBaseProp::Full32.getU32Value() ) );
        setProperty( kNoTid, HlmsBaseProp::Midf16,
                     static_cast<int32>( HlmsBaseProp::Midf16.getU32Value() ) );
        setProperty( kNoTid, HlmsBaseProp::Relaxed,
                     static_cast<int32>( HlmsBaseProp::Relaxed.getU32Value() ) );
        setProperty( kNoTid, HlmsBaseProp::PrecisionMode, getSupportedPrecisionModeHash() );

        if( mFastShaderBuildHack )
            setProperty( kNoTid, HlmsBaseProp::FastShaderBuildHack, 1 );

        // Piece files
        processPieces( job->mIncludedPieceFiles );

        String inString;
        String outString;

        inString.resize( inFile->size() );
        inFile->read( &inString[0], inFile->size() );

        bool syntaxError = false;

        syntaxError |= this->parseMath( inString, outString, kNoTid );
        while( !syntaxError && outString.find( "@foreach" ) != String::npos )
        {
            syntaxError |= this->parseForEach( outString, inString, kNoTid );
            inString.swap( outString );
        }
        syntaxError |= this->parseProperties( outString, inString, kNoTid );
        syntaxError |= this->parseUndefPieces( inString, outString, kNoTid );
        while( !syntaxError && ( outString.find( "@piece" ) != String::npos ||
                                 outString.find( "@insertpiece" ) != String::npos ) )
        {
            syntaxError |= this->collectPieces( outString, inString, kNoTid );
            syntaxError |= this->insertPieces( inString, outString, kNoTid );
        }
        syntaxError |= this->parseCounter( outString, inString, kNoTid );

        outString.swap( inString );

        if( syntaxError )
        {
            LogManager::getSingleton().logMessage( "There were HLMS syntax errors while parsing " +
                                                   StringConverter::toString( finalHash ) +
                                                   job->mSourceFilename + mShaderFileExt );
        }

        String debugFilenameOutput;

        if( mDebugOutput )
        {
            debugFilenameOutput = mOutputPath + "./" + StringConverter::toString( finalHash ) +
                                  job->mSourceFilename + mShaderFileExt;
            std::ofstream outFile( Ogre::fileSystemPathFromString( debugFilenameOutput ).c_str(),
                                   std::ios::out | std::ios::binary );
            if( mDebugOutputProperties )
                dumpProperties( outFile, kNoTid );
            outFile.write( &outString[0], static_cast<std::streamsize>( outString.size() ) );
        }

        // Don't create and compile if template requested not to
        if( !getProperty( kNoTid, HlmsBaseProp::DisableStage ) )
        {
            // Very similar to what the GpuProgramManager does with its microcode cache,
            // but we **need** to know if two Compute Shaders share the same source code.
            Hash hashVal;
            OGRE_HASH128_FUNC( outString.c_str(), static_cast<int>( outString.size() ), IdString::Seed,
                               &hashVal );

            const RenderSystemCapabilities *capabilities = mRenderSystem->getCapabilities();

            if( capabilities->hasCapability( RSC_EXPLICIT_API ) )
            {
                // If two shaders have the exact same source code but different
                // Root Layout, we should treat them differently
                RootLayout rootLayout;
                // We MUST memset due to internal padding;
                // otherwise we'll be hashing uninitialized values
                memset( &rootLayout, 0, sizeof( rootLayout ) );
                rootLayout.mCompute = true;
                job->setupRootLayout( rootLayout );

                Hash hashValTmp[2] = { hashVal, Hash() };
                OGRE_HASH128_FUNC( &rootLayout, sizeof( rootLayout ), IdString::Seed, &hashValTmp[1] );
                OGRE_HASH128_FUNC( hashValTmp, sizeof( hashValTmp ), IdString::Seed, &hashVal );
            }

            // ASYNC-SHADERS-1: the asynchronous compiler's threads read and add to this map.
            bool bCached = false;
            {
                ScopedLock lock( mMutex );
                CompiledShaderMap::const_iterator itor = mCompiledShaderCache.find( hashVal );
                if( itor != mCompiledShaderCache.end() )
                {
                    shader = itor->second;
                    bCached = true;
                }
            }
            if( !bCached )
            {
                HighLevelGpuProgramManager *gpuProgramManager =
                    HighLevelGpuProgramManager::getSingletonPtr();

                HighLevelGpuProgramPtr gp = gpuProgramManager->createProgram(
                    StringConverter::toString( finalHash ) + job->mSourceFilename,
                    ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME, mShaderProfile,
                    GPT_COMPUTE_PROGRAM );
                gp->setSource( outString, debugFilenameOutput );

                {
                    RootLayout rootLayout;
                    rootLayout.mCompute = true;
                    job->setupRootLayout( rootLayout );
                    gp->setRootLayout( gp->getType(), rootLayout );
                    if( getProperty( kNoTid, "uses_array_bindings" ) )
                        gp->setAutoReflectArrayBindingsInRootLayout( true );
                }

                if( mComputeShaderTarget )
                {
                    // D3D-specific
                    gp->setParameter( "target", *mComputeShaderTarget );
                    gp->setParameter( "entry_point", "main" );
                }

                gp->setSkeletalAnimationIncluded( getProperty( kNoTid, HlmsBaseProp::Skeleton ) != 0 );
                gp->setMorphAnimationIncluded( false );
                gp->setPoseAnimationIncluded( getProperty( kNoTid, HlmsBaseProp::Pose ) != 0 );
                gp->setVertexTextureFetchRequired( false );

                gp->load();

                shader = gp;

                ScopedLock lock( mMutex );
                mCompiledShaderCache[hashVal] = shader;
            }
        }

        // Reset the disable flag.
        setProperty( kNoTid, HlmsBaseProp::DisableStage, 0 );

        HlmsComputePso pso;
        pso.initialize();
        pso.computeShader = shader;
        pso.computeParams = shader->createParameters();
        pso.mThreadsPerGroup[0] = (uint32)( getProperty( kNoTid, ComputeProperty::ThreadsPerGroupX ) );
        pso.mThreadsPerGroup[1] = (uint32)( getProperty( kNoTid, ComputeProperty::ThreadsPerGroupY ) );
        pso.mThreadsPerGroup[2] = (uint32)( getProperty( kNoTid, ComputeProperty::ThreadsPerGroupZ ) );
        pso.mNumThreadGroups[0] = (uint32)( getProperty( kNoTid, ComputeProperty::NumThreadGroupsX ) );
        pso.mNumThreadGroups[1] = (uint32)( getProperty( kNoTid, ComputeProperty::NumThreadGroupsY ) );
        pso.mNumThreadGroups[2] = (uint32)( getProperty( kNoTid, ComputeProperty::NumThreadGroupsZ ) );

        // Jahshaka fork 1bccc3f93+a98e2b0af (was 0032): a job dispatched INDIRECTLY has no CPU-side group count by
        // definition — the whole point is that only the GPU knows it — so requiring one here
        // would force every such job to carry a meaningless dummy. The threads per group are
        // still required (Metal needs them on the C++ side, and both back ends bake them into
        // the shader), so only the second half of the test is relaxed.
        const bool bIndirect = job->mIndirectDispatchBuffer != 0;
        if( pso.mThreadsPerGroup[0] * pso.mThreadsPerGroup[1] * pso.mThreadsPerGroup[2] == 0u ||
            ( !bIndirect &&
              pso.mNumThreadGroups[0] * pso.mNumThreadGroups[1] * pso.mNumThreadGroups[2] == 0u ) )
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                         job->getNameStr() +
                             ": Shader or C++ must set threads_per_group_x, threads_per_group_y & "
                             "threads_per_group_z and num_thread_groups_x through num_thread_groups_z."
                             " Otherwise we can't run on Metal. Use @pset( threads_per_group_x, 512 );"
                             " or read the value using @value( threads_per_group_x ) if you've already"
                             " set it from C++ or the JSON material",
                         "HlmsCompute::compileShader" );
        }

        ShaderParams *shaderParams = job->_getShaderParams( "default" );
        if( shaderParams )
            shaderParams->updateParameters( pso.computeParams, true );

        shaderParams = job->_getShaderParams( mShaderProfile );
        if( shaderParams )
            shaderParams->updateParameters( pso.computeParams, true );

        mRenderSystem->_hlmsComputePipelineStateObjectCreated( &pso );

        return pso;
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::destroyComputeJob( IdString name )
    {
        HlmsComputeJobMap::iterator itor = mComputeJobs.find( name );

        if( itor != mComputeJobs.end() )
        {
            HlmsComputeJob *job = itor->second.computeJob;
            // ASYNC-SHADERS-1: nothing may build a permutation of a job that is going away.
            if( mHlmsManager )
                mHlmsManager->getAsyncCompiler()->cancelKey( job );
            for( size_t i = 0u; i < mAsyncComputeEntries.size(); )
            {
                if( mAsyncComputeEntries[i].job == job )
                {
                    mAsyncComputeEntries[i] = mAsyncComputeEntries.back();
                    mAsyncComputeEntries.pop_back();
                }
                else
                    ++i;
            }
            ComputePsoCacheVec::iterator itCache = mComputeShaderCache.begin();
            ComputePsoCacheVec::iterator enCache = mComputeShaderCache.end();

            while( itCache != enCache )
            {
                if( itCache->job == job )
                {
                    mRenderSystem->_hlmsComputePipelineStateObjectDestroyed( &itCache->pso );
                    // We can't remove the entry, but we can at least cleanup
                    // some memory and leave an empty, unused entry
                    *itCache = ComputePsoCache();
                    mFreeShaderCacheEntries.push_back(
                        static_cast<size_t>( itCache - mComputeShaderCache.begin() ) );
                }
                ++itCache;
            }

            OGRE_DELETE itor->second.computeJob;
            mComputeJobs.erase( itor );
        }
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::destroyAllComputeJobs()
    {
        clearShaderCache();

        HlmsComputeJobMap::const_iterator itor = mComputeJobs.begin();
        HlmsComputeJobMap::const_iterator endt = mComputeJobs.end();

        while( itor != endt )
        {
            OGRE_DELETE itor->second.computeJob;
            ++itor;
        }

        mComputeJobs.clear();
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::clearShaderCache()
    {
        // ASYNC-SHADERS-1: the in-flight permutations go first (they would publish into the
        // cache being cleared).
        if( mHlmsManager && mHlmsManager->getAsyncCompiler() )
            mHlmsManager->getAsyncCompiler()->cancel( this );
        mAsyncComputeEntries.clear();
        mBuiltPermutations.clear();   // the code cache goes with it

        if( mRenderSystem )
            mRenderSystem->_setComputePso( 0 );

        ComputePsoCacheVec::iterator itor = mComputeShaderCache.begin();
        ComputePsoCacheVec::iterator endt = mComputeShaderCache.end();

        while( itor != endt )
        {
            if( itor->job )
            {
                mRenderSystem->_hlmsComputePipelineStateObjectDestroyed( &itor->pso );
                itor->job->mPsoCacheHash = std::numeric_limits<size_t>::max();
            }
            ++itor;
        }

        Hlms::clearShaderCache();
        mCompiledShaderCache.clear();
        mComputeShaderCache.clear();
        mFreeShaderCacheEntries.clear();
    }
    //-----------------------------------------------------------------------------------
    //-----------------------------------------------------------------------------------
    // ASYNC-SHADERS-1 (Jahshaka fork): a compute permutation built OFF the frame.
    //
    // The main thread snapshots everything compileShader would read from the job and from
    // the resource system (the property set, the pieces, the SOURCE TEXTS of the main file
    // and of every included piece file — ResourceGroupManager is not thread-safe at this
    // build's OGRE_THREAD_SUPPORT 0 — and the root layout); a service thread runs the same
    // template parse, compile and pipeline creation on its own Hlms slot; the main thread
    // files the PSO into mComputeShaderCache when it publishes, where dispatch() finds it.
    //-----------------------------------------------------------------------------------
    class HlmsCompute::AsyncComputeJob final : public HlmsAsyncJob
    {
    public:
        HlmsCompute    *mCompute;
        HlmsComputeJob *mJob;  ///< identity only (the key); never dereferenced off the main thread
        HlmsPropertyVec mJobProperties;  ///< the job's properties: the PSO cache key
        PiecesMap       mPieces;
        String          mSource;
        String          mSourceFilename;  ///< for names and logs
        StringVector    mPieceTexts;
        RootLayout      mRootLayout;      ///< memset + setupRootLayout, as compileShader hashes it
        bool            mIndirect;
        String          mJobName;

        // ---- results ----
        GpuProgramPtr                        mShader;
        vector<HighLevelGpuProgramPtr>::type mPrograms;  ///< created detached on the worker
        HlmsComputePso                       mPso;

        AsyncComputeJob( HlmsCompute *compute, HlmsComputeJob *job ) :
            HlmsAsyncJob( compute, job ),
            mCompute( compute ),
            mJob( job ),
            mIndirect( false )
        {
            memset( &mRootLayout, 0, sizeof( mRootLayout ) );
            mPso.initialize();
        }

        void run( size_t tid ) override { mCompute->compileAsync( *this, tid ); }
        void publish() override { mCompute->asyncComputePublished( *this ); }
        void discard() override { mCompute->asyncComputeDiscarded( *this ); }
    };
    //-----------------------------------------------------------------------------------
    size_t HlmsCompute::findAsyncComputeEntry( const HlmsComputeJob *job,
                                               const HlmsPropertyVec &props ) const
    {
        for( size_t i = 0u; i < mAsyncComputeEntries.size(); ++i )
        {
            if( mAsyncComputeEntries[i].job == job && mAsyncComputeEntries[i].setProperties == props )
                return i;
        }
        return mAsyncComputeEntries.size();
    }
    //-----------------------------------------------------------------------------------
    bool HlmsCompute::waitForAsyncPermutation( HlmsComputeJob *job )
    {
        HlmsAsyncCompiler *compiler = mHlmsManager ? mHlmsManager->getAsyncCompiler() : 0;
        if( !compiler )
            return false;
        bool bWaited = false;
        size_t idx = findAsyncComputeEntry( job, job->mSetProperties );
        while( idx < mAsyncComputeEntries.size() && mAsyncComputeEntries[idx].inFlight )
        {
            // The key is the job: this lands SOME permutation of it; loop until ours has.
            if( !compiler->waitFor( job, "a compute dispatch (HlmsCompute::dispatch)" ) )
                break;
            bWaited = true;
            idx = findAsyncComputeEntry( job, job->mSetProperties );
        }
        return bWaited;
    }
    //-----------------------------------------------------------------------------------
    uint64 HlmsCompute::permutationKey( const HlmsComputeJob *job )
    {
        return permutationKey( job, job->mSetProperties );
    }
    //-----------------------------------------------------------------------------------
    uint64 HlmsCompute::permutationKey( const HlmsComputeJob *job, const HlmsPropertyVec &properties )
    {
        uint64 h = 1469598103934665603ull;
        const auto mix = [&h]( uint64 v ) { h = ( h ^ v ) * 1099511628211ull; };
        for( const char c : job->mSourceFilename )
            mix( (unsigned char)c );
        for( const String &piece : job->mIncludedPieceFiles )
        {
            mix( 0x1fu );
            for( const char c : piece )
                mix( (unsigned char)c );
        }
        for( const HlmsProperty &p : properties )
        {
            mix( p.keyName.mHash );
            mix( (uint32)p.value );
        }
        return h;
    }
    //-----------------------------------------------------------------------------------
    size_t HlmsCompute::getNumAsyncInFlight() const
    {
        size_t n = 0u;
        for( const AsyncComputeEntry &e : mAsyncComputeEntries )
            n += e.inFlight ? 1u : 0u;
        return n;
    }
    //-----------------------------------------------------------------------------------
    HlmsCompute::AsyncReadiness HlmsCompute::requestAsync( HlmsComputeJob *job )
    {
        // dispatch()'s own first two steps, so the permutation asked about is the one a
        // dispatch would select.
        job->_calculateNumThreadGroupsBasedOnSetting();
        if( job->mPsoCacheHash < mComputeShaderCache.size() )
            return AsyncReady;  // the job's current permutation is bound to a built PSO

        job->_updateAutoProperties();

        ComputePsoCache key;
        key.job = job;
        key.setProperties.swap( job->mSetProperties );
        ComputePsoCacheVec::const_iterator itor =
            std::find( mComputeShaderCache.begin(), mComputeShaderCache.end(), key );
        key.setProperties.swap( job->mSetProperties );
        if( itor != mComputeShaderCache.end() )
            return AsyncReady;

        HlmsAsyncCompiler *compiler = mHlmsManager ? mHlmsManager->getAsyncCompiler() : 0;
        if( !compiler || !compiler->isRunning() )
            return AsyncReady;  // no service: dispatch() compiles in the frame, as upstream

        const size_t idx = findAsyncComputeEntry( job, job->mSetProperties );
        if( idx < mAsyncComputeEntries.size() )
            return mAsyncComputeEntries[idx].inFlight ? AsyncPending : AsyncFailed;

        // ---- the snapshot (main thread) ----
        AsyncComputeJob *request = OGRE_NEW AsyncComputeJob( this, job );
        request->mWhat = "compute job '" + job->getNameStr() + "'";
        try
        {
            ResourceGroupManager &resourceGroupMgr = ResourceGroupManager::getSingleton();
            request->mJobProperties = job->mSetProperties;
            request->mPieces = job->mPieces;
            request->mSourceFilename = job->mSourceFilename;
            request->mJobName = job->getNameStr();
            request->mSource =
                resourceGroupMgr.openResource( job->mSourceFilename + mShaderFileExt )->getAsString();
            for( const String &file : job->mIncludedPieceFiles )
            {
                String filename = file;
                // processPieces' extension rule, verbatim.
                String::size_type pos = filename.find_last_of( '.' );
                if( pos == String::npos ||
                    ( filename.compare( pos + 1, String::npos, mShaderFileExt ) != 0 &&
                      filename.compare( pos + 1, String::npos, "any" ) != 0 &&
                      filename.compare( pos + 1, String::npos, "metal" ) != 0 &&
                      filename.compare( pos + 1, String::npos, "glsl" ) != 0 &&
                      filename.compare( pos + 1, String::npos, "hlsl" ) != 0 ) )
                {
                    filename += mShaderFileExt;
                }
                request->mPieceTexts.push_back(
                    resourceGroupMgr.openResource( filename )->getAsString() );
            }
            request->mRootLayout.mCompute = true;
            job->setupRootLayout( request->mRootLayout );
            request->mIndirect = job->mIndirectDispatchBuffer != 0;
        }
        catch( Exception &e )
        {
            OGRE_DELETE request;
            LogManager::getSingleton().logMessage(
                "[async] compute job '" + job->getNameStr() +
                    "' cannot be requested: " + e.getFullDescription(),
                LML_CRITICAL );
            AsyncComputeEntry entry = { job, job->mSetProperties, 0 };
            mAsyncComputeEntries.push_back( entry );
            return AsyncFailed;
        }

        AsyncComputeEntry entry = { job, job->mSetProperties, request };
        mAsyncComputeEntries.push_back( entry );
        compiler->submit( request );
        return AsyncPending;
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::processPieceTexts( const StringVector &pieceTexts, const size_t tid )
    {
        for( const String &text : pieceTexts )
        {
            String inString = text;
            String outString;
            this->parseMath( inString, outString, tid );
            while( outString.find( "@foreach" ) != String::npos )
            {
                this->parseForEach( outString, inString, tid );
                inString.swap( outString );
            }
            this->parseProperties( outString, inString, tid );
            this->parseUndefPieces( inString, outString, tid );
            this->collectPieces( outString, inString, tid );
            this->parseCounter( inString, outString, tid );
        }
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::compileAsync( AsyncComputeJob &request, const size_t tid )
    {
        // compileShader, step for step, on slot `tid` and from the request's snapshot.
        mT[tid].setProperties = request.mJobProperties;
        for( const IdString &ext : mRsSpecificExtensions )
            setProperty( tid, ext, 1 );

        mT[tid].pieces = request.mPieces;

        if( mShaderProfile == "glsl" || mShaderProfile == "glslvk" )
            setProperty( tid, HlmsBaseProp::GL3Plus, mRenderSystem->getNativeShadingLanguageVersion() );
        setProperty( tid, HlmsBaseProp::Syntax, static_cast<int32>( mShaderSyntax.getU32Value() ) );
        setProperty( tid, HlmsBaseProp::Hlsl, static_cast<int32>( HlmsBaseProp::Hlsl.getU32Value() ) );
        setProperty( tid, HlmsBaseProp::Glsl, static_cast<int32>( HlmsBaseProp::Glsl.getU32Value() ) );
        setProperty( tid, HlmsBaseProp::Glslvk,
                     static_cast<int32>( HlmsBaseProp::Glslvk.getU32Value() ) );
        setProperty( tid, HlmsBaseProp::Hlslvk,
                     static_cast<int32>( HlmsBaseProp::Hlslvk.getU32Value() ) );
        setProperty( tid, HlmsBaseProp::Metal, static_cast<int32>( HlmsBaseProp::Metal.getU32Value() ) );
#if OGRE_PLATFORM == OGRE_PLATFORM_APPLE_IOS
        setProperty( tid, HlmsBaseProp::iOS, 1 );
#endif
#if OGRE_PLATFORM == OGRE_PLATFORM_APPLE
        setProperty( tid, HlmsBaseProp::macOS, 1 );
#endif
        setProperty( tid, HlmsBaseProp::Full32,
                     static_cast<int32>( HlmsBaseProp::Full32.getU32Value() ) );
        setProperty( tid, HlmsBaseProp::Midf16,
                     static_cast<int32>( HlmsBaseProp::Midf16.getU32Value() ) );
        setProperty( tid, HlmsBaseProp::Relaxed,
                     static_cast<int32>( HlmsBaseProp::Relaxed.getU32Value() ) );
        setProperty( tid, HlmsBaseProp::PrecisionMode, getSupportedPrecisionModeHash() );
        if( mFastShaderBuildHack )
            setProperty( tid, HlmsBaseProp::FastShaderBuildHack, 1 );

        processPieceTexts( request.mPieceTexts, tid );

        String inString = request.mSource;
        String outString;
        bool syntaxError = false;
        syntaxError |= this->parseMath( inString, outString, tid );
        while( !syntaxError && outString.find( "@foreach" ) != String::npos )
        {
            syntaxError |= this->parseForEach( outString, inString, tid );
            inString.swap( outString );
        }
        syntaxError |= this->parseProperties( outString, inString, tid );
        syntaxError |= this->parseUndefPieces( inString, outString, tid );
        while( !syntaxError && ( outString.find( "@piece" ) != String::npos ||
                                 outString.find( "@insertpiece" ) != String::npos ) )
        {
            syntaxError |= this->collectPieces( outString, inString, tid );
            syntaxError |= this->insertPieces( inString, outString, tid );
        }
        syntaxError |= this->parseCounter( outString, inString, tid );
        outString.swap( inString );

        if( syntaxError )
        {
            LogManager::getSingleton().logMessage( "There were HLMS syntax errors while parsing "
                                                   "(async) " +
                                                   request.mSourceFilename + mShaderFileExt );
        }

        if( getProperty( tid, HlmsBaseProp::DisableStage ) )
        {
            OGRE_EXCEPT( Exception::ERR_INVALID_STATE,
                         request.mJobName + ": the template disabled the compute stage",
                         "HlmsCompute::compileAsync" );
        }

        Hash hashVal;
        OGRE_HASH128_FUNC( outString.c_str(), static_cast<int>( outString.size() ), IdString::Seed,
                           &hashVal );
        if( mRenderSystem->getCapabilities()->hasCapability( RSC_EXPLICIT_API ) )
        {
            Hash hashValTmp[2] = { hashVal, Hash() };
            OGRE_HASH128_FUNC( &request.mRootLayout, sizeof( request.mRootLayout ), IdString::Seed,
                               &hashValTmp[1] );
            OGRE_HASH128_FUNC( hashValTmp, sizeof( hashValTmp ), IdString::Seed, &hashVal );
        }

        {
            ScopedLock lock( mMutex );
            CompiledShaderMap::const_iterator itor = mCompiledShaderCache.find( hashVal );
            if( itor != mCompiledShaderCache.end() )
                request.mShader = itor->second;
        }
        if( !request.mShader )
        {
            HighLevelGpuProgramManager *gpuProgramManager = HighLevelGpuProgramManager::getSingletonPtr();
            uint32 counter;
            {
                ScopedLock lock( mMutex );
                counter = mAsyncProgramCounter++;
            }
            HighLevelGpuProgramPtr gp = gpuProgramManager->createProgramDetached(
                "AsyncCompute" + StringConverter::toString( counter ) + request.mSourceFilename,
                ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME, mShaderProfile,
                GPT_COMPUTE_PROGRAM );
            gp->setSource( outString, "" );
            {
                RootLayout rootLayout = request.mRootLayout;
                gp->setRootLayout( gp->getType(), rootLayout );
                if( getProperty( tid, "uses_array_bindings" ) )
                    gp->setAutoReflectArrayBindingsInRootLayout( true );
            }
            if( mComputeShaderTarget )
            {
                gp->setParameter( "target", *mComputeShaderTarget );
                gp->setParameter( "entry_point", "main" );
            }
            gp->setSkeletalAnimationIncluded( getProperty( tid, HlmsBaseProp::Skeleton ) != 0 );
            gp->setMorphAnimationIncluded( false );
            gp->setPoseAnimationIncluded( getProperty( tid, HlmsBaseProp::Pose ) != 0 );
            gp->setVertexTextureFetchRequired( false );
            gp->load();
            request.mPrograms.push_back( gp );

            ScopedLock lock( mMutex );
            CompiledShaderMap::const_iterator itor = mCompiledShaderCache.find( hashVal );
            if( itor != mCompiledShaderCache.end() )
                request.mShader = itor->second;  // another thread got there first: share it
            else
            {
                request.mShader = gp;
                mCompiledShaderCache[hashVal] = gp;
            }
        }

        HlmsComputePso &pso = request.mPso;
        pso.computeShader = request.mShader;
        pso.computeParams = request.mShader->createParameters();
        pso.mThreadsPerGroup[0] = (uint32)( getProperty( tid, ComputeProperty::ThreadsPerGroupX ) );
        pso.mThreadsPerGroup[1] = (uint32)( getProperty( tid, ComputeProperty::ThreadsPerGroupY ) );
        pso.mThreadsPerGroup[2] = (uint32)( getProperty( tid, ComputeProperty::ThreadsPerGroupZ ) );
        pso.mNumThreadGroups[0] = (uint32)( getProperty( tid, ComputeProperty::NumThreadGroupsX ) );
        pso.mNumThreadGroups[1] = (uint32)( getProperty( tid, ComputeProperty::NumThreadGroupsY ) );
        pso.mNumThreadGroups[2] = (uint32)( getProperty( tid, ComputeProperty::NumThreadGroupsZ ) );
        if( pso.mThreadsPerGroup[0] * pso.mThreadsPerGroup[1] * pso.mThreadsPerGroup[2] == 0u ||
            ( !request.mIndirect &&
              pso.mNumThreadGroups[0] * pso.mNumThreadGroups[1] * pso.mNumThreadGroups[2] == 0u ) )
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                         request.mJobName + ": threads_per_group / num_thread_groups not set",
                         "HlmsCompute::compileAsync" );
        }

        mRenderSystem->_hlmsComputePipelineStateObjectCreated( &pso );
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::asyncComputePublished( AsyncComputeJob &request )
    {
        const size_t idx = findAsyncComputeEntry( request.mJob, request.mJobProperties );
        if( request.mFailed )
        {
            request.mPrograms.clear();  // a failed build's programs are dropped, never registered
            if( request.mPso.rsData )
                mRenderSystem->_hlmsComputePipelineStateObjectDestroyed( &request.mPso );
            if( idx < mAsyncComputeEntries.size() )
                mAsyncComputeEntries[idx].inFlight = 0;  // remembered: AsyncFailed
            return;
        }
        {
            // Under msGlobalMutex: see Hlms::AsyncPsoJob::registerPrograms.
            ScopedLock lock( msGlobalMutex );
            HighLevelGpuProgramManager &mgr = HighLevelGpuProgramManager::getSingleton();
            for( const HighLevelGpuProgramPtr &gp : request.mPrograms )
                mgr._registerDetachedProgram( gp );
            request.mPrograms.clear();
        }
        if( idx < mAsyncComputeEntries.size() )
        {
            mAsyncComputeEntries[idx] = mAsyncComputeEntries.back();
            mAsyncComputeEntries.pop_back();
        }

        // dispatch()'s filing of a freshly compiled PSO, with the job's shader params applied
        // now (the job may have changed them since the request).
        HlmsComputeJob *job = request.mJob;
        ShaderParams *shaderParams = job->_getShaderParams( "default" );
        if( shaderParams )
            shaderParams->updateParameters( request.mPso.computeParams, true );
        ShaderParams *profileParams = job->_getShaderParams( mShaderProfile );
        if( profileParams )
            profileParams->updateParameters( request.mPso.computeParams, true );

        mBuiltPermutations.insert( permutationKey( job, request.mJobProperties ) );
        ComputePsoCache psoCache( job, request.mJobProperties );
        psoCache.pso = request.mPso;
        if( shaderParams )
            psoCache.paramsUpdateCounter = shaderParams->getUpdateCounter();
        if( profileParams )
            psoCache.paramsProfileUpdateCounter = profileParams->getUpdateCounter();

        if( mFreeShaderCacheEntries.empty() )
            mComputeShaderCache.push_back( psoCache );
        else
        {
            const size_t freeIdx = mFreeShaderCacheEntries.back();
            mFreeShaderCacheEntries.pop_back();
            mComputeShaderCache[freeIdx] = psoCache;
        }
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::asyncComputeDiscarded( AsyncComputeJob &request )
    {
        if( request.mPso.rsData )
            mRenderSystem->_hlmsComputePipelineStateObjectDestroyed( &request.mPso );
        request.mPrograms.clear();
        const size_t idx = findAsyncComputeEntry( request.mJob, request.mJobProperties );
        if( idx < mAsyncComputeEntries.size() && mAsyncComputeEntries[idx].inFlight == &request )
        {
            mAsyncComputeEntries[idx] = mAsyncComputeEntries.back();
            mAsyncComputeEntries.pop_back();
        }
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::dispatch( HlmsComputeJob *job, SceneManager *sceneManager, Camera *camera )
    {
        job->_calculateNumThreadGroupsBasedOnSetting();

        if( job->mPsoCacheHash >= mComputeShaderCache.size() )
        {
            // Potentially needs to recompile.
            job->_updateAutoProperties();

            ComputePsoCache psoCache;
            psoCache.job = job;
            // To perform the search, temporarily borrow the properties to avoid an allocation & a copy.
            psoCache.setProperties.swap( job->mSetProperties );
            ComputePsoCacheVec::const_iterator itor =
                std::find( mComputeShaderCache.begin(), mComputeShaderCache.end(), psoCache );
            if( itor == mComputeShaderCache.end() )
            {
                // DEFERRED DISPATCH (setDeferredDispatch): not built, so not dispatched — handed
                // to the background compiler (or already in flight) and counted; never a
                // compile in the frame, never a wait.
                // A permutation that FAILED is not deferred for ever: it falls through and
                // throws as upstream does.
                // ...and a dispatch from a pass of an ASYNCHRONOUS workspace defers too: its
                // SceneManager's render queue carries the workspace's mode for exactly the
                // length of that workspace's update (CompositorWorkspace::_update).
                const bool bDefer =
                    mDeferredDispatch || msInAsyncWorkspace ||
                    ( sceneManager && sceneManager->getRenderQueue()->getAsyncShaderCompile() );
                if( bDefer && mHlmsManager && mHlmsManager->getAsyncCompiler()->isRunning() )
                {
                    psoCache.setProperties.swap( job->mSetProperties );
                    // A permutation already built under another job (a clone) needs no shader
                    // compile: it is built in the frame below, from the code cache.
                    const bool bKnown = mBuiltPermutations.count( permutationKey( job ) ) != 0u;
                    const AsyncReadiness readiness = bKnown ? AsyncReady : requestAsync( job );
                    psoCache.setProperties.swap( job->mSetProperties );
                    if( readiness == AsyncPending )
                    {
                        psoCache.setProperties.swap( job->mSetProperties );
                        ++mNumDeferredDispatches;
                        LogManager::getSingleton().logMessage(
                            "HlmsCompute: job '" + job->getNameStr() +
                            "' deferred: its permutation builds in the background" );
                        return;
                    }
                }
                // ASYNC-SHADERS-1: the asynchronous compiler may be building exactly this
                // permutation; take its result instead of building it a second time.
                psoCache.setProperties.swap( job->mSetProperties );
                const bool bWaited = waitForAsyncPermutation( job );
                psoCache.setProperties.swap( job->mSetProperties );
                if( bWaited )
                {
                    itor = std::find( mComputeShaderCache.begin(), mComputeShaderCache.end(),
                                      psoCache );
                }
            }
            if( itor == mComputeShaderCache.end() )
            {
                // Needs to recompile.

                // Return back the borrowed properties and make
                // a hard copy for starting the compilation.
                psoCache.setProperties.swap( job->mSetProperties );

                // (ASYNC-SHADERS-1) Named, like Hlms::createShaderCacheEntry's: a compute
                // permutation built in the frame while the background compiler runs.
                if( mHlmsManager && mHlmsManager->getAsyncCompiler()->isRunning() )
                {
                    // A clone of a built permutation only builds its pipeline (the code is cached);
                    // a NEW one compiles its shader here, on the frame's thread.
                    const bool bKnown = mBuiltPermutations.count( permutationKey( job ) ) != 0u;
                    LogManager::getSingleton().logMessage(
                        "HlmsCompute: job '" + job->getNameStr() +
                        ( bKnown ? "' builds a pipeline in the frame (its code is compiled)"
                                 : "' compiles a permutation in the frame" ) );
                }
                this->mT[kNoTid].setProperties = job->mSetProperties;

                // Uset the HlmsComputePso, as the ptr may be cached by the
                // RenderSystem and this could be invalidated
                mRenderSystem->_setComputePso( 0 );

                size_t newCacheEntryIdx = mComputeShaderCache.size();
                if( mFreeShaderCacheEntries.empty() )
                    mComputeShaderCache.push_back( ComputePsoCache() );
                else
                {
                    newCacheEntryIdx = mFreeShaderCacheEntries.back();
                    mFreeShaderCacheEntries.pop_back();
                }

                // Compile and add the PSO to the cache.
                psoCache.pso = compileShader( job, (uint32)newCacheEntryIdx );
                mBuiltPermutations.insert( permutationKey( job ) );

                ShaderParams *shaderParams = job->_getShaderParams( "default" );
                if( shaderParams )
                    psoCache.paramsUpdateCounter = shaderParams->getUpdateCounter();
                if( shaderParams )
                    psoCache.paramsProfileUpdateCounter = shaderParams->getUpdateCounter();

                mComputeShaderCache[newCacheEntryIdx] = psoCache;

                // The PSO in the cache doesn't have the properties. Make a hard copy.
                // We can use this->mSetProperties as it may have been modified during
                // compilerShader by the template.
                mComputeShaderCache[newCacheEntryIdx].setProperties = job->mSetProperties;

                job->mPsoCacheHash = newCacheEntryIdx;
            }
            else
            {
                // It was already in the cache. Return back the borrowed
                // properties and set the proper index to the cache.
                psoCache.setProperties.swap( job->mSetProperties );
                job->mPsoCacheHash = static_cast<size_t>( itor - mComputeShaderCache.begin() );
            }
        }

        ComputePsoCache &psoCache = mComputeShaderCache[job->mPsoCacheHash];

        {
            // Update dirty parameters, if necessary
            ShaderParams *shaderParams = job->_getShaderParams( "default" );
            if( shaderParams && psoCache.paramsUpdateCounter != shaderParams->getUpdateCounter() )
            {
                shaderParams->updateParameters( psoCache.pso.computeParams, false );
                psoCache.paramsUpdateCounter = shaderParams->getUpdateCounter();
            }

            shaderParams = job->_getShaderParams( mShaderProfile );
            if( shaderParams && psoCache.paramsProfileUpdateCounter != shaderParams->getUpdateCounter() )
            {
                shaderParams->updateParameters( psoCache.pso.computeParams, false );
                psoCache.paramsProfileUpdateCounter = shaderParams->getUpdateCounter();
            }
        }

        mRenderSystem->_setComputePso( &psoCache.pso );

        HlmsComputeJob::ConstBufferSlotVec::const_iterator itConst = job->mConstBuffers.begin();
        HlmsComputeJob::ConstBufferSlotVec::const_iterator enConst = job->mConstBuffers.end();

        while( itConst != enConst )
        {
            itConst->buffer->bindBufferCS( itConst->slotIdx );
            ++itConst;
        }

        if( job->mTexturesDescSet )
            mRenderSystem->_setTexturesCS( job->getGlTexSlotStart(), job->mTexturesDescSet );
        if( job->mSamplersDescSet )
            mRenderSystem->_setSamplersCS( job->getGlTexSlotStart(), job->mSamplersDescSet );
        if( job->mUavsDescSet )
            mRenderSystem->_setUavCS( 0u, job->mUavsDescSet );

        mAutoParamDataSource->setCurrentJob( job );
        mAutoParamDataSource->setCurrentCamera( camera );
        mAutoParamDataSource->setCurrentSceneManager( sceneManager );
        // mAutoParamDataSource->setCurrentShadowNode( shadowNode );
        // mAutoParamDataSource->setCurrentViewport( sceneManager->getCurrentViewport() );

        GpuProgramParametersSharedPtr csParams = psoCache.pso.computeParams;
        csParams->_updateAutoParams( mAutoParamDataSource, GPV_ALL );
        mRenderSystem->bindGpuProgramParameters( GPT_COMPUTE_PROGRAM, csParams, GPV_ALL );

        // Jahshaka fork 1bccc3f93+a98e2b0af (was 0032): a job that has been given an indirect buffer is sized by
        // the GPU, not by mNumThreadGroups.
        if( job->mIndirectDispatchBuffer )
        {
            mRenderSystem->_dispatchIndirect( psoCache.pso, job->mIndirectDispatchBuffer,
                                              job->mIndirectDispatchOffset,
                                              job->mIndirectDispatchBarrier );
        }
        else
        {
            mRenderSystem->_dispatch( psoCache.pso );
        }
    }
    //----------------------------------------------------------------------------------
    HlmsDatablock *HlmsCompute::createDatablockImpl( IdString datablockName,
                                                     const HlmsMacroblock *macroblock,
                                                     const HlmsBlendblock *blendblock,
                                                     const HlmsParamVec &paramVec )
    {
        return 0;
    }
    //----------------------------------------------------------------------------------
    void HlmsCompute::setupRootLayout( RootLayout &rootLayout, const size_t tid ) {}
    //----------------------------------------------------------------------------------
    void HlmsCompute::reloadFrom( Archive *newDataFolder, ArchiveVec *libraryFolders )
    {
        Hlms::reloadFrom( newDataFolder, libraryFolders );

        HlmsComputeJobMap::const_iterator itor = mComputeJobs.begin();
        HlmsComputeJobMap::const_iterator endt = mComputeJobs.end();

        while( itor != endt )
        {
            HlmsComputeJob *job = itor->second.computeJob;
            map<IdString, ShaderParams>::type::iterator it = job->mShaderParams.begin();
            map<IdString, ShaderParams>::type::iterator en = job->mShaderParams.end();

            while( it != en )
            {
                ShaderParams &shaderParams = it->second;
                ShaderParams::ParamVec::iterator itParam = shaderParams.mParams.begin();
                ShaderParams::ParamVec::iterator enParam = shaderParams.mParams.end();

                while( itParam != enParam )
                {
                    itParam->isDirty = true;
                    ++itParam;
                }

                shaderParams.setDirty();
                ++it;
            }

            ++itor;
        }
    }
    //----------------------------------------------------------------------------------
    HlmsComputeJob *HlmsCompute::createComputeJob( IdString datablockName, const String &refName,
                                                   const String &sourceFilename,
                                                   const StringVector &includedPieceFiles )
    {
        HlmsComputeJob *retVal = 0;

        std::pair<HlmsComputeJobMap::iterator, bool> insertion =
            mComputeJobs.insert( { datablockName, ComputeJobEntry( 0, refName ) } );
        if( insertion.second )
        {
            retVal = OGRE_NEW HlmsComputeJob( datablockName, this, sourceFilename, includedPieceFiles );
            insertion.first->second.computeJob = retVal;
        }
        else
        {
            OGRE_EXCEPT( Exception::ERR_DUPLICATE_ITEM,
                         "Compute Job with name " + datablockName.getFriendlyText() + " already exists!",
                         "HlmsCompute::createComputeJob" );
        }

        return retVal;
    }
    //----------------------------------------------------------------------------------
    HlmsComputeJob *HlmsCompute::findComputeJob( IdString datablockName ) const
    {
        HlmsComputeJob *retVal = findComputeJobNoThrow( datablockName );

        if( !retVal )
        {
            OGRE_EXCEPT( Exception::ERR_ITEM_NOT_FOUND,
                         "Compute Job with name " + datablockName.getFriendlyText() + " not found",
                         "HlmsCompute::findComputeJob" );
        }

        return retVal;
    }
    //----------------------------------------------------------------------------------
    HlmsComputeJob *HlmsCompute::findComputeJobNoThrow( IdString datablockName ) const
    {
        HlmsComputeJob *retVal = 0;

        HlmsComputeJobMap::const_iterator itor = mComputeJobs.find( datablockName );
        if( itor != mComputeJobs.end() )
            retVal = itor->second.computeJob;

        return retVal;
    }
    //----------------------------------------------------------------------------------
    const String *HlmsCompute::getJobNameStr( IdString name ) const
    {
        String const *retVal = 0;
        HlmsComputeJobMap::const_iterator itor = mComputeJobs.find( name );
        if( itor != mComputeJobs.end() )
            retVal = &itor->second.name;

        return retVal;
    }
    //----------------------------------------------------------------------------------
    HlmsDatablock *HlmsCompute::createDefaultDatablock() { return 0; }
    //----------------------------------------------------------------------------------
    uint32 HlmsCompute::fillBuffersFor( const HlmsCache *cache, const QueuedRenderable &queuedRenderable,
                                        bool casterPass, uint32 lastCacheHash, uint32 lastTextureHash )
    {
        OGRE_EXCEPT( Exception::ERR_INVALID_CALL, "This is a Compute Hlms",
                     "HlmsCompute::fillBuffersFor" );
    }
    uint32 HlmsCompute::fillBuffersForV1( const HlmsCache *cache,
                                          const QueuedRenderable &queuedRenderable, bool casterPass,
                                          uint32 lastCacheHash, CommandBuffer *commandBuffer )
    {
        OGRE_EXCEPT( Exception::ERR_INVALID_CALL, "This is a Compute Hlms",
                     "HlmsCompute::fillBuffersForV1" );
    }
    uint32 HlmsCompute::fillBuffersForV2( const HlmsCache *cache,
                                          const QueuedRenderable &queuedRenderable, bool casterPass,
                                          uint32 lastCacheHash, CommandBuffer *commandBuffer )
    {
        OGRE_EXCEPT( Exception::ERR_INVALID_CALL, "This is a Compute Hlms",
                     "HlmsCompute::fillBuffersForV2" );
    }
}  // namespace Ogre

#undef OGRE_HASH128_FUNC
