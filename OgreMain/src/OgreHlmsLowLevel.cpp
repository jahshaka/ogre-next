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

#include "OgreHlmsLowLevel.h"

#include "Animation/OgreSkeletonInstance.h"
#include "CommandBuffer/OgreCbLowLevelMaterial.h"
#include "CommandBuffer/OgreCommandBuffer.h"
#include "Compositor/OgreCompositorShadowNode.h"
#include "OgreHighLevelGpuProgram.h"
#include "OgreHighLevelGpuProgramManager.h"
#include "OgreHlmsLowLevelDatablock.h"
#include "OgreMaterial.h"
#include "OgrePass.h"
#include "OgreRenderQueue.h"
#include "OgreSceneManager.h"
#include "OgreTechnique.h"
#include "Vao/OgreVertexArrayObject.h"

#include "OgreLogManager.h"
#include "OgreStringConverter.h"

#include <atomic>

namespace Ogre
{
    const IdString LowLevelProp::Program[LowLevelProp::NumStages] = {
        IdString( "ll_vs" ), IdString( "ll_gs" ), IdString( "ll_hs" ), IdString( "ll_ds" ), IdString( "ll_ps" )
    };
    const IdString LowLevelProp::ProgramLoad[LowLevelProp::NumStages] = {
        IdString( "ll_vs_load" ), IdString( "ll_gs_load" ), IdString( "ll_hs_load" ), IdString( "ll_ds_load" ),
        IdString( "ll_ps_load" )
    };
    const IdString LowLevelProp::VertexInput = IdString( "ll_vertex_input" );

