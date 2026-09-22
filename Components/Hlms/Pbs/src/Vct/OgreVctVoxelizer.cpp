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

#include "Vct/OgreVctVoxelizer.h"

#include "Vct/OgreVctMaterial.h"
#include "Vct/OgreVoxelVisualizer.h"

#include "Compute/OgreComputeTools.h"
#include "OgreHlmsCompute.h"
#include "OgreHlmsComputeJob.h"
#include "OgreItem.h"
#include "OgreLogManager.h"
#include "OgreLwString.h"
#include "OgreMaterial.h"
#include "OgreMaterialManager.h"
#include "OgreMesh2.h"
#include "OgrePixelFormatGpuUtils.h"
#include "OgreProfiler.h"
#include "OgreRenderSystem.h"
#include "OgreRoot.h"
#include "OgreSceneManager.h"
#include "OgreStringConverter.h"
#include "OgreSubMesh2.h"
#include "OgreTextureGpuManager.h"
#include "Vao/OgreIndexBufferPacked.h"
#include "Vao/OgreReadOnlyBufferPacked.h"
#include "Vao/OgreVertexBufferPacked.h"
#include "Vao/OgreStagingBuffer.h"
#include "Vao/OgreUavBufferPacked.h"
#include "Vao/OgreVaoManager.h"
#include "Vao/OgreVertexArrayObject.h"

#define TODO_deal_no_index_buffer

namespace Ogre
{
    static const uint32 c_numVctProperties = 2u;

    struct VctVoxelizerProp
    {
        static const IdString HasDiffuseTex;
        static const IdString HasEmissiveTex;

        static const IdString *AllProps[c_numVctProperties];
    };

    const IdString VctVoxelizerProp::HasDiffuseTex = IdString( "has_diffuse_tex" );
    const IdString VctVoxelizerProp::HasEmissiveTex = IdString( "has_emissive_tex" );

