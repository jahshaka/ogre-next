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

#include <chrono>
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
    HlmsCompute::JahCodeRecord::JahCodeRecord() :
        explicitApi( false ),
        arrayBindings( false ),
        indirect( false ),
        jobSized( false )
    {
        inputKey[0] = inputKey[1] = programKey[0] = programKey[1] = 0u;
        for( size_t i = 0u; i < 3u; ++i )
            threadsPerGroup[i] = numThreadGroups[i] = 0u;
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::jahSetRsProperties( const size_t tid )
    {
        // (Jahshaka) compileShader's and compileAsync's own set-up, in one place: the key reads the same set.
        for( const IdString &ext : mRsSpecificExtensions )
            setProperty( tid, ext, 1 );

        if( mShaderProfile == "glsl" || mShaderProfile == "glslvk" )  // TODO: String comparision
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
    }
    //-----------------------------------------------------------------------------------
    String HlmsCompute::jahPieceFileName( const String &file ) const
    {
        // If it has an explicit extension, only open it if it matches the current
        // render system's. If it doesn't, then we add it ourselves.
        String filename = file;
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
        return filename;
    }
    //-----------------------------------------------------------------------------------
    const String &HlmsCompute::jahFileText( const String &filename )
    {
        std::map<String, String>::const_iterator itor = mJahFileMemo.find( filename );
        if( itor == mJahFileMemo.end() )
        {
            itor = mJahFileMemo
                       .insert( { filename,
                                  ResourceGroupManager::getSingleton().openResource( filename )->getAsString() } )
                       .first;
        }
        return itor->second;
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::jahGatherInputs( HlmsComputeJob *job, JahParseInputs &out )
    {
        out.sourceFilename = job->mSourceFilename + mShaderFileExt;
        out.templateText = &jahFileText( out.sourceFilename );
        out.pieceFiles.clear();
        for( const String &file : job->mIncludedPieceFiles )
        {
            const String name = jahPieceFileName( file );
            out.pieceFiles.push_back( { name, &jahFileText( name ) } );
        }
        // We MUST memset due to internal padding; otherwise we'll be hashing uninitialized values
        RootLayout rootLayout;
        memset( &rootLayout, 0, sizeof( rootLayout ) );
        rootLayout.mCompute = true;
        job->setupRootLayout( rootLayout );
        out.rootLayout.assign( reinterpret_cast<const char *>( &rootLayout ), sizeof( rootLayout ) );
    }
    //-----------------------------------------------------------------------------------
    bool HlmsCompute::jahInputKey( const JahParseInputs &in, const PiecesMap &cppPieces,
                                   const HlmsPropertyVec &properties, Hash &outKey ) const
    {
        if( !msJahComputeKeyHook )
            return false;
        const JahKeyInputs keyIn = { in.templateText, &in.pieceFiles, &cppPieces, &properties, &in.rootLayout };
        msJahComputeKeyHook( keyIn, outKey.hashVal );
        return true;
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::jahProgramKey( const String &source, const String &rootLayout, const bool explicitApi,
                                     uint64 outKey[2] )
    {
        Hash hashVal;
        OGRE_HASH128_FUNC( source.c_str(), static_cast<int>( source.size() ), IdString::Seed, &hashVal );
        if( explicitApi )
        {
            // If two shaders have the exact same source code but different
            // Root Layout, we should treat them differently
            Hash hashValTmp[2] = { hashVal, Hash() };
            OGRE_HASH128_FUNC( rootLayout.data(), static_cast<int>( rootLayout.size() ), IdString::Seed,
                               &hashValTmp[1] );
            OGRE_HASH128_FUNC( hashValTmp, sizeof( hashValTmp ), IdString::Seed, &hashVal );
        }
        outKey[0] = hashVal.hashVal[0];
        outKey[1] = hashVal.hashVal[1];
    }
    //-----------------------------------------------------------------------------------
    const HlmsCompute::JahCodeRecord *HlmsCompute::jahHit( const Hash &inputKey, const String &rootLayout,
                                                          GpuProgramPtr &outProgram, const bool join )
    {
        JahRecordMap::iterator itor = mJahRecords.find( inputKey );
        if( itor == mJahRecords.end() )
            return 0;
        const JahCodeRecord &r = *itor->second;
        // ANOTHER ROOT LAYOUT IS ANOTHER PROGRAM (the start preflight's F2): the descriptor layout the program was
        // built for must be the job's, whatever the key says; and a record of another kind of API is not ours.
        const bool bExplicit = mRenderSystem->getCapabilities()->hasCapability( RSC_EXPLICIT_API );
        if( r.rootLayout != rootLayout || r.explicitApi != bExplicit )
            return 0;
        // (A record pairs with a program only through its program key: one handed in whose source does not re-hash
        // to it was refused at the door, jahPrewarmCode - the lead's condition 1; a parse files only true ones.)
        Hash programKey;
        programKey.hashVal[0] = r.programKey[0];
        programKey.hashVal[1] = r.programKey[1];
        // THE JOIN (SHADER-SYSTEM-1, the final read's item 3a): a record's program enters mCompiledShaderCache only at
        // its code job's publish, on this thread - so a synchronous build that comes first (the atmosphere's LUT
        // dispatches on a warm boot) found it "not compiled yet" and parsed its template. Its job in flight is waited
        // for instead: HlmsAsyncCompiler::waitFor moves it to the front if queued, waits for the worker, publishes it
        // here. Never under mMutex (the publish takes it) - this thread holds neither it nor msGlobalMutex here.
        std::map<Hash, const void *>::const_iterator codeJob = mJahCodeJobs.find( inputKey );
        if( join && codeJob != mJahCodeJobs.end() && mHlmsManager && mHlmsManager->getAsyncCompiler() )
        {
            const void *jobKey = codeJob->second;  // the publish erases the entry
            const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            const bool bJoined =
                mHlmsManager->getAsyncCompiler()->waitFor( jobKey, "a compute hit (HlmsCompute::jahHit)" );
            const Real ms =
                Real( std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
            LogManager::getSingleton().logMessage(
                "HlmsCompute: '" + r.name + ( bJoined ? "' joined its code job" : "' found no code job to join" ) +
                " in " + StringConverter::toString( ms, 4 ) + " ms on the main thread (no template parse)" );
        }
        ScopedLock lock( mMutex );
        CompiledShaderMap::const_iterator program = mCompiledShaderCache.find( programKey );
        if( program == mCompiledShaderCache.end() )
            return 0;  // its code is not compiled (yet): the parse builds it
        outProgram = program->second;
        mJahUsedRecords.insert( inputKey );  // (Jahshaka) F10: a hit is a use
        return &r;
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::jahFileRecord( const JahCodeRecordPtr &record )
    {
        Hash key;
        key.hashVal[0] = record->inputKey[0];
        key.hashVal[1] = record->inputKey[1];
        mJahUsedRecords.insert( key );  // (Jahshaka) F10: built this launch is used this launch
        JahCodeRecordPtr &slot = mJahRecords[key];
        if( slot && slot->programKey[0] == record->programKey[0] &&
            slot->programKey[1] == record->programKey[1] && slot->rootLayout == record->rootLayout )
        {
            return;  // the same code under the same key: nothing changed
        }
        slot = record;
        ++mJahRecordGeneration;
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::getJahUsedRecordKeys( vector<std::pair<uint64, uint64> >::type &out ) const
    {
        out.clear();
        out.reserve( mJahUsedRecords.size() );
        for( const Hash &k : mJahUsedRecords )
            out.push_back( { k.hashVal[0], k.hashVal[1] } );
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::getJahRecords( vector<JahCodeRecordPtr>::type &out ) const
    {
        out.clear();
        out.reserve( mJahRecords.size() );
        for( const JahRecordMap::value_type &r : mJahRecords )
            out.push_back( r.second );
    }
    //-----------------------------------------------------------------------------------
    HlmsComputePso HlmsCompute::compileShader( HlmsComputeJob *job, uint32 finalHash )
    {
        // Assumes mSetProperties is already set
        // mSetProperties[kNoTid].clear();
        // Add RenderSystem-specific properties (and the rest of the set every parse starts from)
        jahSetRsProperties( kNoTid );

        GpuProgramPtr shader;
        // Generate the shader

        // Collect pieces
        mT[kNoTid].pieces.clear();

        // Start with the pieces sent by the user
        mT[kNoTid].pieces = job->mPieces;

        // (Jahshaka) WHAT THE PARSE READS, read once (the memo): the template, the piece files, the root layout.
        JahParseInputs inputs;
        jahGatherInputs( job, inputs );

        // (Jahshaka) THE HIT BEFORE THE PARSE: a record filed under the content key of exactly these inputs, whose
        // code is compiled, gives the program and the parse's outputs; the template is not parsed.
        Hash inputKey;
        const bool bKeyed = jahInputKey( inputs, job->mPieces, mT[kNoTid].setProperties, inputKey );
        const JahCodeRecord *hit = bKeyed ? jahHit( inputKey, inputs.rootLayout, shader, true ) : 0;

        uint32 threadsPerGroup[3];
        uint32 numThreadGroups[3];
        if( hit )
        {
            for( size_t i = 0u; i < 3u; ++i )
            {
                threadsPerGroup[i] = hit->threadsPerGroup[i];
                numThreadGroups[i] = hit->numThreadGroups[i];
            }
        }
        else
        {
            ++mJahParsedInFrame;
            // THE COUNTER's LINE (it replaces the two "in the frame" lines dispatch logged from the permutation set):
            // a template parsed on the frame's thread while the background compiler runs is named.
            if( mHlmsManager && mHlmsManager->getAsyncCompiler()->isRunning() )
            {
                LogManager::getSingleton().logMessage(
                    "HlmsCompute: job '" + job->getNameStr() + "' parsed its template in the frame (" +
                    StringConverter::toString( mJahParsedInFrame ) + " this run)" );
            }

            // Piece files
            StringVector pieceTexts;
            for( const std::pair<String, const String *> &piece : inputs.pieceFiles )
                pieceTexts.push_back( *piece.second );
            processPieceTexts( pieceTexts, kNoTid );

            String inString = *inputs.templateText;
            String outString;

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

            const bool bExplicitApi =
                mRenderSystem->getCapabilities()->hasCapability( RSC_EXPLICIT_API );
            Hash hashVal;
            // Don't create and compile if template requested not to
            if( !getProperty( kNoTid, HlmsBaseProp::DisableStage ) )
            {
                // Very similar to what the GpuProgramManager does with its microcode cache,
                // but we **need** to know if two Compute Shaders share the same source code.
                // (Jahshaka) the source's hash, paired with the root layout's on an explicit API: jahProgramKey.
                jahProgramKey( outString, inputs.rootLayout, bExplicitApi, hashVal.hashVal );

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

                    gp->setSkeletalAnimationIncluded( getProperty( kNoTid, HlmsBaseProp::Skeleton ) !=
                                                      0 );
                    gp->setMorphAnimationIncluded( false );
                    gp->setPoseAnimationIncluded( getProperty( kNoTid, HlmsBaseProp::Pose ) != 0 );
                    gp->setVertexTextureFetchRequired( false );

                    gp->load();

                    shader = gp;

                    ScopedLock lock( mMutex );
                    mCompiledShaderCache[hashVal] = shader;
                }
            }

            threadsPerGroup[0] = (uint32)( getProperty( kNoTid, ComputeProperty::ThreadsPerGroupX ) );
            threadsPerGroup[1] = (uint32)( getProperty( kNoTid, ComputeProperty::ThreadsPerGroupY ) );
            threadsPerGroup[2] = (uint32)( getProperty( kNoTid, ComputeProperty::ThreadsPerGroupZ ) );
            numThreadGroups[0] = (uint32)( getProperty( kNoTid, ComputeProperty::NumThreadGroupsX ) );
            numThreadGroups[1] = (uint32)( getProperty( kNoTid, ComputeProperty::NumThreadGroupsY ) );
            numThreadGroups[2] = (uint32)( getProperty( kNoTid, ComputeProperty::NumThreadGroupsZ ) );

            // (Jahshaka) THE RECORD of what was built, under the key of what the parse read.
            if( bKeyed && shader )
            {
                std::shared_ptr<JahCodeRecord> record = std::make_shared<JahCodeRecord>();
                record->inputKey[0] = inputKey.hashVal[0];
                record->inputKey[1] = inputKey.hashVal[1];
                record->programKey[0] = hashVal.hashVal[0];
                record->programKey[1] = hashVal.hashVal[1];
                record->name = job->getNameStr();
                record->sourceFilename = inputs.sourceFilename;
                record->source = outString;
                record->rootLayout = inputs.rootLayout;
                record->explicitApi = bExplicitApi;
                record->arrayBindings = getProperty( kNoTid, "uses_array_bindings" ) != 0;
                record->indirect = job->mIndirectDispatchBuffer != 0;
                record->jobSized = !job->getInformHlmsOfTextureSize();
                for( size_t i = 0u; i < 3u; ++i )
                {
                    record->threadsPerGroup[i] = threadsPerGroup[i];
                    record->numThreadGroups[i] = numThreadGroups[i];
                }
                JahFileHash file;
                file.name = inputs.sourceFilename;
                OGRE_HASH128_FUNC( inputs.templateText->c_str(), static_cast<int>( inputs.templateText->size() ),
                                   IdString::Seed, file.hash );
                record->files.push_back( file );
                for( const std::pair<String, const String *> &piece : inputs.pieceFiles )
                {
                    file.name = piece.first;
                    OGRE_HASH128_FUNC( piece.second->c_str(), static_cast<int>( piece.second->size() ),
                                       IdString::Seed, file.hash );
                    record->files.push_back( file );
                }
                jahFileRecord( record );
            }
        }

        // Reset the disable flag.
        setProperty( kNoTid, HlmsBaseProp::DisableStage, 0 );

        HlmsComputePso pso;
        pso.initialize();
        pso.computeShader = shader;
        pso.computeParams = shader->createParameters();
        for( size_t i = 0u; i < 3u; ++i )
        {
            pso.mThreadsPerGroup[i] = threadsPerGroup[i];
            pso.mNumThreadGroups[i] = numThreadGroups[i];
        }

        // Jahshaka fork 1bccc3f93+a98e2b0af (was 0032): a job dispatched INDIRECTLY has no CPU-side group count by
        // definition — the whole point is that only the GPU knows it — so requiring one here
        // would force every such job to carry a meaningless dummy. The threads per group are
        // still required (Metal needs them on the C++ side, and both back ends bake them into
        // the shader), so only the second half of the test is relaxed.
        const bool bIndirect = job->mIndirectDispatchBuffer != 0;
        // JAHSHAKA fork (BUGS-6): a job not told its sizes carries its group counts to the dispatch
        // (HlmsComputeJob::setInformHlmsOfTextureSize), so its permutation has none; dispatch checks.
        const bool bJobSized = !job->getInformHlmsOfTextureSize();
        if( pso.mThreadsPerGroup[0] * pso.mThreadsPerGroup[1] * pso.mThreadsPerGroup[2] == 0u ||
            ( !bIndirect && !bJobSized &&
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

        const auto psoStart = std::chrono::steady_clock::now();
        mRenderSystem->_hlmsComputePipelineStateObjectCreated( &pso );
        ++mJahPsoInFrame;
        // (Jahshaka) THE COUNTER's LINE, the parse line's twin: a compute pipeline built on the frame's thread while the
        // background compiler runs is named, with the driver's time for it.
        if( mHlmsManager && mHlmsManager->getAsyncCompiler()->isRunning() )
        {
            const double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - psoStart )
                                  .count();
            LogManager::getSingleton().logMessage(
                "HlmsCompute: job '" + job->getNameStr() + "' built its PSO in the frame (" +
                StringConverter::toString( mJahPsoInFrame ) + " this run, " + StringConverter::toString( Real( ms ) ) +
                " ms)" );
        }

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
            // (Jahshaka) A RUNNING build is waited for, on this (the render) thread: counted and timed (F4).
            if( mHlmsManager )
            {
                const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
                if( mHlmsManager->getAsyncCompiler()->cancelKey( job ) )
                {
                    ++mJahDestroyWaits;
                    mJahDestroyWaitUs += static_cast<uint64>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0 )
                            .count() );
                }
            }
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
        // (Jahshaka) the files are read afresh (reloadFrom calls this); the records stay - they are keyed by content,
        // and a hit needs its program in mCompiledShaderCache, which goes below.
        mJahFileMemo.clear();

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
        bool            mJobSized;  ///< (BUGS-6) the job carries its group counts to the dispatch
        String          mJobName;
        /// (Jahshaka) the record this build files at its publish: the input key (when a key hook is set), the
        /// files the parse read with their bytes' hashes, and - from run() - the generated source and its key.
        bool                      mKeyed;
        Hash                      mInputKey;
        String                    mRootLayoutBytes;
        vector<JahFileHash>::type mFiles;
        String                    mGenerated;
        Hash                      mProgramKey;
        bool                      mExplicitApi;
        bool                      mArrayBindings;

        // ---- results ----
        GpuProgramPtr                        mShader;
        vector<HighLevelGpuProgramPtr>::type mPrograms;  ///< created detached on the worker
        HlmsComputePso                       mPso;

        AsyncComputeJob( HlmsCompute *compute, HlmsComputeJob *job ) :
            HlmsAsyncJob( compute, job ),
            mCompute( compute ),
            mJob( job ),
            mIndirect( false ),
            mJobSized( false ),
            mKeyed( false ),
            mExplicitApi( false ),
            mArrayBindings( false )
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
            request->mJobProperties = job->mSetProperties;
            request->mPieces = job->mPieces;
            request->mSourceFilename = job->mSourceFilename;
            request->mJobName = job->getNameStr();
            // (Jahshaka) the texts through the memo, the root layout as compileShader hashes it, and the input key
            // over the properties the parse starts from (the main thread's slot as scratch: compileShader sets it
            // afresh).
            JahParseInputs inputs;
            jahGatherInputs( job, inputs );
            request->mSource = *inputs.templateText;
            for( const std::pair<String, const String *> &piece : inputs.pieceFiles )
                request->mPieceTexts.push_back( *piece.second );
            request->mRootLayout.mCompute = true;
            job->setupRootLayout( request->mRootLayout );
            request->mIndirect = job->mIndirectDispatchBuffer != 0;
            request->mJobSized = !job->getInformHlmsOfTextureSize();
            mT[kNoTid].setProperties = job->mSetProperties;
            jahSetRsProperties( kNoTid );
            request->mKeyed =
                jahInputKey( inputs, job->mPieces, mT[kNoTid].setProperties, request->mInputKey );
            if( request->mKeyed )
            {
                request->mRootLayoutBytes = inputs.rootLayout;
                JahFileHash file;
                file.name = inputs.sourceFilename;
                OGRE_HASH128_FUNC( inputs.templateText->c_str(), static_cast<int>( inputs.templateText->size() ),
                                   IdString::Seed, file.hash );
                request->mFiles.push_back( file );
                for( const std::pair<String, const String *> &piece : inputs.pieceFiles )
                {
                    file.name = piece.first;
                    OGRE_HASH128_FUNC( piece.second->c_str(), static_cast<int>( piece.second->size() ),
                                       IdString::Seed, file.hash );
                    request->mFiles.push_back( file );
                }
            }
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
        jahSetRsProperties( tid );

        mT[tid].pieces = request.mPieces;

        // (Jahshaka) a queued request the MAIN thread runs itself (no service thread left: waitFor) is a parse in
        // the frame.
        if( !HlmsAsyncCompiler::isServiceThread() )
            ++mJahParsedInFrame;

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
        const bool bExplicitApi = mRenderSystem->getCapabilities()->hasCapability( RSC_EXPLICIT_API );
        jahProgramKey( outString,
                       String( reinterpret_cast<const char *>( &request.mRootLayout ),
                               sizeof( request.mRootLayout ) ),
                       bExplicitApi, hashVal.hashVal );
        if( request.mKeyed )
        {
            // (Jahshaka) what the publish files as the record
            request.mGenerated = outString;
            request.mProgramKey = hashVal;
            request.mExplicitApi = bExplicitApi;
            request.mArrayBindings = getProperty( tid, "uses_array_bindings" ) != 0;
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
        // (Jahshaka) NO computeParams HERE: the program may be SHARED (mCompiledShaderCache, above) with
        // another service thread, and HighLevelGpuProgram::createParameters builds its definitions lazily
        // without a lock (OGRE_LOCK_AUTO_MUTEX is a no-op at OGRE_THREAD_SUPPORT 0) - two workers built one
        // program's definitions at once into one object, doubled, and a dispatch read past its parameters.
        // asyncComputePublished makes them on the main thread.
        pso.mThreadsPerGroup[0] = (uint32)( getProperty( tid, ComputeProperty::ThreadsPerGroupX ) );
        pso.mThreadsPerGroup[1] = (uint32)( getProperty( tid, ComputeProperty::ThreadsPerGroupY ) );
        pso.mThreadsPerGroup[2] = (uint32)( getProperty( tid, ComputeProperty::ThreadsPerGroupZ ) );
        pso.mNumThreadGroups[0] = (uint32)( getProperty( tid, ComputeProperty::NumThreadGroupsX ) );
        pso.mNumThreadGroups[1] = (uint32)( getProperty( tid, ComputeProperty::NumThreadGroupsY ) );
        pso.mNumThreadGroups[2] = (uint32)( getProperty( tid, ComputeProperty::NumThreadGroupsZ ) );
        if( pso.mThreadsPerGroup[0] * pso.mThreadsPerGroup[1] * pso.mThreadsPerGroup[2] == 0u ||
            ( !request.mIndirect && !request.mJobSized &&
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

        // dispatch()'s filing of a freshly compiled PSO.
        // (Jahshaka) ITS PARAMETERS ARE MADE HERE, ON THE MAIN THREAD (compileAsync's note): the only
        // thread that builds a program's definitions.
        request.mPso.computeParams = request.mShader->createParameters();
        HlmsComputeJob *job = request.mJob;
        // (Jahshaka) THE JOB's VALUES ARE OWED, NOT APPLIED HERE: the job's parameters now are its
        // CURRENT permutation's, and a shared job may have moved to another since the request (the
        // voxel clear's ClearUav interleaves float and uint targets: a uint program published while the
        // job holds fClearValue threw ITEM_NOT_FOUND twice on every first launch). Applying them here
        // would also CONSUME them: updateParameters clears isDirty on every parameter it visits, one
        // the program lacks included (OgreShaderParams.cpp), and dispatch() applies only dirty ones on a
        // counter change - a value the current permutation still owed its own program would be lost.
        // So the program gets the job's values whole at ITS OWN first dispatch (dispatch(),
        // jahParamsOwed), when the job is in this permutation: what the in-frame build does at the
        // dispatch that builds it (compileShader's forced update).
        ShaderParams *shaderParams = job->_getShaderParams( "default" );
        ShaderParams *profileParams = job->_getShaderParams( mShaderProfile );

        // (Jahshaka) THE RECORD of what the request built, under the key of what its parse read.
        if( request.mKeyed )
        {
            std::shared_ptr<JahCodeRecord> record = std::make_shared<JahCodeRecord>();
            record->inputKey[0] = request.mInputKey.hashVal[0];
            record->inputKey[1] = request.mInputKey.hashVal[1];
            record->programKey[0] = request.mProgramKey.hashVal[0];
            record->programKey[1] = request.mProgramKey.hashVal[1];
            record->name = request.mJobName;
            record->sourceFilename = request.mSourceFilename + mShaderFileExt;
            record->source = request.mGenerated;
            record->rootLayout = request.mRootLayoutBytes;
            record->explicitApi = request.mExplicitApi;
            record->arrayBindings = request.mArrayBindings;
            record->indirect = request.mIndirect;
            record->jobSized = request.mJobSized;
            for( size_t i = 0u; i < 3u; ++i )
            {
                record->threadsPerGroup[i] = request.mPso.mThreadsPerGroup[i];
                record->numThreadGroups[i] = request.mPso.mNumThreadGroups[i];
            }
            record->files = request.mFiles;
            jahFileRecord( record );
        }
        ComputePsoCache psoCache( job, request.mJobProperties );
        psoCache.pso = request.mPso;
        if( shaderParams )
            psoCache.paramsUpdateCounter = shaderParams->getUpdateCounter();
        if( profileParams )
            psoCache.paramsProfileUpdateCounter = profileParams->getUpdateCounter();
        psoCache.jahParamsOwed = true;

        // (Jahshaka) Unset the HlmsComputePso, as dispatch()'s in-frame build does: the ptr may be
        // cached by the RenderSystem, and the push_back below can reallocate mComputeShaderCache
        // (a publish inside waitForAsyncPermutation runs between two dispatches).
        mRenderSystem->_setComputePso( 0 );

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
    // (Jahshaka fork, THE CODE-ONLY REQUEST: COLD-BOOT-1 stage 3, the lead's ruling P6.) A record a launch read has its
    // CODE compiled off the frame: no job, no PSO - the generated source and the root layout it was built with, on a
    // service thread, into a detached program the main thread registers and files into mCompiledShaderCache at the
    // publish, where the record's hit finds it. The record itself was filed when the request was made.
    //-----------------------------------------------------------------------------------
    class HlmsCompute::AsyncCodeJob final : public HlmsAsyncJob
    {
    public:
        HlmsCompute           *mCompute;
        JahCodeRecordPtr       mRecord;
        HighLevelGpuProgramPtr mProgram;  ///< created detached on the worker; null if the code was compiled already
        uint32                 mCounter;

        AsyncCodeJob( HlmsCompute *compute, const JahCodeRecordPtr &record, uint32 counter ) :
            HlmsAsyncJob( compute, this ),
            mCompute( compute ),
            mRecord( record ),
            mCounter( counter )
        {
        }

        void run( size_t tid ) override;
        void publish() override;
        void discard() override
        {
            forget();
            mProgram.reset();
        }
        /// Main thread (publish, discard): the record's job is no longer in flight (jahHit joins only one that is).
        void forget()
        {
            Hash key;
            key.hashVal[0] = mRecord->inputKey[0];
            key.hashVal[1] = mRecord->inputKey[1];
            std::map<Hash, const void *>::iterator it = mCompute->mJahCodeJobs.find( key );
            if( it != mCompute->mJahCodeJobs.end() && it->second == mKey )
                mCompute->mJahCodeJobs.erase( it );
        }
    };
    //-----------------------------------------------------------------------------------
    void HlmsCompute::AsyncCodeJob::run( size_t /*tid*/ )
    {
        const JahCodeRecord &r = *mRecord;
        Hash programKey;
        programKey.hashVal[0] = r.programKey[0];
        programKey.hashVal[1] = r.programKey[1];
        {
            ScopedLock lock( mCompute->mMutex );
            if( mCompute->mCompiledShaderCache.find( programKey ) != mCompute->mCompiledShaderCache.end() )
                return;  // compiled already (a parse, or another record of the same code)
        }
        // THE ROOT LAYOUT FROM ITS BYTES: the public fields only; what follows them (the reflected array ranges and
        // the padding) is zero in every layout a record is made from (memset + setupRootLayout), or it is no record.
        RootLayout rootLayout;
        memset( &rootLayout, 0, sizeof( rootLayout ) );
        const size_t fields = static_cast<size_t>( reinterpret_cast<const char *>( &rootLayout.mDescBindingRanges ) -
                                                   reinterpret_cast<const char *>( &rootLayout ) ) +
                              sizeof( rootLayout.mDescBindingRanges );
        if( r.rootLayout.size() != sizeof( rootLayout ) ||
            r.rootLayout.find_first_not_of( '\0', fields ) != String::npos )
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS, r.name + ": its code record holds no root layout",
                         "HlmsCompute::AsyncCodeJob::run" );
        }
        memcpy( &rootLayout, r.rootLayout.data(), fields );

        HighLevelGpuProgramPtr gp = HighLevelGpuProgramManager::getSingleton().createProgramDetached(
            "AsyncComputeCode" + StringConverter::toString( mCounter ) + r.sourceFilename,
            ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME, mCompute->mShaderProfile, GPT_COMPUTE_PROGRAM );
        gp->setSource( r.source, "" );
        gp->setRootLayout( gp->getType(), rootLayout );
        if( r.arrayBindings )
            gp->setAutoReflectArrayBindingsInRootLayout( true );
        if( mCompute->mComputeShaderTarget )
        {
            gp->setParameter( "target", *mCompute->mComputeShaderTarget );
            gp->setParameter( "entry_point", "main" );
        }
        gp->setSkeletalAnimationIncluded( false );
        gp->setMorphAnimationIncluded( false );
        gp->setPoseAnimationIncluded( false );
        gp->setVertexTextureFetchRequired( false );
        gp->load();
        mProgram = gp;
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::AsyncCodeJob::publish()
    {
        forget();
        if( mFailed || !mProgram )
        {
            mProgram.reset();
            return;
        }
        Hash programKey;
        programKey.hashVal[0] = mRecord->programKey[0];
        programKey.hashVal[1] = mRecord->programKey[1];
        {
            ScopedLock lock( mCompute->mMutex );
            if( mCompute->mCompiledShaderCache.find( programKey ) != mCompute->mCompiledShaderCache.end() )
            {
                mProgram.reset();  // a parse in the meantime got there first: its program serves
                return;
            }
        }
        {
            // Under msGlobalMutex: see Hlms::AsyncPsoJob::registerPrograms.
            ScopedLock lock( msGlobalMutex );
            HighLevelGpuProgramManager::getSingleton()._registerDetachedProgram( mProgram );
        }
        ScopedLock lock( mCompute->mMutex );
        mCompute->mCompiledShaderCache[programKey] = mProgram;
        mProgram.reset();
    }
    //-----------------------------------------------------------------------------------
    void HlmsCompute::jahPrewarmCode( vector<JahCodeRecord>::type &records )
    {
        HlmsAsyncCompiler *compiler = mHlmsManager ? mHlmsManager->getAsyncCompiler() : 0;
        const bool bExplicit = mRenderSystem->getCapabilities()->hasCapability( RSC_EXPLICIT_API );
        for( JahCodeRecord &in : records )
        {
            Hash key;
            key.hashVal[0] = in.inputKey[0];
            key.hashVal[1] = in.inputKey[1];
            if( mJahRecords.find( key ) != mJahRecords.end() )
                continue;  // this launch filed it already
            // THE LEAD's CONDITION 1, AT THE DOOR: a record whose source does not re-hash to its program key is
            // another compiler's or damaged, and is never filed (nor one of another kind of API): it cannot hit.
            uint64 programKey[2];
            jahProgramKey( in.source, in.rootLayout, in.explicitApi, programKey );
            if( programKey[0] != in.programKey[0] || programKey[1] != in.programKey[1] ||
                in.explicitApi != bExplicit )
            {
                continue;
            }
            JahCodeRecordPtr record = std::make_shared<const JahCodeRecord>( std::move( in ) );
            mJahRecords[key] = record;
            ++mJahRecordGeneration;
            if( compiler && compiler->isRunning() )
            {
                uint32 counter;
                {
                    ScopedLock lock( mMutex );
                    counter = mAsyncProgramCounter++;
                }
                AsyncCodeJob *job = OGRE_NEW AsyncCodeJob( this, record, counter );
                job->mWhat = "compute code '" + record->name + "'";
                mJahCodeJobs[key] = job->mKey;  // in flight until its publish or discard (jahHit joins it)
                compiler->submit( job );
            }
        }
        records.clear();
    }
    //-----------------------------------------------------------------------------------
    void ( *HlmsCompute::msJahDispatchHook )( const HlmsComputeJob *job ) = 0;
    void ( *HlmsCompute::msJahComputeKeyHook )( const JahKeyInputs &in, uint64 outKey[2] ) = 0;
    //-----------------------------------------------------------------------------------
    void HlmsCompute::dispatch( HlmsComputeJob *job, SceneManager *sceneManager, Camera *camera )
    {
        // JAHSHAKA fork: a job STOOD DOWN (HlmsComputeJob::setStoodDown) dispatches nothing - and
        // reads nothing of its slots or its counts: BUGS-6's zero-group guard below throws for a
        // job that is not.
        if( job->getStoodDown() )
            return;

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
                    // (Jahshaka, SHADER-SYSTEM-1 7a) A KNOWN CODE TOO: a permutation whose code a record
                    // already holds (a clone, a warm launch) still needs its PIPELINE, a driver call of its
                    // own - requested on the background compiler like any other (compileAsync builds the PSO
                    // there), its dispatch deferred until it publishes. It used to be built here, in the
                    // frame (a warm launch's arm: 46-50 pipelines in frames, gi.async_arm_warm).
                    const AsyncReadiness readiness = requestAsync( job );
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
                const std::chrono::steady_clock::time_point joinT0 = std::chrono::steady_clock::now();
                const bool bWaited = waitForAsyncPermutation( job );
                psoCache.setProperties.swap( job->mSetProperties );
                if( bWaited )
                {
                    // (Jahshaka) A JOIN, counted and timed (getJahJoins).
                    ++mJahJoins;
                    mJahJoinUs += static_cast<uint64>( std::chrono::duration_cast<std::chrono::microseconds>(
                                                           std::chrono::steady_clock::now() - joinT0 )
                                                           .count() );
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

                // (Jahshaka) A permutation built in the frame is COUNTED: compileShader's getJahParsedInFrame
                // (its template parsed here, the line it logs names the job) and getJahPsoInFrame (its pipeline).
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

        // (Jahshaka) THE PARAMETERS FIT THEIR PROGRAM, checked at a PSO's first dispatch and until it holds:
        // every definition the render system copies (VulkanProgram::updateBuffers) lies inside the
        // parameters object's lists. A misfit is never dispatched - it is deferred and counted
        // (getJahParamsMisfits: a test asserts 0) - whatever made it (Jahshaka PHOTON-7 stage 5: the
        // doubled definitions of a program two service threads built at once read past the lists, and
        // a compute loop bounded by the garbage ran until the GPU lost its context).
        // (the counter's negative control, _jahForceParamsMisfits: a dispatch refused as a misfit is)
        const bool jahForced = mJahForceMisfits > 0u;
        if( jahForced )
            --mJahForceMisfits;
        if( !psoCache.jahParamsFit || jahForced )
        {
            if( jahForced ||
                ( psoCache.pso.computeParams && !mRenderSystem->_jahComputeParamsFit( psoCache.pso ) ) )
            {
                if( mJahParamsMisfits++ < 4u )
                {
                    LogManager::getSingleton().logMessage(
                        "HlmsCompute: job '" + job->getNameStr() +
                            "' NOT dispatched: its program's parameter definitions do not fit its parameters",
                        LML_CRITICAL );
                }
                return;
            }
            psoCache.jahParamsFit = true;
        }

        {
            // Update dirty parameters, if necessary
            // (Jahshaka) ...or ALL of them, forced, at an asynchronously published PSO's first dispatch
            // (jahParamsOwed: asyncComputePublished's note).
            const bool bOwed = psoCache.jahParamsOwed;
            psoCache.jahParamsOwed = false;
            ShaderParams *shaderParams = job->_getShaderParams( "default" );
            if( shaderParams &&
                ( bOwed || psoCache.paramsUpdateCounter != shaderParams->getUpdateCounter() ) )
            {
                shaderParams->updateParameters( psoCache.pso.computeParams, bOwed );
                psoCache.paramsUpdateCounter = shaderParams->getUpdateCounter();
            }

            shaderParams = job->_getShaderParams( mShaderProfile );
            if( shaderParams &&
                ( bOwed || psoCache.paramsProfileUpdateCounter != shaderParams->getUpdateCounter() ) )
            {
                shaderParams->updateParameters( psoCache.pso.computeParams, bOwed );
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
        if( msJahDispatchHook )
            msJahDispatchHook( job );   // Jahshaka: THE GPU CHECKPOINTS (the declaration)
        if( job->mIndirectDispatchBuffer )
        {
            mRenderSystem->_dispatchIndirect( psoCache.pso, job->mIndirectDispatchBuffer,
                                              job->mIndirectDispatchOffset,
                                              job->mIndirectDispatchBarrier );
        }
        else
        {
            // JAHSHAKA fork (BUGS-6): a job not told its sizes is sized here, from its own counts
            // (its permutation carries none: HlmsComputeJob::setInformHlmsOfTextureSize). The cache
            // entry is this job's alone (ComputePsoCache compares the job), so it is written in place.
            if( !job->getInformHlmsOfTextureSize() )
            {
                for( size_t i = 0u; i < 3u; ++i )
                    psoCache.pso.mNumThreadGroups[i] = job->mNumThreadGroups[i];
                if( psoCache.pso.mNumThreadGroups[0] * psoCache.pso.mNumThreadGroups[1] *
                        psoCache.pso.mNumThreadGroups[2] ==
                    0u )
                {
                    OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                                 job->getNameStr() + ": num_thread_groups not set (the job sizes its "
                                                     "own dispatch: setInformHlmsOfTextureSize)",
                                 "HlmsCompute::dispatch" );
                }
            }
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