    HlmsLowLevel::HlmsLowLevel() :
        Hlms( HLMS_LOW_LEVEL, "", 0, 0 ),
        mAutoParamDataSource( 0 ),
        mCurrentSceneManager( 0 )
    {
        mAutoParamDataSource = OGRE_NEW AutoParamDataSource();
    }
    //-----------------------------------------------------------------------------------
    HlmsLowLevel::~HlmsLowLevel()
    {
        OGRE_DELETE mAutoParamDataSource;
        mAutoParamDataSource = 0;
    }
    //-----------------------------------------------------------------------------------
    /// (Jahshaka) getJahLowLevelBuiltInFrame's total.
    static std::atomic<uint64> sJahLowLevelBuiltInFrame{ 0u };
    uint64 HlmsLowLevel::getJahLowLevelBuiltInFrame()
    {
        return sJahLowLevelBuiltInFrame.load( std::memory_order_relaxed );
    }
    //-----------------------------------------------------------------------------------
    const HlmsCache *HlmsLowLevel::createShaderCacheEntry( uint32 renderableHash,
                                                           const HlmsCache &passCache, uint32 finalHash,
                                                           const QueuedRenderable &queuedRenderable,
                                                           HlmsCache *reservedStubEntry, uint64 deadline,
                                                           const size_t tid )
    {
        Renderable *renderable = queuedRenderable.renderable;
        const MaterialPtr &mat = renderable->getMaterial();
        Technique *technique = mat->getBestTechnique( renderable->getCurrentMaterialLod(), renderable );
        Pass *pass = technique->getPass( 0 );

        if( !pass->isProgrammable() )
        {
            OGRE_EXCEPT( Exception::ERR_RENDERINGAPI_ERROR,
                         "Fixed Function pipeline is no longer allowed nor supported. "
                         "The material " +
                             mat->getName() + " must use shaders",
                         "HlmsLowLevel::createShaderCacheEntry" );
        }

        HlmsPso pso;
        pso.initialize();
        if( pass->hasVertexProgram() )
        {
            pso.vertexShader = pass->getVertexProgram();
            pso.clipDistances = pso.vertexShader->getNumClipDistances();
        }
        if( pass->hasGeometryProgram() )
            pso.geometryShader = pass->getGeometryProgram();
        if( pass->hasTessellationHullProgram() )
            pso.tesselationHullShader = pass->getTessellationHullProgram();
        if( pass->hasTessellationDomainProgram() )
            pso.tesselationDomainShader = pass->getTessellationDomainProgram();
        if( pass->hasFragmentProgram() )
            pso.pixelShader = pass->getFragmentProgram();

        bool casterPass = getProperty( passCache.setProperties, HlmsBaseProp::ShadowCaster ) != 0;

        // Set the properties from pass cache
        mT[tid].setProperties.clear();
        mT[tid].setProperties.reserve( passCache.setProperties.size() );
        {
            // Now copy the properties from the pass (one by one, since be must maintain the order)
            HlmsPropertyVec::const_iterator itor = passCache.setProperties.begin();
            HlmsPropertyVec::const_iterator endt = passCache.setProperties.end();

            while( itor != endt )
            {
                setProperty( tid, itor->keyName, itor->value );
                ++itor;
            }
        }

        const HlmsDatablock *datablock = queuedRenderable.renderable->getDatablock();
        pso.macroblock = datablock->getMacroblock( casterPass );
        pso.blendblock = datablock->getBlendblock( casterPass );
        pso.pass = passCache.pso.pass;

        if( queuedRenderable.renderable )
        {
            const VertexArrayObjectArray &vaos =
                queuedRenderable.renderable->getVaos( static_cast<VertexPass>( casterPass ) );
            if( !vaos.empty() )
            {
                // v2 object. TODO: LOD? Should we allow Vaos with different vertex formats on LODs?
                //(also the datablock hash in the renderable would have to account for that)
                pso.operationType = vaos.front()->getOperationType();
                pso.vertexElements = vaos.front()->getVertexDeclaration();
            }
            else
            {
                // v1 object.
                v1::RenderOperation renderOp;
                queuedRenderable.renderable->getRenderOperation( renderOp, casterPass );
                pso.operationType = renderOp.operationType;
                pso.vertexElements = renderOp.vertexData->vertexDeclaration->convertToV2();
            }

            pso.enablePrimitiveRestart = false;
        }

        applyStrongBlockRules( pso, tid );

        mRenderSystem->_hlmsPipelineStateObjectCreated( &pso, (uint64)-1 );
        // (Jahshaka) COUNTED and NAMED: a low-level pipeline is always built in the frame that first draws it.
        sJahLowLevelBuiltInFrame.fetch_add( 1u, std::memory_order_relaxed );
        LogManager::getSingleton().logMessage(
            "HlmsLowLevel: material '" + mat->getName() + "' built its pipeline in the frame (" +
            StringConverter::toString( sJahLowLevelBuiltInFrame.load( std::memory_order_relaxed ) ) +
            " this run)" );

        if( reservedStubEntry )
        {
            OGRE_ASSERT_LOW( !reservedStubEntry->pso.vertexShader &&
                             !reservedStubEntry->pso.geometryShader &&
                             !reservedStubEntry->pso.tesselationHullShader &&
                             !reservedStubEntry->pso.tesselationDomainShader &&
                             !reservedStubEntry->pso.pixelShader && "Race condition?" );
            reservedStubEntry->pso = pso;
        }

        const HlmsCache *retVal =
            reservedStubEntry ? reservedStubEntry : addShaderCache( finalHash, pso );

        return retVal;
    }
    //-----------------------------------------------------------------------------------
    void HlmsLowLevel::setJahProgramKey( const Pass *pass )
    {
        const GpuProgramPtr programs[LowLevelProp::NumStages] = {
            pass->hasVertexProgram() ? pass->getVertexProgram() : GpuProgramPtr(),
            pass->hasGeometryProgram() ? pass->getGeometryProgram() : GpuProgramPtr(),
            pass->hasTessellationHullProgram() ? pass->getTessellationHullProgram() : GpuProgramPtr(),
            pass->hasTessellationDomainProgram() ? pass->getTessellationDomainProgram() : GpuProgramPtr(),
            pass->hasFragmentProgram() ? pass->getFragmentProgram() : GpuProgramPtr()
        };
        for( size_t i = 0u; i < LowLevelProp::NumStages; ++i )
        {
            if( !programs[i] )
                continue;
            // THE PROGRAM THE PIPELINE BINDS: a unified program forwards its load, reload and listeners to its
            // delegate and never moves its own state count (UnifiedHighLevelGpuProgram), so the key is the binding
            // delegate's - the program the render system builds the pipeline from (VulkanRenderSystem reads
            // _getBindingDelegate). Not loaded yet (no delegate): the program itself.
            const GpuProgram *bound = programs[i]->_getBindingDelegate();
            if( !bound )
                bound = programs[i].get();
            // The handle is unique per program for the process (ResourceManager's counter): truncated to the
            // property's 32 bits it repeats only after 2^32 resources.
            setProperty( kNoTid, LowLevelProp::Program[i], static_cast<int32>( bound->getHandle() ) );
            setProperty( kNoTid, LowLevelProp::ProgramLoad[i], static_cast<int32>( bound->getStateCount() ) );
        }
    }
    //-----------------------------------------------------------------------------------
    void HlmsLowLevel::setJahVertexKey( Renderable *renderable, bool casterPass )
    {
        // The vertex input the pipeline is built with (createShaderCacheEntry reads the same): with the pass id gone,
        // two renderables that share a program pair share a pipeline only when their vertex layouts and topology
        // agree.
        uint32 h = 0x811C9DC5u;
        const auto mix = [&h]( uint32 v )
        {
            h ^= v;
            h *= 0x01000193u;
        };
        const VertexArrayObjectArray &vaos = renderable->getVaos( static_cast<VertexPass>( casterPass ) );
        if( !vaos.empty() )
        {
            mix( static_cast<uint32>( vaos.front()->getOperationType() ) );
            for( const VertexElement2Vec &buffer : vaos.front()->getVertexDeclaration() )
            {
                mix( 0xFFFFFFFFu );   // a buffer's boundary
                for( const VertexElement2 &e : buffer )
                {
                    mix( static_cast<uint32>( e.mType ) );
                    mix( static_cast<uint32>( e.mSemantic ) );
                    mix( e.mInstancingStepRate );
                }
            }
        }
        else
        {
            v1::RenderOperation renderOp;
            renderable->getRenderOperation( renderOp, casterPass );
            mix( static_cast<uint32>( renderOp.operationType ) );
            if( renderOp.vertexData && renderOp.vertexData->vertexDeclaration )
            {
                for( const VertexElement2Vec &buffer :
                     renderOp.vertexData->vertexDeclaration->convertToV2() )
                {
                    mix( 0xFFFFFFFFu );
                    for( const VertexElement2 &e : buffer )
                    {
                        mix( static_cast<uint32>( e.mType ) );
                        mix( static_cast<uint32>( e.mSemantic ) );
                        mix( e.mInstancingStepRate );
                    }
                }
            }
        }
        setProperty( kNoTid, LowLevelProp::VertexInput, static_cast<int32>( h ) );
    }
    //-----------------------------------------------------------------------------------
    void HlmsLowLevel::calculateHashFor( Renderable *renderable, uint32 &outHash, uint32 &outCasterHash )
    {
        mT[kNoTid].setProperties.clear();

        const MaterialPtr &mat = renderable->getMaterial();

        Material::TechniqueIterator techniqueIt = mat->getTechniqueIterator();
        while( techniqueIt.hasMoreElements() )
        {
            Technique *technique = techniqueIt.getNext();
            Technique::PassIterator passIt = technique->getPassIterator();

            while( passIt.hasMoreElements() )
            {
                Pass *pass = passIt.getNext();

                if( !pass->isProgrammable() )
                {
                    OGRE_EXCEPT( Exception::ERR_RENDERINGAPI_ERROR,
                                 "Fixed Function pipeline is no longer allowed nor supported. "
                                 "The material " +
                                     mat->getName() + " must use shaders",
                                 "HlmsLowLevel::calculateHashFor" );
                }
            }
        }

        setProperty( kNoTid, HlmsPsoProp::Macroblock,
                     renderable->getDatablock()->getMacroblock( false )->mLifetimeId );
        setProperty( kNoTid, HlmsPsoProp::Blendblock,
                     renderable->getDatablock()->getBlendblock( false )->mLifetimeId );

        Technique *technique = mat->getBestTechnique( renderable->getCurrentMaterialLod(), renderable );
        Pass *pass = technique->getPass( 0 );

        // (Jahshaka, SHADER-SYSTEM-1 stage 7a) THE KEY IS THE PIPELINE's CONTENT: the programs (each stage's identity
        // AND its load generation - Resource::getStateCount moves on every load, so a reloaded program is a new key and
        // never keeps the pipeline of its old code), the blocks (above) and the vertex input (below). It was the PASS ID:
        // a material clone (a per-scene sky, the same programs and blocks) built a pipeline of its own, and a reloaded
        // program kept its old one (an SMAA preset change never reached the picture). Everything else the pipeline holds
        // is the pass's (its hash is the final hash's other half), and per-material data binds at draw (executeCommand).
        setJahProgramKey( pass );

        setJahVertexKey( renderable, false );
        outHash = this->addRenderableCache( mT[kNoTid].setProperties, (const PiecesMap *)0 );

        setProperty( kNoTid, HlmsBaseProp::ShadowCaster, true );
        setProperty( kNoTid, HlmsPsoProp::Macroblock,
                     renderable->getDatablock()->getMacroblock( true )->mLifetimeId );
        setProperty( kNoTid, HlmsPsoProp::Blendblock,
                     renderable->getDatablock()->getBlendblock( true )->mLifetimeId );
        setJahVertexKey( renderable, true );

        outCasterHash = this->addRenderableCache( mT[kNoTid].setProperties, (const PiecesMap *)0 );
    }
    //-----------------------------------------------------------------------------------
    HlmsCache HlmsLowLevel::preparePassHash( const CompositorShadowNode *shadowNode, bool casterPass,
                                             bool dualParaboloid, SceneManager *sceneManager )
    {
        mCurrentSceneManager = sceneManager;

        HlmsCache retVal = Hlms::preparePassHash( shadowNode, casterPass, dualParaboloid, sceneManager );

        // (Jahshaka) THE LOW-LEVEL PASS KEY IS WHAT ITS PIPELINE READS: the pass's PSO (targets, depth, samples,
        // stencil, views) and whether it is a caster pass (createShaderCacheEntry's one read of the pass's
        // properties: the caster macroblock and blendblock). The base keys the pass on every property a LIT Hlms
        // derives from the scene - its lights per type, the shadow node's splits and maps - which no material
        // script's program reads, so one quad drawn in a scene with another light set (the editor's first,
        // not yet lit frame against the startup gate's) built a second pipeline for the same PSO.
        const int32 caster = getProperty( retVal.setProperties, HlmsBaseProp::ShadowCaster );
        PassCache keyed;
        keyed.passPso = retVal.pso.pass;
        if( caster )
            keyed.properties.push_back( HlmsProperty( HlmsBaseProp::ShadowCaster, caster ) );
        size_t passIdx = 0u;
        findOrAddPassCache( keyed, true, passIdx );
        retVal.hash = static_cast<uint32>( passIdx ) << HlmsBits::PassShift;
        retVal.setProperties = keyed.properties;

        // Never produce a HlmsCache.hash of 0. Only affects LowLevel because HLMS_LOW_LEVEL = 0
        retVal.hash += 1;

        // TODO: Update auto params here
        const Camera *camera = sceneManager->getCamerasInProgress().renderingCamera;
        mAutoParamDataSource->setCurrentCamera( camera );
        mAutoParamDataSource->setCurrentSceneManager( sceneManager );
        mAutoParamDataSource->setCurrentShadowNode( shadowNode );
        mAutoParamDataSource->setCurrentViewport( sceneManager->getCurrentViewport0() );

        return retVal;
    }
    //-----------------------------------------------------------------------------------
    uint32 HlmsLowLevel::fillBuffersFor( const HlmsCache *cache,
                                         const QueuedRenderable &queuedRenderable, bool casterPass,
                                         uint32 lastCacheHash, uint32 lastTextureHash )
    {
        executeCommand( queuedRenderable.movableObject, queuedRenderable.renderable, casterPass );
        return 0;
    }
    //-----------------------------------------------------------------------------------
    uint32 HlmsLowLevel::fillBuffersForV1( const HlmsCache *cache,
                                           const QueuedRenderable &queuedRenderable, bool casterPass,
                                           uint32 lastCacheHash, CommandBuffer *commandBuffer )
    {
        *commandBuffer->addCommand<CbLowLevelMaterial>() = CbLowLevelMaterial(
            casterPass, this, queuedRenderable.movableObject, queuedRenderable.renderable );
        return 0;
    }
    //-----------------------------------------------------------------------------------
    uint32 HlmsLowLevel::fillBuffersForV2( const HlmsCache *cache,
                                           const QueuedRenderable &queuedRenderable, bool casterPass,
                                           uint32 lastCacheHash, CommandBuffer *commandBuffer )
    {
        *commandBuffer->addCommand<CbLowLevelMaterial>() = CbLowLevelMaterial(
            casterPass, this, queuedRenderable.movableObject, queuedRenderable.renderable );

        return 0;
    }
    //-----------------------------------------------------------------------------------
    void HlmsLowLevel::executeCommand( const MovableObject *movableObject, Renderable *renderable,
                                       bool casterPass )
    {
        unsigned short numMatrices = 1;
        if( renderable->getVaos( static_cast<VertexPass>( casterPass ) ).empty() )
        {
            numMatrices = renderable->getNumWorldTransforms();
            renderable->getWorldTransforms( mTempXform );
        }
        else
        {
            if( !renderable->hasSkeletonAnimation() )
            {
                mTempXform[0] = movableObject->_getParentNodeFullTransform();
            }
            else
            {
                SkeletonInstance *skeleton = movableObject->getSkeletonInstance();
#if OGRE_DEBUG_MODE
                assert( dynamic_cast<const RenderableAnimated *>( renderable ) );
#endif

                const RenderableAnimated *renderableAnimated =
                    static_cast<const RenderableAnimated *>( renderable );

                const RenderableAnimated::IndexMap *indexMap =
                    renderableAnimated->getBlendIndexToBoneIndexMap();

                assert( indexMap->size() < 256 &&
                        "Up to 256 bones per submesh are supported for low level materials!" );

                RenderableAnimated::IndexMap::const_iterator itBone = indexMap->begin();
                RenderableAnimated::IndexMap::const_iterator enBone = indexMap->end();

                size_t matIdx = 0;
                while( itBone != enBone )
                {
                    const SimpleMatrixAf4x3 &mat4x3 = skeleton->_getBoneFullTransform( *itBone );
                    mat4x3.store( &mTempXform[matIdx++] );

                    ++itBone;
                }
            }
        }

        const MaterialPtr &mat = renderable->getMaterial();
        Technique *technique = mat->getBestTechnique( renderable->getCurrentMaterialLod(), renderable );
        const Pass *pass = technique->getPass( 0 );

        mAutoParamDataSource->setCurrentRenderable( renderable );
        mAutoParamDataSource->setWorldMatrices( mTempXform, numMatrices );
        mAutoParamDataSource->setCurrentPass( pass );
        mAutoParamDataSource->setCurrentLightList( &renderable->getLights() );

#if OGRE_COMPILER == OGRE_COMPILER_MSVC
#    pragma warning( push, 0 )
#else
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        if( pass->getFogOverride() )
        {
            mAutoParamDataSource->setFog( pass->getFogMode(), pass->getFogColour(),
                                          pass->getFogDensity(), pass->getFogStart(),
                                          pass->getFogEnd() );
        }
        else
        {
            mAutoParamDataSource->setFog(
                mCurrentSceneManager->getFogMode(), mCurrentSceneManager->getFogColour(),
                mCurrentSceneManager->getFogDensity(), mCurrentSceneManager->getFogStart(),
                mCurrentSceneManager->getFogEnd() );
        }
#if OGRE_COMPILER == OGRE_COMPILER_MSVC
#    pragma warning( pop )
#else
#    pragma GCC diagnostic pop
#endif

        Pass::ConstTextureUnitStateIterator texIter = pass->getTextureUnitStateIterator();
        size_t unit = 0;
        while( texIter.hasMoreElements() )
        {
            TextureUnitState *pTex = texIter.getNext();
            if( !casterPass && pTex->getContentType() != TextureUnitState::CONTENT_SHADOW )
            {
                // Manually set texture projector for shaders if present
                // This won't get set any other way if using manual projection
                const TextureUnitState::EffectMap &effectMap = pTex->getEffects();
                TextureUnitState::EffectMap::const_iterator effi =
                    effectMap.find( TextureUnitState::ET_PROJECTIVE_TEXTURE );
                if( effi != effectMap.end() )
                    mAutoParamDataSource->setTextureProjector( effi->second.frustum, unit );
            }

            if( pTex->getContentType() == TextureUnitState::CONTENT_COMPOSITOR )
            {
                const CompositorTextureVec &compositorTextures =
                    mCurrentSceneManager->getCompositorTextures();
                CompositorTextureVec::const_iterator itor =
                    std::find( compositorTextures.begin(), compositorTextures.end(),
                               pTex->getReferencedTextureName() );

                if( itor == compositorTextures.end() )
                {
                    OGRE_EXCEPT( Exception::ERR_ITEM_NOT_FOUND,
                                 "Invalid compositor content_type compositor name '" +
                                     pTex->getReferencedTextureName().getFriendlyText() + "'",
                                 "HlmsLowLevel::fillBuffersFor" );
                }

                pTex->_setTexturePtr( itor->texture );
            }

            mRenderSystem->_setTextureUnitSettings( unit, *pTex );
            ++unit;
        }

        // Disable remaining texture units
        // mRenderSystem->_disableTextureUnitsFrom( pass->getNumTextureUnitStates() );

        pass->_updateAutoParams( mAutoParamDataSource, GPV_ALL );

        if( pass->hasVertexProgram() )
        {
            mRenderSystem->bindGpuProgramParameters( GPT_VERTEX_PROGRAM,
                                                     pass->getVertexProgramParameters(), GPV_ALL );
        }
        if( pass->hasGeometryProgram() )
        {
            mRenderSystem->bindGpuProgramParameters( GPT_GEOMETRY_PROGRAM,
                                                     pass->getGeometryProgramParameters(), GPV_ALL );
        }
        if( pass->hasTessellationHullProgram() )
        {
            mRenderSystem->bindGpuProgramParameters(
                GPT_HULL_PROGRAM, pass->getTessellationHullProgramParameters(), GPV_ALL );
        }
        if( pass->hasTessellationDomainProgram() )
        {
            mRenderSystem->bindGpuProgramParameters(
                GPT_DOMAIN_PROGRAM, pass->getTessellationDomainProgramParameters(), GPV_ALL );
        }
        if( pass->hasFragmentProgram() )
        {
            mRenderSystem->bindGpuProgramParameters( GPT_FRAGMENT_PROGRAM,
                                                     pass->getFragmentProgramParameters(), GPV_ALL );
        }
    }
    //-----------------------------------------------------------------------------------
    HlmsDatablock *HlmsLowLevel::createDatablockImpl( IdString datablockName,
                                                      const HlmsMacroblock *macroblock,
                                                      const HlmsBlendblock *blendblock,
                                                      const HlmsParamVec &paramVec )
    {
        return OGRE_NEW HlmsLowLevelDatablock( datablockName, this, macroblock, blendblock, paramVec );
    }
    //-----------------------------------------------------------------------------------
    void HlmsLowLevel::setupRootLayout( RootLayout &rootLayout, size_t tid ) {}
}  // namespace Ogre
