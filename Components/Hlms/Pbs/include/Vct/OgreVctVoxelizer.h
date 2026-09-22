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
#ifndef _OgreVctVoxelizer_H_
#define _OgreVctVoxelizer_H_

#include "OgreVctVoxelizerSourceBase.h"

#include "Vao/OgreVertexBufferDownloadHelper.h"

#ifdef OGRE_FORCE_VCT_VOXELIZER_DETERMINISTIC
#    include "OgreMesh2.h"
#endif

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class VctMaterial;

    namespace VoxelizerJobSetting
    {
        enum VoxelizerJobSetting
        {
            Index32bit = 1u << 0u,
            CompressedVertexFormat = 1u << 1u,
            HasDiffuseTex = 1u << 2u,
            HasEmissiveTex = 1u << 3u,
        };
    }

    struct VoxelizerBucket
    {
        HlmsComputeJob    *job;
        ConstBufferPacked *materialBuffer;
        UavBufferPacked   *vertexBuffer;
        UavBufferPacked   *indexBuffer;
        bool               needsTexPool;
        /// WHAT THIS BUCKET IS, said without a pointer: the compute job's VARIANT
        /// (the VoxelizerJobSetting bits — it also decides the vertex and index
        /// buffers) and the material POOL its const buffer is. See operator< below.
        uint32             variant;
        uint32             materialBucketIdx;

        /// ORDERED BY WHAT A BUCKET IS, NEVER BY WHERE IT LIVES.
        ///
        /// This ordering is the ORDER THE DISPATCHES RUN IN (VctVoxelizer::build
        /// iterates mBuckets). Comparing POINTERS made that order the allocator's,
        /// and the voxelisation's per-voxel merge used to be order-dependent, so
        /// the same scene voxelised in two processes was not the same picture and
        /// no re-solve could correct it (the VOXELS differed). Patch 0065 made the
        /// merge itself order-independent — the dispatches accumulate exact integer
        /// sums and one resolve pass turns them into the voxel textures — so the
        /// order no longer decides anything; this key stays because a stable order
        /// is still the right thing for a renderer to have, and it costs nothing.
        ///
        /// A BUCKET IS A DISPATCH, AND A DISPATCH IS SIZED BY THE WHOLE OCTANT
        /// however few instances it holds, so the key must name only what a
        /// dispatch BINDS: the job variant (which also decides the vertex and index
        /// buffers and the texture pool) and the material POOL's const buffer. The
        /// material SLOT rides per instance and must NOT be here — patch 0062 put
        /// it here to buy determinism from the pin's order-dependent merge, and it
        /// split one whole-volume dispatch into one PER MATERIAL: on a scene whose
        /// 8,001 primitives each own a material the outermost cascade's rebuild
        /// went 86 -> 321 ms. The pointer comparisons remain underneath as a
        /// total-order tie-break that nothing reaches.
        bool operator<( const VoxelizerBucket &other ) const
        {
            if( this->variant != other.variant )
                return this->variant < other.variant;
            if( this->materialBucketIdx != other.materialBucketIdx )
                return this->materialBucketIdx < other.materialBucketIdx;
            if( this->needsTexPool != other.needsTexPool )
                return this->needsTexPool < other.needsTexPool;
            if( this->job != other.job )
                return this->job < other.job;
            if( this->materialBuffer != other.materialBuffer )
                return this->materialBuffer < other.materialBuffer;
            if( this->vertexBuffer != other.vertexBuffer )
                return this->vertexBuffer < other.vertexBuffer;
            return this->indexBuffer < other.indexBuffer;
        }
    };

#ifdef OGRE_FORCE_VCT_VOXELIZER_DETERMINISTIC
    struct DeterministicMeshPtrOrder
    {
        bool operator()( const MeshPtr &a, const MeshPtr &b ) const
        {
            return a->getName() < b->getName();
        }
    };