    const IdString *VctVoxelizerProp::AllProps[c_numVctProperties] = {
        &VctVoxelizerProp::HasDiffuseTex,
        &VctVoxelizerProp::HasEmissiveTex,
    };
    //-------------------------------------------------------------------------
    VctVoxelizer::VctVoxelizer( IdType id, RenderSystem *renderSystem, HlmsManager *hlmsManager,
                                bool correctAreaLightShadows ) :
        VctVoxelizerSourceBase( id, renderSystem, hlmsManager ),
        mAabbWorldSpaceJob( 0 ),
        mMergeAccumTex( 0 ),
        mTotalNumInstances( 0 ),
        mCpuInstanceBuffer( 0 ),
        mInstanceBuffer( 0 ),
        mInstanceBufferAsTex( 0 ),
        mGeometryBuffer( 0 ),
        mNumPartSubMeshes( 0 ),
        mGpuPartitionedSubMeshes( 0 ),
        mMeshAabb( 0 ),
        mNeedsAlbedoMipmaps( correctAreaLightShadows ),
        mNeedsAllMipmaps( false ),
        mDefaultIndexCountSplit( 2001u
                                 /*std::numeric_limits<uint32>::max()*/ ),
        mComputeTools( new ComputeTools( hlmsManager->getComputeHlms() ) ),
        mVctMaterial( new VctMaterial( id, renderSystem->getVaoManager(),
                                       Root::getSingleton().getCompositorManager2(),
                                       renderSystem->getTextureGpuManager() ) ),
        mAutoRegion( true ),
        mMaxRegion( Aabb::BOX_INFINITE ),
        mNumOctantsX( 0u ),
        mNumOctantsY( 0u ),
        mNumOctantsZ( 0u )
    {
        memset( mComputeJobs, 0, sizeof( mComputeJobs ) );
        mAabbCalculator = 0;
        createComputeJobs();

        // A device with no buffer device addresses cannot voxelize at all any more:
        // the shader dereferences the raster's vertex and index pools. Say so ONCE,
        // loudly, instead of producing empty volumes - the cure is a driver with
        // VK_KHR_buffer_device_address, which every target this engine ships on has.
        if( !mVaoManager->supportsBufferDeviceAddress() )
        {
            LogManager::getSingleton().logMessage(
                "WARNING: VctVoxelizer needs buffer device addresses (the compute shader "
                "reads the raster's own vertex and index buffers) and this device has none. "
                "Voxel-cone GI will be empty.",
                LML_CRITICAL );
        }
    }
    //-------------------------------------------------------------------------
    VctVoxelizer::~VctVoxelizer()
    {
        setDebugVisualization( DebugVisualizationNone, 0 );
        destroyVoxelTextures();
        freeBuffers( true );
        destroyInstanceBuffers();

        delete mVctMaterial;
        mVctMaterial = 0;

        delete mComputeTools;
        mComputeTools = 0;
    }
    //-------------------------------------------------------------------------
    /// THE VAO OF ONE LOD LEVEL, and the ONE place this file resolves which one a
    /// (SubMesh, level) request means. The level arrives PER ITEM (ATOM P4 / AT-A10).
    ///
    /// `mVao[VpNormal]` is the mesh's LOD chain, finest first, and it always has
    /// at least one entry: a mesh with no chain has exactly one and every level
    /// clamps back onto it, which is why this is a drop-in for the
    /// `.front()` this class used everywhere.
    static VertexArrayObject *getLodVao( const SubMesh *subMesh, uint32 lodLevel )
    {
        const VertexArrayObjectArray &vaos = subMesh->mVao[VpNormal];
        if( vaos.empty() )
            return 0;
        const size_t idx = std::min<size_t>( lodLevel, vaos.size() - 1u );
        return vaos[idx];
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::createComputeJobs()
    {
        HlmsCompute *hlmsCompute = mHlmsManager->getComputeHlms();

        HlmsComputeJob *voxelizerJob = hlmsCompute->findComputeJobNoThrow( "VCT/Voxelizer" );

#if OGRE_NO_JSON
        OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                     "To use VctVoxelizer, Ogre must be build with JSON support "
                     "and you must include the resources bundled at "
                     "Samples/Media/VCT",
                     "VctVoxelizer::createComputeJobs" );
#endif
        if( !voxelizerJob )
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                         "To use VctVoxelizer, you must include the resources bundled at "
                         "Samples/Media/VCT\n"
                         "Could not find VCT/Voxelizer",
                         "VctVoxelizer::createComputeJobs" );
        }

        uint32 numVariants = 1u << c_numVctProperties;

        char tmpBuffer[128];
        LwString jobName( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );

        for( uint32 variant = 0u; variant < numVariants; ++variant )
        {
            jobName.clear();
            jobName.a( "VCT/Voxelizer/", variant );

            mComputeJobs[variant] = hlmsCompute->findComputeJobNoThrow( jobName.c_str() );

            if( !mComputeJobs[variant] )
            {
                mComputeJobs[variant] = voxelizerJob->clone( jobName.c_str() );

                ShaderParams &glslShaderParams = mComputeJobs[variant]->getShaderParams( "glsl" );

                uint8 numTexUnits = 1u;
                if( variant &
                    ( VoxelizerJobSetting::HasDiffuseTex | VoxelizerJobSetting::HasEmissiveTex ) )
                {
                    ShaderParams::Param param;
                    param.name = "texturePool";
                    param.setManualValue( static_cast<int32>(
                        numTexUnits + mComputeJobs[variant]->_getRawGlTexSlotStart() ) );
                    glslShaderParams.mParams.push_back( param );
                    glslShaderParams.setDirty();
                    ++numTexUnits;
                }
                mComputeJobs[variant]->setNumTexUnits( numTexUnits );

                for( uint32 property = 0; property < c_numVctProperties; ++property )
                {
                    const int32 propValue = variant & ( 1u << property ) ? 1 : 0;
                    mComputeJobs[variant]->setProperty( *VctVoxelizerProp::AllProps[property],
                                                        propValue );
                }
            }
        }

        // ONE AABB CALCULATOR. Its two variants were the index width and the vertex
        // packing; both are data in a geometry row now, so there is one job and one
        // dispatch over all the partitions instead of four ranges.
        mAabbCalculator = hlmsCompute->findComputeJob( "VCT/AabbCalculator" );

        const RenderSystemCapabilities *caps = mRenderSystem->getCapabilities();
        mAabbCalculator->setThreadsPerGroup( caps->getMaxThreadsPerThreadgroupAxis()[0], 1u, 1u );

        mAabbWorldSpaceJob = hlmsCompute->findComputeJob( "VCT/AabbWorldSpace" );
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::clearComputeJobResources( bool calculatorDataOnly )
    {
        // Do not leave dangling pointers when destroying buffers, even if we later set them
        // with a new pointer (if malloc reuses an address and the jobs weren't cleared, we're screwed)
        if( !calculatorDataOnly )
        {
            for( size_t i = 0; i < sizeof( mComputeJobs ) / sizeof( mComputeJobs[0] ); ++i )
            {
                mComputeJobs[i]->clearUavBuffers();
                mComputeJobs[i]->clearTexBuffers();
            }
        }

        mAabbCalculator->clearUavBuffers();
        mAabbCalculator->clearTexBuffers();

        mAabbWorldSpaceJob->clearTexBuffers();
        mAabbWorldSpaceJob->clearUavBuffers();
    }
    //-------------------------------------------------------------------------
    /// JAHSHAKA TEST HOOK: refuse every (mesh, level, submesh), which is the state a
    /// device with no buffer device addresses is in for every mesh in the scene. It is
    /// unreachable on hardware this engine ships on, and the path it exercises - a
    /// build with items queued and NO geometry - used to segfault, so it needs a way to
    /// be tested. Read per build, like the GI cascade fault hooks on the host side: a
    /// getenv against a build that costs milliseconds is not a cost anyone can measure.
    static bool jahRefuseGeometry()
    {
        return getenv( "JAH_VCT_REFUSE_GEOMETRY" ) != 0;
    }
    //-------------------------------------------------------------------------
    /// The normal / uv formats a geometry row can name, mirrored in
    /// Voxelizer_piece_cs.any. A FORMAT IS DATA, NOT A SHADER PERMUTATION: the old
    /// `compressed_vertex_format` property existed only because this class chose the
    /// format itself when it repacked every mesh.
    namespace VoxelizerGeomFlag
    {
        enum VoxelizerGeomFlag
        {
            Index32bit = 1u << 0u,

            NormalShift = 4u,
            NormalNone = 0u << 4u,
            NormalFloat3 = 1u << 4u,
            NormalShort4Snorm = 2u << 4u,
            NormalHalf4 = 3u << 4u,

            UvShift = 8u,
            UvNone = 0u << 8u,
            UvFloat2 = 1u << 8u,
            UvHalf2 = 2u << 8u,
        };
    }
    //-------------------------------------------------------------------------
    /// THE ONE PLACE A (mesh, level, submesh)'s GEOMETRY IS DESCRIBED.
    ///
    /// It replaces `countBuffersSize` and `convertMeshUncompressed`: there is nothing
    /// to count (no private buffer is sized) and nothing to convert (no vertex is
    /// downloaded, repacked or uploaded). What is produced is a GeometryRow - two
    /// device addresses and a layout - plus this (submesh, level)'s partitions.
    void VctVoxelizer::describeMeshLevel( const MeshPtr &mesh, QueuedMesh &queuedMesh,
                                          uint32 level )
    {
        QueuedMeshLevel &lvl = queuedMesh.levels[level];
        const unsigned numSubmeshes = mesh->getNumSubMeshes();
        lvl.submeshes.resize( numSubmeshes );

        for( unsigned subMeshIdx = 0; subMeshIdx < numSubmeshes; ++subMeshIdx )
        {
            QueuedSubMesh &qsm = lvl.submeshes[subMeshIdx];
            qsm.geomRow = 0xFFFFFFFFu;
            qsm.partSubMeshes.clear();

            SubMesh *subMesh = mesh->getSubMesh( (uint16)subMeshIdx );
            VertexArrayObject *vao = getLodVao( subMesh, level );
            if( !vao )
                continue;
            IndexBufferPacked *indexBuffer = vao->getIndexBuffer();
            if( !indexBuffer )
            {
                TODO_deal_no_index_buffer;
                continue;
            }

            size_t posSource = 0u, posOffset = 0u;
            const VertexElement2 *posElem =
                vao->findBySemantic( VES_POSITION, posSource, posOffset );
            if( jahRefuseGeometry() )
            {
                LogManager::getSingleton().logMessage(
                    "WARNING: JAH_VCT_REFUSE_GEOMETRY: refusing mesh '" + mesh->getName() +
                        "'. It will not contribute to GI.",
                    LML_CRITICAL );
                continue;
            }
            if( !posElem || posElem->mType != VET_FLOAT3 ||
                posSource >= vao->getVertexBuffers().size() )
            {
                LogManager::getSingleton().logMessage(
                    "WARNING: Mesh '" + mesh->getName() +
                        "' has no float3 VES_POSITION the voxelizer can read in place. It "
                        "will not contribute to GI.",
                    LML_CRITICAL );
                continue;
            }

            VertexBufferPacked *vertexBuffer = vao->getVertexBuffers()[posSource];
            const uint64 posAddress = mVaoManager->getBufferDeviceAddress( vertexBuffer );
            const uint64 rawIdxAddress = mVaoManager->getBufferDeviceAddress( indexBuffer );
            if( !posAddress || !rawIdxAddress )
                continue;  // no addresses on this device; the ctor already said so

            GeometryRow row;
            memset( &row, 0, sizeof( row ) );
            row.vertexStride = vertexBuffer->getBytesPerElement();
            row.posOffset = uint32( posOffset );
            row.normalOffset = 0xFFFFFFFFu;
            row.uvOffset = 0xFFFFFFFFu;
            row.flags = indexBuffer->getIndexType() == IndexBufferPacked::IT_32BIT
                            ? VoxelizerGeomFlag::Index32bit
                            : 0u;

            // A SEMANTIC IN ANOTHER SOURCE BUFFER IS TREATED AS ABSENT, which is
            // exactly what the download path did when its helper returned no data for
            // one (normal fell back to UNIT_Y, uv to zero). One row carries one vertex
            // address; a second source would need a second, and no mesh this engine
            // bakes has one.
            size_t normSource = 0u, normOffset = 0u;
            const VertexElement2 *normElem =
                vao->findBySemantic( VES_NORMAL, normSource, normOffset );
            if( normElem && normSource == posSource )
            {
                uint32 fmt = VoxelizerGeomFlag::NormalNone;
                if( normElem->mType == VET_FLOAT3 || normElem->mType == VET_FLOAT4 )
                    fmt = VoxelizerGeomFlag::NormalFloat3;
                else if( normElem->mType == VET_SHORT4_SNORM )
                    fmt = VoxelizerGeomFlag::NormalShort4Snorm;
                else if( normElem->mType == VET_HALF4 )
                    fmt = VoxelizerGeomFlag::NormalHalf4;
                if( fmt != VoxelizerGeomFlag::NormalNone )
                {
                    row.flags |= fmt;
                    row.normalOffset = uint32( normOffset );
                }
            }

            size_t uvSource = 0u, uvOffset = 0u;
            const VertexElement2 *uvElem =
                vao->findBySemantic( VES_TEXTURE_COORDINATES, uvSource, uvOffset );
            if( uvElem && uvSource == posSource )
            {
                uint32 fmt = VoxelizerGeomFlag::UvNone;
                if( uvElem->mType == VET_FLOAT2 || uvElem->mType == VET_FLOAT3 ||
                    uvElem->mType == VET_FLOAT4 )
                    fmt = VoxelizerGeomFlag::UvFloat2;
                else if( uvElem->mType == VET_HALF2 || uvElem->mType == VET_HALF4 )
                    fmt = VoxelizerGeomFlag::UvHalf2;
                if( fmt != VoxelizerGeomFlag::UvNone )
                {
                    row.flags |= fmt;
                    row.uvOffset = uint32( uvOffset );
                }
            }

            // A buffer_reference of uints must sit on a 4-byte boundary and a 16-bit
            // index buffer can start on an odd uint16. Floor the address and carry the
            // remainder as an ELEMENT bias, so the shader's index arithmetic stays
            // whole-element and nothing is copied to make it align.
            const uint32 idxBytes = indexBuffer->getBytesPerElement();
            const uint64 flooredIdx = rawIdxAddress & ~uint64( 3u );
            row.idxBias = uint32( ( rawIdxAddress - flooredIdx ) / idxBytes );
            row.posAddress[0] = uint32( posAddress & 0xFFFFFFFFu );
            row.posAddress[1] = uint32( posAddress >> 32u );
            row.idxAddress[0] = uint32( flooredIdx & 0xFFFFFFFFu );
            row.idxAddress[1] = uint32( flooredIdx >> 32u );

            // THE SHADER'S ADDRESS ARITHMETIC IS IN 4-BYTE LANES (`GEOM_LANE` does a
            // `>> 2` over `vertexIdx * stride + offset`), so an odd stride or an odd
            // element offset would not fail - it would read the WRONG BYTES, silently,
            // and only for that one mesh. Refuse it with a reason instead. Every layout
            // this engine bakes is 48 or 68 bytes with 4-aligned elements; a format that
            // is not wants the row widened to carry a byte shift, not this loosened.
            const bool bAligned =
                ( row.vertexStride & 3u ) == 0u && ( row.posOffset & 3u ) == 0u &&
                ( row.normalOffset == 0xFFFFFFFFu || ( row.normalOffset & 3u ) == 0u ) &&
                ( row.uvOffset == 0xFFFFFFFFu || ( row.uvOffset & 3u ) == 0u );
            if( !bAligned )
            {
                LogManager::getSingleton().logMessage(
                    "WARNING: Mesh '" + mesh->getName() +
                        "' has a vertex stride or element offset that is not a multiple of 4 "
                        "bytes; the voxelizer reads vertices in 4-byte lanes and will not "
                        "read this mesh. It will not contribute to GI.",
                    LML_CRITICAL );
                continue;
            }

            qsm.geomRow = uint32( mCpuGeometry.size() );
            mCpuGeometry.push_back( row );

            // The partitions. A mesh with a lot of triangles would have every voxel of
            // the octant test every triangle; splitting the index range and giving each
            // piece its own AABB is the broadphase. Unchanged in intent - only the
            // offsets are now relative to the level's own index buffer instead of to a
            // private concatenation of every mesh's.
            const uint32 firstIndex = vao->getPrimitiveStart();
            const uint32 numIndices = vao->getPrimitiveCount();
            const uint32 numPartitions =
                queuedMesh.indexCountSplit == std::numeric_limits<uint32>::max()
                    ? 1u
                    : alignToNextMultiple( numIndices, queuedMesh.indexCountSplit ) /
                          queuedMesh.indexCountSplit;
            qsm.partSubMeshes.resize( numPartitions );
            for( uint32 partition = 0u; partition < numPartitions; ++partition )
            {
                PartitionedSubMesh &partSubMesh = qsm.partSubMeshes[partition];
                partSubMesh.firstIndex = firstIndex + queuedMesh.indexCountSplit * partition;
                partSubMesh.numIndices = std::min(
                    numIndices - queuedMesh.indexCountSplit * partition, queuedMesh.indexCountSplit );
                partSubMesh.aabbSubMeshIdx = mNumPartSubMeshes;
                ++mNumPartSubMeshes;
            }
        }
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::prepareAabbCalculatorMeshData()
    {
        OgreProfile( "VctVoxelizer::prepareAabbCalculatorMeshData" );

        destroyAabbCalculatorMeshData();
        const size_t totalNumMeshes = mNumPartSubMeshes;
        if( !totalNumMeshes )
            return;
        mMeshAabb = mVaoManager->createUavBuffer( totalNumMeshes, sizeof( float ) * 4u * 2u,
                                                  BB_FLAG_READONLY, 0, false );

        // ONE ARRAY, IN aabbSubMeshIdx ORDER. It used to be four ranges (16/32-bit x
        // packed/unpacked) because each range was a different shader variant reading a
        // different pair of buffers; there is one calculator now, so a partition's index
        // IS its place in this array and the four starts and their four asserts are gone.
        struct GpuPartitionedSubMesh
        {
            uint32 geomRow;
            uint32 firstIndex;
            uint32 numIndices;
            uint32 padding;
        };
        GpuPartitionedSubMesh *partitionedSubMeshGpu = reinterpret_cast<GpuPartitionedSubMesh *>(
            OGRE_MALLOC_SIMD( totalNumMeshes * sizeof( GpuPartitionedSubMesh ),
                              MEMCATEGORY_GEOMETRY ) );
        FreeOnDestructor partitionedSubMeshGpuPtr( partitionedSubMeshGpu );
        memset( partitionedSubMeshGpu, 0, totalNumMeshes * sizeof( GpuPartitionedSubMesh ) );

        MeshPtrMap::iterator itor = mMeshesV2.begin();
        MeshPtrMap::iterator end = mMeshesV2.end();

        while( itor != end )
        {
            QueuedMesh &queuedMesh = itor->second;
            const size_t numLevels = queuedMesh.levels.size();
            for( size_t level = 0u; level < numLevels; ++level )
            {
                if( !queuedMesh.levels[level].wanted )
                    continue;
                QueuedSubMeshArray &submeshes = queuedMesh.levels[level].submeshes;
                const size_t numSubMeshes = submeshes.size();
                for( size_t i = 0u; i < numSubMeshes; ++i )
                {
                    FastArray<PartitionedSubMesh>::const_iterator itPartSub =
                        submeshes[i].partSubMeshes.begin();
                    FastArray<PartitionedSubMesh>::const_iterator enPartSub =
                        submeshes[i].partSubMeshes.end();
                    while( itPartSub != enPartSub )
                    {
                        OGRE_ASSERT_LOW( itPartSub->aabbSubMeshIdx < totalNumMeshes );
                        GpuPartitionedSubMesh &dst =
                            partitionedSubMeshGpu[itPartSub->aabbSubMeshIdx];
                        dst.geomRow = submeshes[i].geomRow;
                        dst.firstIndex = itPartSub->firstIndex;
                        dst.numIndices = itPartSub->numIndices;
                        // VCT/AabbWorldSpace reads aabbSubMeshIdx from
                        // InstanceBuffer::meshData.w & ~0x80000000u, not from here.
                        dst.padding = 0u;
                        ++itPartSub;
                    }
                }
            }
            ++itor;
        }

        mGpuPartitionedSubMeshes = mVaoManager->createTexBuffer(
            PFG_RGBA32_UINT, totalNumMeshes * sizeof( GpuPartitionedSubMesh ), BT_DEFAULT,
            partitionedSubMeshGpuPtr.ptr, false );
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::destroyAabbCalculatorMeshData()
    {
        // if( mGpuMeshDataDirty )
        if( mGpuPartitionedSubMeshes )
        {
            mVaoManager->destroyTexBuffer( mGpuPartitionedSubMeshes );
            mGpuPartitionedSubMeshes = 0;
        }
        if( mMeshAabb )
        {
            mVaoManager->destroyUavBuffer( mMeshAabb );
            mMeshAabb = 0;
        }

        clearComputeJobResources( true );
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::addItem( Item *item, uint32 indexCountSplit, uint32 lodLevel )
    {
        const MeshPtr &mesh = item->getMesh();

        if( indexCountSplit == 0u )
            indexCountSplit = mDefaultIndexCountSplit;
        if( indexCountSplit != std::numeric_limits<uint32>::max() )
            indexCountSplit = alignToNextMultiple( indexCountSplit, 3u );

        const unsigned numSubMeshes = mesh->getNumSubMeshes();

        // CLAMP THE LEVEL HERE, so everything downstream - the queue, the histogram,
        // getQueuedIndexCount - agrees about which level this instance really got. The
        // chain's usable length is the SHORTEST any submesh has, so one level index
        // means one level for the whole item.
        uint32 maxLevel = 0u;
        for( unsigned i = 0u; i < numSubMeshes; ++i )
        {
            const VertexArrayObjectArray &vaos = mesh->getSubMesh( (uint16)i )->mVao[VpNormal];
            if( vaos.empty() || !vaos[0]->getIndexBuffer() )
            {
                LogManager::getSingleton().logMessage(
                    "WARNING: Mesh '" + mesh->getName() +
                        "' contains geometry without index buffers. This is currently not "
                        "implemented and cannot be added to VctVoxelizer. GI may not look as "
                        "expected",
                    LML_CRITICAL );
                return;
            }
            const uint32 levels = uint32( vaos.size() ) - 1u;
            maxLevel = i == 0u ? levels : std::min( maxLevel, levels );
        }
        lodLevel = std::min( lodLevel, maxLevel );

        MeshPtrMap::iterator itor = mMeshesV2.find( mesh );
        const bool isNewEntry = itor == mMeshesV2.end();

        QueuedMesh &queuedMesh = mMeshesV2[mesh];
        if( isNewEntry )
        {
            queuedMesh.numItems = 0u;
            queuedMesh.indexCountSplit = indexCountSplit;
        }
        ++queuedMesh.numItems;

        // THE LEVEL IS THE ITEM'S, AND SEVERAL LEVELS OF ONE MESH LIVE SIDE BY SIDE.
        // "The finest request wins" (patch 0064) is deleted together with the private
        // copies that made it necessary: nothing is downloaded per mesh, so a mesh
        // instanced near and far is voxelized at two levels in the same build.
        if( queuedMesh.levels.size() <= lodLevel )
            queuedMesh.levels.resize( lodLevel + 1u );
        queuedMesh.levels[lodLevel].wanted = true;

        QueuedItem queuedItem;
        queuedItem.item = item;
        queuedItem.lodLevel = lodLevel;
        mItems.push_back( queuedItem );
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::removeItem( Item *item )
    {
        ItemArray::iterator itor = mItems.begin();
        ItemArray::iterator endt = mItems.end();
        while( itor != endt && itor->item != item )
            ++itor;
        if( itor == endt )
            OGRE_EXCEPT( Exception::ERR_ITEM_NOT_FOUND, "", "VctVoxelizer::removeItem" );

        const MeshPtr &mesh = item->getMesh();
        MeshPtrMap::iterator itMesh = mMeshesV2.find( mesh );
        if( itMesh == mMeshesV2.end() )
        {
            OGRE_EXCEPT( Exception::ERR_ITEM_NOT_FOUND,
                         "The item was in our records but its mesh wasn't! This should be "
                         "impossible. Was the mesh ptr of the item altered manually?",
                         "VctVoxelizer::removeItem" );
        }
        --itMesh->second.numItems;
        if( !itMesh->second.numItems )
            mMeshesV2.erase( mesh );

        efficientVectorRemove( mItems, itor );
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::removeAllItems()
    {
        mItems.clear();
        mMeshesV2.clear();
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::freeBuffers( bool bForceFree )
    {
        // THE GEOMETRY TABLE IS THE ONLY GEOMETRY BUFFER LEFT. The four it replaces
        // (mVertexBufferUncompressed, mVertexBufferCompressed, mIndexBuffer16,
        // mIndexBuffer32) held a full copy of the world's triangles; this one holds 48
        // bytes per (mesh, level, submesh) and is re-created whenever the number of rows
        // changes, which costs nothing worth keeping.
        if( mGeometryBuffer &&
            ( bForceFree || mGeometryBuffer->getNumElements() != mCpuGeometry.size() ) )
        {
            mVaoManager->destroyUavBuffer( mGeometryBuffer );
            mGeometryBuffer = 0;
        }

        if( bForceFree )
            destroyAabbCalculatorMeshData();

        clearComputeJobResources( false );
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::buildGeometryTable()
    {
        OgreProfile( "VctVoxelizer::buildGeometryTable" );

        mCpuGeometry.clear();
        mNumPartSubMeshes = 0u;

        {
            OgreProfile( "VctVoxelizer::describeMeshLevel aggregated" );
            MeshPtrMap::iterator itor = mMeshesV2.begin();
            MeshPtrMap::iterator end = mMeshesV2.end();

            while( itor != end )
            {
                QueuedMesh &queuedMesh = itor->second;
                const size_t numLevels = queuedMesh.levels.size();
                for( size_t level = 0u; level < numLevels; ++level )
                {
                    if( queuedMesh.levels[level].wanted )
                        describeMeshLevel( itor->first, queuedMesh, uint32( level ) );
                }
                ++itor;
            }
        }

        freeBuffers( false );

        // THERE IS NO DOWNLOAD, NO REPACK, NO STAGING MAP AND NO INDEX copyTo. What used
        // to be a staging buffer of `numVertices * 32` bytes, a per-vertex CPU loop over
        // every mesh in the volume and two GPU-side index copies is one upload of
        // `rows * 48` bytes - and the CPU never sees a vertex.
        if( !mCpuGeometry.empty() )
        {
            if( !mGeometryBuffer )
            {
                mGeometryBuffer = mVaoManager->createUavBuffer(
                    mCpuGeometry.size(), sizeof( GeometryRow ), BB_FLAG_READONLY, 0, false );
            }
            mGeometryBuffer->upload( mCpuGeometry.begin(), 0u, mCpuGeometry.size() );
        }

        prepareAabbCalculatorMeshData();
    }
    //-------------------------------------------------------------------------
    /// Jahshaka patch 0065: the merge accumulator dies with the voxel textures.
    void VctVoxelizer::destroyVoxelTextures()
    {
        if( mMergeAccumTex )
        {
            mTextureGpuManager->destroyTexture( mMergeAccumTex );
            mMergeAccumTex = 0;
        }
        VctVoxelizerSourceBase::destroyVoxelTextures();
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::createVoxelTextures()
    {
        if( mAlbedoVox && mAlbedoVox->getWidth() == mWidth && mAlbedoVox->getHeight() == mHeight &&
            mAlbedoVox->getDepth() == mDepth )
        {
            mAccumValVox->scheduleTransitionTo( GpuResidency::Resident );
            // Jahshaka patch 0071: the merge accumulator is NOT transient. It is
            // created once with the voxel textures and stays Resident until they
            // are destroyed -- see the note at the end of build().
            return;
        }

        const bool hasTypedUavs = mRenderSystem->getCapabilities()->hasCapability( RSC_TYPED_UAV_LOADS );

        if( !mAlbedoVox )
        {
            uint32 texFlags = TextureFlags::Uav;
            if( !hasTypedUavs )
                texFlags |= TextureFlags::Reinterpretable;

            if( mNeedsAlbedoMipmaps || mNeedsAllMipmaps )
                texFlags |= TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps;

            mAlbedoVox = mTextureGpuManager->createTexture(
                "VctVoxelizer" + StringConverter::toString( getId() ) + "/Albedo",
                GpuPageOutStrategy::Discard, texFlags, TextureTypes::Type3D );

            if( !mNeedsAllMipmaps )
                texFlags &= ~uint32( TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps );

            mEmissiveVox = mTextureGpuManager->createTexture(
                "VctVoxelizer" + StringConverter::toString( getId() ) + "/Emissive",
                GpuPageOutStrategy::Discard, texFlags, TextureTypes::Type3D );
            mNormalVox = mTextureGpuManager->createTexture(
                "VctVoxelizer" + StringConverter::toString( getId() ) + "/Normal",
                GpuPageOutStrategy::Discard, texFlags, TextureTypes::Type3D );

            texFlags &= ~uint32( TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps );

            mAccumValVox = mTextureGpuManager->createTexture(
                "VctVoxelizer" + StringConverter::toString( getId() ) + "/AccumVal",
                GpuPageOutStrategy::Discard, TextureFlags::NotTexture | texFlags, TextureTypes::Type3D );

            // Jahshaka patch 0065 — the order-independent merge's accumulator.
            mMergeAccumTex = mTextureGpuManager->createTexture(
                "VctVoxelizer" + StringConverter::toString( getId() ) + "/MergeAccum",
                GpuPageOutStrategy::Discard,
                TextureFlags::NotTexture | TextureFlags::Uav, TextureTypes::Type3D );
        }

        TextureGpu *textures[4] = { mAlbedoVox, mEmissiveVox, mNormalVox, mAccumValVox };
        for( size_t i = 0; i < sizeof( textures ) / sizeof( textures[0] ); ++i )
            textures[i]->scheduleTransitionTo( GpuResidency::OnStorage );

        mAlbedoVox->setPixelFormat( PFG_RGBA8_UNORM );
        // JAHSHAKA PATCH 0087: THE EMISSIVE VOXEL IS A FLOAT.
        //
        // Albedo is a RATIO and lives in [0, 1] by definition, so its UNORM store
        // above is exact. EMISSIVE is a RADIANCE -- W/(m^2 sr), no upper bound
        // worth naming -- and this store was the one place it was clipped: the
        // material store carries it as four honest floats (VctMaterial:
        // shaderMaterial.emissive[i] = emissiveCol[i]) and the merge accumulates
        // it on a fixed-point grid clamped at 16.0 per contribution (patch 0065),
        // but the final imageWrite went into a UNORM8 texel, so an emitter
        // authored at 3.0 was stored as exactly 1.0 and the light injection seeded
        // the radiance volume from that (LightInjection_piece_cs.any:
        // blockColour = emissiveVal.xyz) -- a third of the energy, entering the
        // bounces, the irradiance field, the cones and the ray hits, with nothing
        // in any log and a picture that merely looks dimmer.
        //
        // RGBA16_FLOAT and not 32: half carries 65504 with 11 bits of mantissa,
        // which is finer than the merge's own 1/4096 grid everywhere in [0, 16] --
        // the grid's clamp, not the format, is now the documented ceiling.
        //
        // NOTHING ELSE MOVES. The GLSL image declaration carries no hard-coded
        // format: HlmsComputeJob generates the layout qualifier from the bound
        // texture (uav4_pf_type, OgreHlmsComputeJob.cpp), the typed-UAV write is a
        // float4 either way, ComputeTools::clearUavFloat clears any non-integer
        // format, and every reader loads through a sampled texture3D. The one
        // backend that cannot follow is D3D11 WITHOUT typed UAV loads, whose
        // branch packs the texel into a single uint -- see the note beside it in
        // Voxelizer_piece_cs.any.
        mEmissiveVox->setPixelFormat( PFG_RGBA16_FLOAT );
        mNormalVox->setPixelFormat( PFG_R10G10B10A2_UNORM );
        if( hasTypedUavs )
            mAccumValVox->setPixelFormat( PFG_R16_UINT );
        else
            mAccumValVox->setPixelFormat( PFG_R32_UINT );

        // Jahshaka patch 0065: thirteen texels per voxel, interleaved in Z — the
        // shader derives their coordinates from the voxel's own, so a dispatch
        // that covers one OCTANT needs to know nothing about the volume's depth.
        // R32_UINT rather than RGBA32_UINT because ComputeTools' clear of a
        // 128-bit 3D uav loses the device on this driver (VoxelMerge_piece_cs),
        // and thirteen rather than sixteen because only thirteen carry anything.
        mMergeAccumTex->scheduleTransitionTo( GpuResidency::OnStorage );
        mMergeAccumTex->setPixelFormat( PFG_R32_UINT );
        mMergeAccumTex->setResolution( mWidth, mHeight, mDepth * 13u );
        mMergeAccumTex->setNumMipmaps( 1u );
        mMergeAccumTex->scheduleTransitionTo( GpuResidency::Resident );

        const uint8 numMipmaps = PixelFormatGpuUtils::getMaxMipmapCount( mWidth, mHeight, mDepth );

        for( size_t i = 0; i < sizeof( textures ) / sizeof( textures[0] ); ++i )
        {
            if( textures[i] != mAccumValVox || hasTypedUavs )
                textures[i]->setResolution( mWidth, mHeight, mDepth );
            else
                textures[i]->setResolution( mWidth >> 1u, mHeight, mDepth );
            if( ( ( mNeedsAlbedoMipmaps && i == 0u ) || mNeedsAllMipmaps ) && i < 3u )
                textures[i]->setNumMipmaps( numMipmaps );
            else
                textures[i]->setNumMipmaps( 1u );
            textures[i]->scheduleTransitionTo( GpuResidency::Resident );
        }

        if( mDebugVoxelVisualizer )
        {
            setTextureToDebugVisualizer();
            mDebugVoxelVisualizer->setVisible( true );
        }
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::setRegionToVoxelize( bool autoRegion, const Aabb &regionToVoxelize,
                                            const Aabb &maxRegion )
    {
        mAutoRegion = autoRegion;
        mRegionToVoxelize = regionToVoxelize;
        mMaxRegion = maxRegion;

        // THE OCTANTS DESCRIBE THE REGION, so a region that moves takes them with it.
        // dividideOctants() COPIES mRegionToVoxelize into every octant's own Aabb, and
        // build() then uses that copy twice: placeItemsInBuckets() culls the items
        // against it, and the dispatch passes its minimum as `voxelOrigin` — the world
        // point the voxelisation shader writes from. Leave them behind and the next
        // build() voxelises the geometry of the OLD box, at the OLD origin, into a
        // texture that VctLighting maps onto the NEW one (fillConstBufferData reads
        // getVoxelOrigin() live): a wrong bounce, permanently, with no log line, no
        // validation error and no cost difference.
        //
        // VctImageVoxelizer::setRegionToVoxelize already ends with mOctants.clear() for
        // exactly this reason, and its caller re-divides before building — so this class
        // was the one of the two that did not keep the invariant. Re-deriving (rather
        // than clearing) keeps every existing caller working: a clear would leave
        // build() with an empty octant list, which is an assert in debug and a silently
        // BLACK volume in release.
        if( mNumOctantsX )
            dividideOctants( mNumOctantsX, mNumOctantsY, mNumOctantsZ );
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::autoCalculateRegion()
    {
        if( !mAutoRegion )
            return;

        mRegionToVoxelize = Aabb::BOX_NULL;

        ItemArray::const_iterator itor = mItems.begin();
        ItemArray::const_iterator end = mItems.end();

        while( itor != end )
        {
            mRegionToVoxelize.merge( itor->item->getWorldAabb() );
            ++itor;
        }

        Vector3 minAabb = mRegionToVoxelize.getMinimum();
        Vector3 maxAabb = mRegionToVoxelize.getMaximum();

        minAabb.makeCeil( mMaxRegion.getMinimum() );
        maxAabb.makeFloor( mMaxRegion.getMaximum() );

        if( minAabb.x > maxAabb.x || minAabb.y > maxAabb.y || minAabb.z > maxAabb.z )
        {
            LogManager::getSingleton().logMessage(
                "WARNING: VctVoxelizer::autoCalculateRegion could not calculate a valid bound! GI won't "
                "be available or won't look as expected",
                LML_CRITICAL );
            mRegionToVoxelize.setExtents( Ogre::Vector3::ZERO, Ogre::Vector3::ZERO );
        }
        else
        {
            mRegionToVoxelize.setExtents( minAabb, maxAabb );
        }
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::placeItemsInBuckets()
    {
        OgreProfile( "VctVoxelizer::placeItemsInBuckets" );

        mBuckets.clear();

        ItemArray::const_iterator itor = mItems.begin();
        ItemArray::const_iterator end = mItems.end();

        while( itor != end )
        {
            Item *item = itor->item;
            const uint32 lodLevel = itor->lodLevel;
            MeshPtrMap::const_iterator itMesh = mMeshesV2.find( item->getMesh() );
            OGRE_ASSERT_MEDIUM( itMesh != mMeshesV2.end() );
            OGRE_ASSERT_MEDIUM( lodLevel < itMesh->second.levels.size() );
            const QueuedMeshLevel &meshLevel = itMesh->second.levels[lodLevel];

            const size_t numSubItems = item->getNumSubItems();
            for( size_t i = 0; i < numSubItems; ++i )
            {
                if( i >= meshLevel.submeshes.size() )
                    continue;
                const QueuedSubMesh &qsm = meshLevel.submeshes[i];
                if( qsm.geomRow == 0xFFFFFFFFu )
                    continue;  // describeMeshLevel refused it and said why

                SubItem *subItem = item->getSubItem( i );

                // THE VARIANT IS NOW ONLY ABOUT TEXTURES. The index width and the vertex
                // packing used to be in here, each splitting an octant's dispatch in two;
                // they are fields of the geometry row the shader reads per instance.
                uint32 variant = 0;

                HlmsDatablock *datablock = subItem->getDatablock();
                VctMaterial::DatablockConversionResult convResult =
                    mVctMaterial->addDatablock( datablock );

                if( convResult.hasDiffuseTex() )
                    variant |= VoxelizerJobSetting::HasDiffuseTex;
                if( convResult.hasEmissiveTex() )
                    variant |= VoxelizerJobSetting::HasEmissiveTex;

                VoxelizerBucket bucket;
                bucket.job = mComputeJobs[variant];
                bucket.materialBuffer = convResult.constBuffer;
                bucket.needsTexPool = convResult.hasDiffuseTex() || convResult.hasEmissiveTex();
                // The pointer-free identity the bucket is ORDERED by (see operator<).
                bucket.variant = variant;
                bucket.materialBucketIdx = convResult.bucketIdx;

                QueuedInstance queuedInstance;
                queuedInstance.movableObject = item;
                queuedInstance.materialIdx = convResult.slotIdx;
                queuedInstance.geomRow = qsm.geomRow;
                queuedInstance.lodLevel = lodLevel;

                const size_t numPartitions = qsm.partSubMeshes.size();
                for( size_t j = 0u; j < numPartitions; ++j )
                {
                    const PartitionedSubMesh &partSubMesh = qsm.partSubMeshes[j];
                    queuedInstance.firstIndex = partSubMesh.firstIndex;
                    queuedInstance.numIndices = partSubMesh.numIndices;
                    queuedInstance.aabbSubMeshIdx = partSubMesh.aabbSubMeshIdx;
                    queuedInstance.needsAabbUpdate = numPartitions != 1u || numSubItems != 1u;
                    mBuckets[bucket].queuedInst.push_back( queuedInstance );
                }
            }

            ++itor;
        }
    }
    //-------------------------------------------------------------------------
    size_t VctVoxelizer::countSubMeshPartitionsIn( const QueuedItem &queuedItem ) const
    {
        size_t numSubMeshPartitions = 0;
        MeshPtrMap::const_iterator itMesh = mMeshesV2.find( queuedItem.item->getMesh() );
        OGRE_ASSERT_MEDIUM( itMesh != mMeshesV2.end() );
        OGRE_ASSERT_MEDIUM( queuedItem.lodLevel < itMesh->second.levels.size() );

        const QueuedSubMeshArray &submeshes = itMesh->second.levels[queuedItem.lodLevel].submeshes;
        QueuedSubMeshArray::const_iterator itor = submeshes.begin();
        QueuedSubMeshArray::const_iterator end = submeshes.end();

        while( itor != end )
        {
            numSubMeshPartitions += itor->partSubMeshes.size();
            ++itor;
        }

        return numSubMeshPartitions;
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::createInstanceBuffers()
    {
        size_t instanceCount = 0;
        ItemArray::const_iterator itor = mItems.begin();
        ItemArray::const_iterator end = mItems.end();

        while( itor != end )
        {
            instanceCount += countSubMeshPartitionsIn( *itor );
            ++itor;
        }
        if( !instanceCount )
            return;

        const size_t structStride = sizeof( float ) * 4u * 6u;
        const size_t elementCount = alignToNextMultiple<size_t>(
            instanceCount * mOctants.size(), mAabbWorldSpaceJob->getThreadsPerGroupX() );

        if( !mInstanceBuffer || ( elementCount * structStride ) > mInstanceBuffer->getTotalSizeBytes() )
        {
            destroyInstanceBuffers();
            mInstanceBuffer = mVaoManager->createUavBuffer( elementCount, structStride,
                                                            BB_FLAG_UAV | BB_FLAG_READONLY, 0, false );
            mCpuInstanceBuffer = reinterpret_cast<float *>(
                OGRE_MALLOC_SIMD( elementCount * structStride, MEMCATEGORY_GENERAL ) );
            mInstanceBufferAsTex = mInstanceBuffer->getAsReadOnlyBufferView();
        }
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::destroyInstanceBuffers()
    {
        if( mInstanceBuffer )
        {
            mVaoManager->destroyUavBuffer( mInstanceBuffer );
            mInstanceBuffer = 0;
            mInstanceBufferAsTex = 0;

            OGRE_FREE_SIMD( mCpuInstanceBuffer, MEMCATEGORY_GENERAL );
            mCpuInstanceBuffer = 0;
        }

        clearComputeJobResources( false );
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::fillInstanceBuffers()
    {
        OgreProfile( "VctVoxelizer::fillInstanceBuffers" );

        createInstanceBuffers();
        if( !mInstanceBuffer || !mCpuInstanceBuffer )
        {
            // createInstanceBuffers had nothing to size. build() already decided to
            // clear and return in that case, so reaching here means a new caller: say
            // nothing and touch nothing rather than dereference null.
            mTotalNumInstances = 0u;
            return;
        }

        //        float * RESTRICT_ALIAS instanceBuffer =
        //                reinterpret_cast<float*>( mInstanceBuffer->map( 0,
        //                mInstanceBuffer->getNumElements() ) );
        float *RESTRICT_ALIAS instanceBuffer = reinterpret_cast<float *>( mCpuInstanceBuffer );
        const float *instanceBufferStart = instanceBuffer;
        const FastArray<Octant>::const_iterator begin = mOctants.begin();
        FastArray<Octant>::const_iterator itor = begin;
        FastArray<Octant>::const_iterator end = mOctants.end();

        while( itor != end )
        {
            const Aabb octantAabb = itor->region;
            VoxelizerBucketMap::iterator itBucket = mBuckets.begin();
            VoxelizerBucketMap::iterator enBucket = mBuckets.end();

            while( itBucket != enBucket )
            {
                uint32 numInstancesAfterCulling = 0u;
                FastArray<QueuedInstance>::const_iterator itQueuedInst =
                    itBucket->second.queuedInst.begin();
                FastArray<QueuedInstance>::const_iterator enQueuedInst =
                    itBucket->second.queuedInst.end();

                while( itQueuedInst != enQueuedInst )
                {
                    const QueuedInstance &instance = *itQueuedInst;
                    Aabb worldAabb = instance.movableObject->getWorldAabb();

                    // Perform culling against this octant.
                    if( octantAabb.intersects( worldAabb ) )
                    {
                        const Matrix4 &fullTransform =
                            instance.movableObject->_getParentNodeFullTransform();
                        for( size_t i = 0; i < 12u; ++i )
                            *instanceBuffer++ = static_cast<float>( fullTransform[0][i] );

                        *instanceBuffer++ = worldAabb.mCenter.x;
                        *instanceBuffer++ = worldAabb.mCenter.y;
                        *instanceBuffer++ = worldAabb.mCenter.z;
                        *instanceBuffer++ = 0.0f;

#define AS_U32PTR( x ) reinterpret_cast<uint32 * RESTRICT_ALIAS>( x )

                        *instanceBuffer++ = worldAabb.mHalfSize.x;
                        *instanceBuffer++ = worldAabb.mHalfSize.y;
                        *instanceBuffer++ = worldAabb.mHalfSize.z;
                        *AS_U32PTR( instanceBuffer ) = instance.materialIdx;
                        ++instanceBuffer;

                        uint32 aabbSubMeshIdx = instance.aabbSubMeshIdx;
                        if( instance.needsAabbUpdate )
                            aabbSubMeshIdx |= 0x80000000;

                        // meshData.x is the GEOMETRY ROW (which mesh, which LEVEL, which
                        // submesh), not a vertex offset into a private concatenation:
                        // the row's device address already points at this submesh's own
                        // vertex buffer, so `vertexBufferStart` has nothing to say.
                        *AS_U32PTR( instanceBuffer ) = instance.geomRow;
                        ++instanceBuffer;
                        *AS_U32PTR( instanceBuffer ) = instance.firstIndex;
                        ++instanceBuffer;
                        *AS_U32PTR( instanceBuffer ) = instance.numIndices;
                        ++instanceBuffer;
                        *AS_U32PTR( instanceBuffer ) = aabbSubMeshIdx;
                        ++instanceBuffer;

#undef AS_U32PTR

                        ++numInstancesAfterCulling;
                    }

                    ++itQueuedInst;
                }

                if( numInstancesAfterCulling > 0u )
                {
                    const uint32 octantIdx = static_cast<uint32>( itor - begin );
                    itBucket->second.numInstancesAfterCulling[octantIdx] = numInstancesAfterCulling;
                }

                ++itBucket;
            }

            ++itor;
        }

        OGRE_ASSERT_LOW( (size_t)( instanceBuffer - instanceBufferStart ) * sizeof( float ) <=
                         mInstanceBuffer->getTotalSizeBytes() );

        mTotalNumInstances =
            static_cast<uint32>( ( instanceBuffer - instanceBufferStart ) / ( 4u * ( 3u + 3u ) ) );

        // Fill the remaining bytes with 0 so that mAabbWorldSpaceJob ignores those
        memset( instanceBuffer, 0,
                mInstanceBuffer->getTotalSizeBytes() -
                    ( static_cast<size_t>( instanceBuffer - instanceBufferStart ) * sizeof( float ) ) );
        //        mInstanceBuffer->unmap( UO_UNMAP_ALL );
        mInstanceBuffer->upload( mCpuInstanceBuffer, 0u, mInstanceBuffer->getNumElements() );
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::computeMeshAabbs()
    {
        OgreProfile( "VctVoxelizer::computeMeshAabbs" );
        HlmsCompute *hlmsCompute = mHlmsManager->getComputeHlms();

        // ONE DISPATCH OVER EVERY PARTITION. It used to be up to four, one per
        // (index width x vertex packing) range, each binding a different pair of private
        // buffers; the geometry row says both, per partition, so there is one range.
        if( mNumPartSubMeshes && mGeometryBuffer )
        {
            OgreProfileGpuBegin( "VCT Mesh AABB calculation" );

            DescriptorSetUav::BufferSlot bufferSlot( DescriptorSetUav::BufferSlot::makeEmpty() );
            bufferSlot.buffer = mGeometryBuffer;
            bufferSlot.access = ResourceAccess::Read;
            mAabbCalculator->_setUavBuffer( 0, bufferSlot );
            bufferSlot.buffer = mMeshAabb;
            bufferSlot.access = ResourceAccess::Write;
            mAabbCalculator->_setUavBuffer( 1, bufferSlot );

            DescriptorSetTexture2::BufferSlot texBufSlot(
                DescriptorSetTexture2::BufferSlot::makeEmpty() );
            texBufSlot.buffer = mGpuPartitionedSubMeshes;
            mAabbCalculator->setTexBuffer( 0, texBufSlot );

            const uint32 meshRange[2] = { 0u, mNumPartSubMeshes };
            ShaderParams::Param paramMeshRange;
            paramMeshRange.name = "meshStart_meshEnd";
            paramMeshRange.setManualValue( meshRange, 2u );

            ShaderParams &shaderParams = mAabbCalculator->getShaderParams( "default" );
            shaderParams.mParams.clear();
            shaderParams.mParams.push_back( paramMeshRange );
            shaderParams.setDirty();

            mAabbCalculator->analyzeBarriers( mResourceTransitions );
            mRenderSystem->executeResourceTransition( mResourceTransitions );
            hlmsCompute->dispatch( mAabbCalculator, 0, 0 );

            OgreProfileGpuEnd( "VCT Mesh AABB calculation" );
        }

        if( !mMeshAabb || !mInstanceBuffer )
            return;

        DescriptorSetUav::BufferSlot bufferSlot( DescriptorSetUav::BufferSlot::makeEmpty() );
        bufferSlot.buffer = mInstanceBuffer;
        bufferSlot.access = ResourceAccess::ReadWrite;
        mAabbWorldSpaceJob->_setUavBuffer( 0, bufferSlot );

        DescriptorSetTexture2::BufferSlot texBufSlot( DescriptorSetTexture2::BufferSlot::makeEmpty() );
        texBufSlot.buffer = mMeshAabb->getAsReadOnlyBufferView();
        mAabbWorldSpaceJob->setTexBuffer( 0, texBufSlot );

        const uint32 threadsPerGroupX = mAabbWorldSpaceJob->getThreadsPerGroupX();
        mAabbWorldSpaceJob->setNumThreadGroups(
            ( mTotalNumInstances + threadsPerGroupX - 1u ) / threadsPerGroupX, 1u, 1u );

        OgreProfileGpuBegin( "VCT AABB local to world space conversion" );
        mAabbWorldSpaceJob->analyzeBarriers( mResourceTransitions );
        mRenderSystem->executeResourceTransition( mResourceTransitions );
        hlmsCompute->dispatch( mAabbWorldSpaceJob, 0, 0 );
        OgreProfileGpuEnd( "VCT AABB local to world space conversion" );
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::dividideOctants( uint32 numOctantsX, uint32 numOctantsY, uint32 numOctantsZ )
    {
        // Remembered so that setRegionToVoxelize can re-derive them when the region
        // moves (see the note there).
        mNumOctantsX = numOctantsX;
        mNumOctantsY = numOctantsY;
        mNumOctantsZ = numOctantsZ;

        mOctants.clear();
        mOctants.reserve( numOctantsX * numOctantsY * numOctantsZ );

        OGRE_ASSERT_LOW( mWidth % numOctantsX == 0 );
        OGRE_ASSERT_LOW( mHeight % numOctantsY == 0 );
        OGRE_ASSERT_LOW( mDepth % numOctantsZ == 0 );

        Octant octant;
        octant.width = mWidth / numOctantsX;
        octant.height = mHeight / numOctantsY;
        octant.depth = mDepth / numOctantsZ;

        const Vector3 voxelOrigin = mRegionToVoxelize.getMinimum();
        const Vector3 voxelCellSize = mRegionToVoxelize.getSize() /
                                      Vector3( (Real)numOctantsX, (Real)numOctantsY, (Real)numOctantsZ );

        for( uint32 x = 0u; x < numOctantsX; ++x )
        {
            octant.x = x * octant.width;
            for( uint32 y = 0u; y < numOctantsY; ++y )
            {
                octant.y = y * octant.height;
                for( uint32 z = 0u; z < numOctantsZ; ++z )
                {
                    octant.z = z * octant.depth;

                    Vector3 octantOrigin = Vector3( (Real)x, (Real)y, (Real)z ) * voxelCellSize;
                    octantOrigin += voxelOrigin;
                    octant.region.setExtents( octantOrigin, octantOrigin + voxelCellSize );
                    mOctants.push_back( octant );
                }
            }
        }
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::clearVoxels()
    {
        OgreProfileGpuBegin( "VCT Voxelization Clear" );
        float fClearValue[4];
        uint32 uClearValue[4];
        float fClearNormals[4];
        memset( fClearValue, 0, sizeof( fClearValue ) );
        memset( uClearValue, 0, sizeof( uClearValue ) );
        fClearNormals[0] = 0.5f;
        fClearNormals[1] = 0.5f;
        fClearNormals[2] = 0.5f;
        fClearNormals[3] = 0.0f;

        mResourceTransitions.clear();
        mComputeTools->prepareForUavClear( mResourceTransitions, mAlbedoVox );
        mComputeTools->prepareForUavClear( mResourceTransitions, mEmissiveVox );
        mComputeTools->prepareForUavClear( mResourceTransitions, mNormalVox );
        mComputeTools->prepareForUavClear( mResourceTransitions, mAccumValVox );
        // Jahshaka patch 0065: the sums start at zero, every build.
        mComputeTools->prepareForUavClear( mResourceTransitions, mMergeAccumTex );
        mRenderSystem->executeResourceTransition( mResourceTransitions );

        mComputeTools->clearUavFloat( mAlbedoVox, fClearValue );
        mComputeTools->clearUavFloat( mEmissiveVox, fClearValue );
        mComputeTools->clearUavFloat( mNormalVox, fClearNormals );
        mComputeTools->clearUavUint( mAccumValVox, uClearValue );
        mComputeTools->clearUavUint( mMergeAccumTex, uClearValue );
        OgreProfileGpuEnd( "VCT Voxelization Clear" );
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::setResolution( uint32 width, uint32 height, uint32 depth )
    {
        destroyVoxelTextures();
        mWidth = width;
        mHeight = height;
        mDepth = depth;
    }
    //-------------------------------------------------------------------------
    void VctVoxelizer::build( SceneManager *sceneManager )
    {
        OgreProfile( "VctVoxelizer::build" );
        OgreProfileGpuBegin( "VCT build" );

        OGRE_ASSERT_LOW( !mOctants.empty() );

        mRenderSystem->endRenderPassDescriptor();

        if( mItems.empty() )
        {
            createVoxelTextures();
            clearVoxels();
            return;
        }

        buildGeometryTable();

        createVoxelTextures();

        mVctMaterial->initTempResources( sceneManager );
        placeItemsInBuckets();
        mVctMaterial->destroyTempResources();

        // NOTHING TO VOXELISE, AND THE DECISION IS MADE HERE - before anything
        // allocates or dispatches. `mItems` non-empty does NOT mean there is geometry:
        // describeMeshLevel refuses a (submesh, level) it cannot read (no float3
        // position, an unaligned layout, or - the whole-scene case - a device with no
        // buffer device addresses at all), and a refused row produces no bucket. That
        // state used to walk on into fillInstanceBuffers with a null instance buffer and
        // SEGFAULT, so the honest "GI is empty" path was a crash.
        if( mBuckets.empty() || !mGeometryBuffer )
        {
            mTotalNumInstances = 0u;
            clearVoxels();
            OgreProfileGpuEnd( "VCT build" );
            return;
        }

        fillInstanceBuffers();

        computeMeshAabbs();

        const bool hasTypedUavs = mRenderSystem->getCapabilities()->hasCapability( RSC_TYPED_UAV_LOADS );

        for( size_t i = 0; i < sizeof( mComputeJobs ) / sizeof( mComputeJobs[0] ); ++i )
        {
            // ONE BUFFER, THE SAME FOR EVERY VARIANT: the geometry table. The two slots
            // this replaces bound a private vertex copy and a private index copy, chosen
            // by the variant, which is why the format was part of the bucket key.
            DescriptorSetUav::BufferSlot bufferSlot( DescriptorSetUav::BufferSlot::makeEmpty() );
            bufferSlot.buffer = mGeometryBuffer;
            bufferSlot.access = ResourceAccess::Read;
            mComputeJobs[i]->_setUavBuffer( 0, bufferSlot );

            DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
            uavSlot.access = ResourceAccess::ReadWrite;

            uavSlot.texture = mAlbedoVox;
            if( hasTypedUavs )
                uavSlot.pixelFormat = mAlbedoVox->getPixelFormat();
            else
                uavSlot.pixelFormat = PFG_R32_UINT;
            uavSlot.access = ResourceAccess::ReadWrite;
            mComputeJobs[i]->_setUavTexture( 1, uavSlot );

            uavSlot.texture = mNormalVox;
            if( hasTypedUavs )
                uavSlot.pixelFormat = mNormalVox->getPixelFormat();
            else
                uavSlot.pixelFormat = PFG_R32_UINT;
            uavSlot.access = ResourceAccess::ReadWrite;
            mComputeJobs[i]->_setUavTexture( 2, uavSlot );

            uavSlot.texture = mEmissiveVox;
            if( hasTypedUavs )
                uavSlot.pixelFormat = mEmissiveVox->getPixelFormat();
            else
                uavSlot.pixelFormat = PFG_R32_UINT;
            uavSlot.access = ResourceAccess::ReadWrite;
            mComputeJobs[i]->_setUavTexture( 3, uavSlot );

            // Jahshaka patch 0065: THIS SLOT WAS THE TRIANGLE COUNTER and is now the
            // per-voxel INTEGER ACCUMULATOR the merge sums into (the count rides in
            // it). Same slot, same uimage3D, so this job's binding shape — and the
            // Vulkan root layout Ogre derives from it — is exactly the pin's. That
            // is not cosmetic: a separate VCT/VoxelResolve compute job was tried
            // first and corrupted the descriptor set of the job dispatched after it
            // (VUID-VkWriteDescriptorSet-descriptorType-00319 on LightInjection's
            // `lightVoxel`, then VK_ERROR_DEVICE_LOST on the Showroom samples).
            uavSlot.texture = mAccumValVox;
            uavSlot.pixelFormat = mAccumValVox->getPixelFormat();
            uavSlot.access = ResourceAccess::ReadWrite;
            mComputeJobs[i]->_setUavTexture( 4, uavSlot );

            // Jahshaka patch 0065: the per-voxel INTEGER ACCUMULATOR the merge
            // sums into (R32_UINT, thirteen texels per voxel — see
            // VoxelMerge_piece_cs.any for why it is not RGBA32_UINT).
            uavSlot.texture = mMergeAccumTex;
            uavSlot.pixelFormat = mMergeAccumTex->getPixelFormat();
            uavSlot.access = ResourceAccess::ReadWrite;
            mComputeJobs[i]->_setUavTexture( 5, uavSlot );

            DescriptorSetTexture2::BufferSlot texBufSlot(
                DescriptorSetTexture2::BufferSlot::makeEmpty() );
            texBufSlot.buffer = mInstanceBufferAsTex;
            mComputeJobs[i]->setTexBuffer( 0, texBufSlot );
        }

        HlmsCompute *hlmsCompute = mHlmsManager->getComputeHlms();
        clearVoxels();

        const uint32 *threadsPerGroup = mComputeJobs[0]->getThreadsPerGroup();

        ShaderParams::Param paramInstanceRange;
        ShaderParams::Param paramVoxelOrigin;
        ShaderParams::Param paramVoxelCellSize;
        ShaderParams::Param paramVoxelPixelOrigin;

        paramInstanceRange.name = "instanceStart_instanceEnd";
        paramVoxelOrigin.name = "voxelOrigin";
        paramVoxelCellSize.name = "voxelCellSize";
        paramVoxelPixelOrigin.name = "voxelPixelOrigin";

        paramVoxelCellSize.setManualValue( getVoxelCellSize() );

        uint32 instanceStart = 0;

        OgreProfileGpuBegin( "VCT Voxelization Jobs" );

        const FastArray<Octant>::const_iterator begin = mOctants.begin();
        FastArray<Octant>::const_iterator itor = begin;
        FastArray<Octant>::const_iterator end = mOctants.end();

        while( itor != end )
        {
            const Octant &octant = *itor;
            VoxelizerBucketMap::const_iterator itBucket = mBuckets.begin();
            VoxelizerBucketMap::const_iterator enBucket = mBuckets.end();

            while( itBucket != enBucket )
            {
                const uint32 octantIdx = static_cast<uint32>( itor - begin );
                const BucketData::InstancesPerOctantIdxMap::const_iterator itNumInstances =
                    itBucket->second.numInstancesAfterCulling.find( octantIdx );

                if( itNumInstances != itBucket->second.numInstancesAfterCulling.end() )
                {
                    const VoxelizerBucket &bucket = itBucket->first;
                    bucket.job->setNumThreadGroups( std::max( 1u, octant.width / threadsPerGroup[0] ),
                                                    std::max( 1u, octant.height / threadsPerGroup[1] ),
                                                    std::max( 1u, octant.depth / threadsPerGroup[2] ) );

                    bucket.job->setConstBuffer( 0, bucket.materialBuffer );

                    uint8 texUnit = 1u;

                    DescriptorSetTexture2::TextureSlot texSlot(
                        DescriptorSetTexture2::TextureSlot::makeEmpty() );
                    if( bucket.needsTexPool )
                    {
                        texSlot.texture = mVctMaterial->getTexturePool();
                        HlmsSamplerblock samplerblock;
                        samplerblock.setAddressingMode( TAM_WRAP );
                        bucket.job->setTexture( texUnit, texSlot, &samplerblock );
                        ++texUnit;
                    }

                    const uint32 numInstancesInBucket = itNumInstances->second;
                    const uint32 instanceRange[2] = { instanceStart,
                                                      instanceStart + numInstancesInBucket };
                    const uint32 voxelPixelOrigin[3] = { octant.x, octant.y, octant.z };

                    paramInstanceRange.setManualValue( instanceRange, 2u );
                    paramVoxelOrigin.setManualValue( octant.region.getMinimum() );
                    paramVoxelPixelOrigin.setManualValue( voxelPixelOrigin, 3u );

                    ShaderParams &shaderParams = bucket.job->getShaderParams( "default" );
                    shaderParams.mParams.clear();
                    shaderParams.mParams.push_back( paramInstanceRange );
                    shaderParams.mParams.push_back( paramVoxelOrigin );
                    shaderParams.mParams.push_back( paramVoxelCellSize );
                    shaderParams.mParams.push_back( paramVoxelPixelOrigin );
                    shaderParams.setDirty();

                    bucket.job->analyzeBarriers( mResourceTransitions );
                    mRenderSystem->executeResourceTransition( mResourceTransitions );
                    hlmsCompute->dispatch( bucket.job, 0, 0 );

                    instanceStart += numInstancesInBucket;
                }

                ++itBucket;
            }

            ++itor;
        }

        OgreProfileGpuEnd( "VCT Voxelization Jobs" );

        // These textures are no longer needed, they're not used for the injection
        // phase. Save memory.
        mAccumValVox->scheduleTransitionTo( GpuResidency::OnStorage );

        // THE MERGE ACCUMULATOR STAYS RESIDENT (Jahshaka patch 0071). Patch 0065
        // gave it upstream's transient treatment: OnStorage at the end of every
        // build(), Resident at the start of the next one. On NVIDIA 595.84 that
        // per-build create/destroy of a large 3D storage image, while the
        // dispatches that wrote the previous one may still be executing, HANGS
        // THE CHANNEL: NVRM Xid 109 CTX SWITCH TIMEOUT -> VK_ERROR_DEVICE_LOST.
        // Measured (lane XID-2, the owner's own sequence scripted -- hide and
        // show a plane in an Epic scene, 100 cycles a run): 4/4 and 6/6 runs
        // lost the device with the round trip, 0/6 and 0/6 without it, every
        // failure carrying a kernel Xid line from that pid and no passing run
        // ever carrying one. It is NOT the delayed-block reuse window (patch
        // 0067's subject: a 16-frame window still hangs 4/6), NOT the 512 MB
        // force-flush (disabled: 6/6), NOT the cached image views (purged on
        // residency loss: 6/6) and NOT the ray-query tier (rays off: 6/6).
        // Keeping the image alive is the only arm that cures it, and it is also
        // the right design: under a cascade chain a build happens every frame or
        // two, so the residency round trip never actually saves anything -- it
        // only returns memory the next build immediately asks for again.
        // THE COST is one accumulator per voxeliser held for its lifetime:
        // width * height * depth * 13 * 4 bytes (13.6 MB at 64^3, 109 MB at
        // 128^3). A future lane may share ONE scratch volume across a chain's
        // cascades; that is an optimisation, not a correctness matter.

        if( mNeedsAlbedoMipmaps || mNeedsAllMipmaps )
            mAlbedoVox->_autogenerateMipmaps();
        if( mNeedsAllMipmaps )
        {
            mEmissiveVox->_autogenerateMipmaps();
            mNormalVox->_autogenerateMipmaps();
        }

        OgreProfileGpuEnd( "VCT build" );
    }
}  // namespace Ogre