#endif

    /**
    @class VctVoxelizer
        The voxelizer consists in several stages. The main ones that requre explanation are:

        1. Download the vertex buffers to CPU, then upload it again to GPU in an homogeneous
           format our compute shader understands. We do the same with the index buffer, except
           we perform GPU -> GPU copies. We can't use the buffers directly because in many
           APIs we can't bind the index buffer as an UAV easily.
           This step is handled by VctVoxelizer::buildMeshBuffers.
           Not implemented yet: when rebuilding the voxelized scene, this step can be skipped
           if no meshes were added since the last change (and the buffers weren't freed
           to save memory)

        2. Iterate through every Item and convert the datablocks to the simplified version
           our compute shader uses. VctMaterial handles this; done in
           VctVoxelizer::placeItemsInBuckets.

        3. During step 2, we also group the items into buckets. Each bucket can be batched
           together to dispatch a single compute shader execution; because they share all
           the same settings (and we haven't run out of material buffer space)

        4. The items grouped in buckets may be split into 8 instance buffers; in order to
           cull each octant of the voxel; thus avoiding having all voxels try to check
           for all instances (performance optimization).
    */
    class _OgreHlmsPbsExport VctVoxelizer : public VctVoxelizerSourceBase
    {
    protected:
        struct MappedBuffers
        {
            float *RESTRICT_ALIAS uncompressedVertexBuffer;
            size_t                index16BufferOffset;
            size_t                index32BufferOffset;
        };

        struct PartitionedSubMesh
        {
            uint32 vbOffset;
            uint32 ibOffset;
            uint32 numIndices;
            uint32 aabbSubMeshIdx;
        };

        struct QueuedSubMesh
        {
            // Due to an infrastructure bug, we're commenting out the 'STREAM_DOWNLOAD' path
            // The goal was to queue up several transfer GPU -> staging area, then map
            // the staging area. However this backfired as each AsyncTicket will hold its own
            // StagingBuffer (and each one is at least 4MB) instead of sharing it. This balloons
            // memory consumption and still needs to map staging buffers a lot, defeating part of
            // its purpose.
            // Since fixing it would take a lot of time, it has been ifdef'ed out and instead
            // we download the data and immediately map it. This causes more stalls but is
            // far more memory friendly.
#ifdef STREAM_DOWNLOAD
            VertexBufferDownloadHelper downloadHelper;
#else
            size_t downloadVertexStart;
            size_t downloadNumVertices;
#endif
            FastArray<PartitionedSubMesh> partSubMeshes;
        };

        typedef FastArray<QueuedSubMesh> QueuedSubMeshArray;

        struct QueuedMesh
        {
            bool               bCompressed;
            uint32             numItems;
            uint32             indexCountSplit;
            /// WHICH MESH LOD OF THIS MESH IS VOXELIZED (Jahshaka patch 0064).
            /// 0 = the finest level, which is what every caller that does not ask
            /// gets and is exactly the behaviour this class always had. Clamped
            /// per SubMesh to the levels that exist, so a mesh with no LOD chain
            /// ignores it entirely.
            ///
            /// It is a property of the MESH inside THIS voxelizer and not of the
            /// item, because the buffers are downloaded, converted and indexed
            /// once per mesh: two items of one mesh in one voxelizer share the
            /// level, and the FINEST request wins (the same precedence rule
            /// `bCompressed` uses).
            uint32             lodLevel;
            QueuedSubMeshArray submeshes;
        };

        typedef map<v1::MeshPtr, bool>::type v1MeshPtrMap;
#ifdef OGRE_FORCE_VCT_VOXELIZER_DETERMINISTIC
        typedef map<MeshPtr, QueuedMesh, DeterministicMeshPtrOrder>::type MeshPtrMap;
#else
        typedef map<MeshPtr, QueuedMesh>::type MeshPtrMap;
#endif
        typedef FastArray<Item *> ItemArray;

        v1MeshPtrMap mMeshesV1;
        MeshPtrMap   mMeshesV2;

        ItemArray mItems;

        /// HlmsComputeJob have internal caches, thus we could dynamically change properties
        /// and let the internal cache handle whether a compute job needs to be compiled.
        ///
        /// However the way we will be using may abuse the cache too much, thus we pre-set
        /// all variants as long as the number of variants is manageable.
        HlmsComputeJob *mComputeJobs[1u << 4u];
        HlmsComputeJob *mAabbCalculator[1u << 2u];
        HlmsComputeJob *mAabbWorldSpaceJob;

        /// Jahshaka patch 0065 — THE ORDER-INDEPENDENT MERGE'S ACCUMULATOR.
        ///
        /// PFG_R32_UINT, (mWidth, mHeight, mDepth * 13): thirteen texels per voxel,
        /// interleaved in Z (albedo sum rgba, raw normal sum xyz, emissive sum
        /// rgb, folded normal sum xyz), all in the fixed-point integers
        /// Samples/Media/VCT/VoxelMerge_piece_cs.any defines — which also says
        /// why it is thirteen R32 texels and not four RGBA32 ones: MEASURED, the
        /// four-texel layout costs the same GPU time and 12 MB more per 64^3
        /// volume (VOXMERGE-2; the earlier reason, a device loss from the
        /// 128-bit clear, was patch 0067's hazard seen from here).
        /// 52 bytes per voxel, which is 13.6 MB at 64^3 and 109 MB at 128^3, and
        /// RESIDENT for the voxeliser's life since patch 0071 (the per-build
        /// residency round trip of an image this size was the second Xid 109
        /// site).
        TextureGpu *mMergeAccumTex;

        uint32                mTotalNumInstances;
        float                *mCpuInstanceBuffer;
        UavBufferPacked      *mInstanceBuffer;
        ReadOnlyBufferPacked *mInstanceBufferAsTex;
        UavBufferPacked      *mVertexBufferCompressed;
        UavBufferPacked      *mVertexBufferUncompressed;
        UavBufferPacked      *mIndexBuffer16;
        UavBufferPacked      *mIndexBuffer32;
        // Aabb Calculator
        uint32           mNumUncompressedPartSubMeshes16;
        uint32           mNumUncompressedPartSubMeshes32;
        uint32           mNumCompressedPartSubMeshes16;
        uint32           mNumCompressedPartSubMeshes32;
        TexBufferPacked *mGpuPartitionedSubMeshes;
        UavBufferPacked *mMeshAabb;

        bool mNeedsAlbedoMipmaps;
        bool mNeedsAllMipmaps;

        uint32 mNumVerticesCompressed;
        uint32 mNumVerticesUncompressed;
        uint32 mNumIndices16;
        uint32 mNumIndices32;

        uint32 mDefaultIndexCountSplit;

        ComputeTools *mComputeTools;

        struct QueuedInstance
        {
            MovableObject *movableObject;
            uint32         vertexBufferStart;
            uint32         indexBufferStart;
            uint32         numIndices;
            uint32         aabbSubMeshIdx;
            uint32         materialIdx;
            bool           needsAabbUpdate;
        };
        struct BucketData
        {
            typedef map<uint32, uint32>::type InstancesPerOctantIdxMap;

            FastArray<QueuedInstance> queuedInst;
            InstancesPerOctantIdxMap  numInstancesAfterCulling;
        };
        typedef map<VoxelizerBucket, BucketData>::type VoxelizerBucketMap;
        VoxelizerBucketMap                             mBuckets;

        VctMaterial *mVctMaterial;

        /// Whether mRegionToVoxelize is manually set or autocalculated
        bool mAutoRegion;
        /// Limit to mRegionToVoxelize in case mAutoRegion is true
        Aabb mMaxRegion;

        struct Octant
        {
            uint32 x, y, z;
            uint32 width, height, depth;
            Aabb   region;
        };

        FastArray<Octant> mOctants;
        /// The division `dividideOctants` was last called with, so that moving the
        /// region can re-derive the octants from the SAME division instead of
        /// leaving them describing the box that has just been replaced.
        /// 0 = never divided, which is the only state in which a move has nothing
        /// to re-derive.
        uint32 mNumOctantsX, mNumOctantsY, mNumOctantsZ;

        ResourceTransitionArray mResourceTransitions;

        /** 16-bit buffer values must always be even since the UAV buffer
            is internally packed uint32 and BufferPacked::copyTo doesn't
            like copying with odd-starting offsets in some APIs.
        @returns
            True if indexStart was decremented
        */
        static bool adjustIndexOffsets16( uint32 &indexStart, uint32 &numIndices );

        void createComputeJobs();
        void clearComputeJobResources( bool calculatorDataOnly );

        void countBuffersSize( const MeshPtr &mesh, QueuedMesh &queuedMesh );
        void prepareAabbCalculatorMeshData();
        void destroyAabbCalculatorMeshData();
        void convertMeshUncompressed( const MeshPtr &mesh, QueuedMesh &queuedMesh,
                                      MappedBuffers &mappedBuffers );

        void freeBuffers( bool bForceFree );

        void buildMeshBuffers();
        void createVoxelTextures();
        /// Jahshaka patch 0065: drops mMergeAccumTex too.
        void destroyVoxelTextures() override;

        void   placeItemsInBuckets();
        size_t countSubMeshPartitionsIn( Item *item ) const;
        void   createInstanceBuffers();
        void   destroyInstanceBuffers();
        void   fillInstanceBuffers();

        void computeMeshAabbs();

        void clearVoxels();

    public:
        VctVoxelizer( IdType id, RenderSystem *renderSystem, HlmsManager *hlmsManager,
                      bool correctAreaLightShadows );
        ~VctVoxelizer();

        void _setNeedsAllMipmaps( bool bNeedsAllMipmaps ) { mNeedsAllMipmaps = bNeedsAllMipmaps; }

        /**
        @param item
        @param bCompressed
            True if we should compress:
                position to 16-bit SNORM
                normal to 16-bit SNORM
                uv as 16-bit Half
            False if we should use everything as 32-bit float
            If multiple Items using the same Mesh are added and one of them asks
            to not use compression, then not using compression takes precedence.
        @param indexCountSplit
            0 to use mDefaultIndexCountSplit. Use a different value to override
            This value is ignored if the mesh had already been added.

            Use std::numeric_limits<uint32>::max to avoid partitioning at all.
        @param lodLevel
            Which LOD level of the item's mesh to voxelize. 0 (the default) is the
            finest level and is what this class always did. Higher levels are
            clamped to the levels the mesh actually has, so a mesh with no LOD
            chain is voxelized exactly as before.

            The level belongs to the MESH within this voxelizer: if several Items
            share a mesh and ask for different levels, the FINEST (lowest) wins,
            because the mesh's vertex/index data is downloaded and converted once.

            A voxel grid cannot represent detail finer than its own cell, so a
            coarse volume voxelizing a simplified level produces the same voxels
            for a fraction of the raster cost. The caller decides which level that
            is; this class only spends it.
        */
        void addItem( Item *item, bool bCompressed, uint32 indexCountSplit = 0u,
                      uint32 lodLevel = 0u );

        /** Removes an item added via VctVoxelizer::addItem
        @remarks
            Once the last item that shares the same mesh is removed, the entry about
            that mesh is also removed.
            That means informations such as 'compressed' setting is forgot.

            Will throw if Item is not found.
        @param item
            Item to remove
        */
        void removeItem( Item *item );

        /// Removes all items added via VctVoxelizer::addItem
        void removeAllItems();

        /** Call this function before VctVoxelizer::autoCalculateRegion
        @param autoRegion
            True to autocalculate region to cover all the added items
            False to use 'regionToVoxelize' instead
        @param regionToVoxelize
            When autoRegion = false, use this to manually provide the region
            When autoRegion = true, it is ignored as it will be overwritten by autoCalculateRegion
        @param maxRegion
            Maximum size of the regions are allowed to cover (mostly useful when autoRegion = true)
        */
        void setRegionToVoxelize( bool autoRegion, const Aabb &regionToVoxelize,
                                  const Aabb &maxRegion = Aabb::BOX_INFINITE );

        /// Does nothing if VctVoxelizer::setRegionToVoxelize( false, ... ) was called.
        void autoCalculateRegion();

        void dividideOctants( uint32 numOctantsX, uint32 numOctantsY, uint32 numOctantsZ );

        /** Changes resolution. Note that after calling this, you will need to call
            VctVoxelizer::build again, and VctLighting::build again.
        @param width
        @param height
        @param depth
        */
        void setResolution( uint32 width, uint32 height, uint32 depth );

        void build( SceneManager *sceneManager );

        /// Jahshaka patch 0065 — HOW MANY DISPATCHES THE LAST build() ISSUED.
        ///
        /// A bucket IS a dispatch, and a dispatch is sized by the whole OCTANT
        /// however few instances the bucket holds, so `getNumBuckets() *
        /// getNumOctants()` is the number that says whether a scene is paying
        /// for its material count rather than for its geometry. It is the
        /// reading patch 0062's 3.6x had no name for; exposed so the host can
        /// report it (Jahshaka: GiStatus::CascadeStatus::voxelDispatches).
        size_t getNumBuckets() const { return mBuckets.size(); }
        size_t getNumOctants() const { return mOctants.size(); }

        /// JAHSHAKA PATCH 0089 - HOW MANY INDICES THE LAST build() ACTUALLY BOUND.
        ///
        /// `QueuedInstance::numIndices` is what sizes each raster dispatch, and it
        /// is filled in `placeItemsInBuckets` from `getLodVao( subMesh, lodLevel )`
        /// (patch 0064) - i.e. from the LOD LEVEL this voxelizer resolved and
        /// CLAMPED for itself. Summing it is therefore a READING of the geometry
        /// the volume holds, where a host walking the mesh's VAOs from outside can
        /// only ever produce a prediction that re-implements the clamp and drifts
        /// from it (Jahshaka: GiStatus::CascadeStatus::voxelTriangles used to be
        /// exactly that prediction).
        ///
        /// Counted over the buckets and not over `mItems`, because the buckets are
        /// what the dispatches iterate: an item the region declined contributes
        /// nothing here, which is the honest answer.
        size_t getQueuedIndexCount() const
        {
            size_t total = 0u;
            VoxelizerBucketMap::const_iterator itor = mBuckets.begin();
            VoxelizerBucketMap::const_iterator endt = mBuckets.end();
            while( itor != endt )
            {
                FastArray<QueuedInstance>::const_iterator itInst = itor->second.queuedInst.begin();
                FastArray<QueuedInstance>::const_iterator enInst = itor->second.queuedInst.end();
                while( itInst != enInst )
                {
                    total += itInst->numIndices;
                    ++itInst;
                }
                ++itor;
            }
            return total;
        }
    public:
        /// JAHSHAKA PATCH 0081: the voxeliser's material cache, so a host can
        /// evict a dying datablock (VctMaterial::removeDatablock) instead of
        /// re-voxelising every volume.
        VctMaterial *getVctMaterial() const { return mVctMaterial; }

    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
