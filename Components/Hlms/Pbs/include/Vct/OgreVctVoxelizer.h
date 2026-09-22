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

// The download helper used to be included here and pulled these in with it.
#include "Vao/OgreVertexBufferPacked.h"

#include "ogrestd/map.h"

#ifdef OGRE_FORCE_VCT_VOXELIZER_DETERMINISTIC
#    include "OgreMesh2.h"
#endif

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class VctMaterial;

    namespace VoxelizerJobSetting
    {
        /// Jahshaka (ATOM P4): `Index32bit` and `CompressedVertexFormat` are GONE.
        /// They existed because the voxelizer owned PRIVATE COPIES of every mesh's
        /// geometry, in one of two formats, in one of two index widths - so the format
        /// was a property of the buffer a dispatch BOUND and therefore a shader
        /// permutation and a bucket key. The shader now reads each instance's geometry
        /// where the raster reads it, through the buffer device addresses and the
        /// layout in its geometry row, so the width and the packing are per-MESH DATA
        /// read at runtime and no longer split a dispatch. Sixteen job variants became
        /// four, and two reasons for one octant to need several dispatches disappeared.
        enum VoxelizerJobSetting
        {
            HasDiffuseTex = 1u << 0u,
            HasEmissiveTex = 1u << 1u,
        };
    }

    struct VoxelizerBucket
    {
        HlmsComputeJob    *job;
        ConstBufferPacked *materialBuffer;
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
            return this->materialBuffer < other.materialBuffer;
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
    public:
        /** JAHSHAKA (ATOM P4) - ONE (mesh, LOD level, submesh)'s GEOMETRY, AS THE
            SHADER READS IT. std430, 48 bytes, three uvec4 lanes so C++ and GLSL agree
            with no padding rule to remember.

            THE WHOLE POINT OF THIS STRUCT is that it holds ADDRESSES, not data. The
            voxelizer used to download every mesh's vertices to the CPU, unpack them to
            8 floats each and upload the result into a private buffer, once per build();
            indices were copied GPU-side into two more private buffers. All of it is
            gone: a row names where the RASTER's vertices and indices already live
            (VaoManager::getBufferDeviceAddress) and how they are laid out, and the
            compute shader dereferences that through GL_EXT_buffer_reference.

            WHY THE ROW IS PER (mesh, LEVEL, submesh) AND NOT PER MESH. A LOD level is
            its own index buffer over the SAME vertex buffer, so the vertex address is
            the mesh's and the index address is the level's; and one mesh instanced at
            two distances is now voxelized at two levels in the same build (the finest
            request no longer wins for every instance of a mesh - AT-A10). A submesh is
            its own vertex buffer entirely.
        */
        struct GeometryRow
        {
            /// Device address of vertex 0 of this submesh's vertex buffer, and of index
            /// 0 of this (submesh, level)'s index range. Split into two uint32 because
            /// shaderInt64 is not enabled on this device; GLSL rebuilds the reference
            /// from a uvec2 (GL_EXT_buffer_reference_uvec2).
            uint32 posAddress[2];
            uint32 idxAddress[2];
            uint32 vertexStride;   ///< bytes between vertices
            uint32 posOffset;      ///< bytes from the vertex start to VES_POSITION
            uint32 normalOffset;   ///< ditto VES_NORMAL; 0xFFFFFFFF when absent
            uint32 uvOffset;       ///< ditto VES_TEXTURE_COORDINATES; 0xFFFFFFFF absent
            /// bit 0: indices are 32-bit. bit 1: positions are float3 (always, today -
            /// reserved so a packed position format becomes data, not a permutation).
            uint32 flags;
            /// Index ELEMENTS skipped by rounding `idxAddress` down to a 4-byte
            /// boundary. A 16-bit index buffer can start at an odd uint16, and a
            /// buffer_reference of uints must be 4-byte aligned, so the address is
            /// floored and the remainder rides here - the same bookkeeping upstream's
            /// adjustIndexOffsets16 did for its private copy, now costing no copy.
            uint32 idxBias;
            uint32 padding[2];
        };

        /** JAHSHAKA (ATOM P4b): DESCRIBE ONE (mesh, level, submesh) WITHOUT A VOXELIZER.

            The row is a fact about a MESH, not about a voxelisation: the same two
            addresses and the same layout serve every cascade that voxelises it and
            (phase D) the raster that draws it. So the table belongs to whoever owns
            meshes - the host's GPU scene, which writes one row per (mesh, level,
            submesh) ONCE when the mesh is first attached - and this is the describer
            it calls. The voxelizer used to rebuild the whole table on EVERY build()
            of EVERY cascade, which is a CPU walk over the scene's geometry per
            rebuild for data that never changes.

            THE FORMAT STAYS OGRE'S because the shader that reads it is Ogre's.
        @param level
            Clamped to the levels the submesh has.
        @return
            False - with one log line naming the mesh and the reason - when this
            (level, submesh) cannot be read in place: no index buffer, no float3
            VES_POSITION, a stride or element offset that is not a multiple of 4, or
            a device with no buffer device addresses. `out` is then untouched.
        */
        static bool describeGeometryRow( const MeshPtr &mesh, uint32 level, uint32 submesh,
                                         VaoManager *vaoManager, GeometryRow &out );

        /** THE GEOMETRY TABLE THIS VOXELIZER READS - the host's, bound for every
            dispatch (the voxelise jobs and the AABB calculator).

            A voxelizer with no source keeps NOTHING of its own: the per-build table it
            used to own is deleted, not kept as a fallback. Setting it to null makes
            every build an empty one (and says so once).
        */
        void setGeometrySource( UavBufferPacked *rows ) { mGeometryBuffer = rows; }

    protected:
        struct PartitionedSubMesh
        {
            /// Element offset of this partition's first index INSIDE its level's index
            /// buffer (the level's own primitiveStart is already folded in).
            uint32 firstIndex;
            uint32 numIndices;
            uint32 aabbSubMeshIdx;
        };

        struct QueuedSubMesh
        {
            /// Row in the HOST's geometry table (setGeometrySource). The voxelizer no
            /// longer describes geometry: `addItem` is told the row of this (mesh,
            /// level, submesh 0) and the rows of one level are contiguous, so submesh s
            /// is geomRowBase + s. 0xFFFFFFFF = the host had no row for it.
            uint32                        geomRow;
            FastArray<PartitionedSubMesh> partSubMeshes;
        };

        typedef FastArray<QueuedSubMesh> QueuedSubMeshArray;

        /// ONE LOD LEVEL OF A QUEUED MESH. A level nobody asked for holds nothing and
        /// costs nothing: the table only describes levels some instance is voxelized at.
        struct QueuedMeshLevel
        {
            bool               wanted;
            /// The host's row for (this mesh, this level, submesh 0); 0xFFFFFFFF = none.
            uint32             geomRowBase;
            QueuedSubMeshArray submeshes;
            QueuedMeshLevel() : wanted( false ), geomRowBase( 0xFFFFFFFFu ) {}
        };

        struct QueuedMesh
        {
            uint32                      numItems;
            uint32                      indexCountSplit;
            /// Indexed by LOD level. `lodLevel` and "the finest request wins" are GONE
            /// (patch 0064's rule): nothing is downloaded per mesh any more, so two
            /// instances of one mesh can be voxelized at two levels and each pays for
            /// its own.
            FastArray<QueuedMeshLevel>  levels;
        };

        /// ONE QUEUED ITEM AND THE LEVEL IT ASKED FOR. The level is the ITEM's, not the
        /// mesh's - see QueuedMesh::levels.
        struct QueuedItem
        {
            Item  *item;
            uint32 lodLevel;
        };

        typedef map<v1::MeshPtr, bool>::type v1MeshPtrMap;
#ifdef OGRE_FORCE_VCT_VOXELIZER_DETERMINISTIC
        typedef map<MeshPtr, QueuedMesh, DeterministicMeshPtrOrder>::type MeshPtrMap;
#else
        typedef map<MeshPtr, QueuedMesh>::type MeshPtrMap;
#endif
        typedef FastArray<QueuedItem> ItemArray;

        v1MeshPtrMap mMeshesV1;
        MeshPtrMap   mMeshesV2;

        ItemArray mItems;

        /// HlmsComputeJob have internal caches, thus we could dynamically change properties
        /// and let the internal cache handle whether a compute job needs to be compiled.
        ///
        /// However the way we will be using may abuse the cache too much, thus we pre-set
        /// all variants as long as the number of variants is manageable.
        HlmsComputeJob *mComputeJobs[1u << 2u];
        /// ONE AABB CALCULATOR, not four: its variants were the index width and the
        /// vertex packing, both of which are now data in a geometry row.
        HlmsComputeJob *mAabbCalculator;
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
        /// THE HOST'S GEOMETRY TABLE (setGeometrySource) - NOT OWNED. One GeometryRow
        /// per (mesh, level, submesh), written once at attach by whoever owns meshes.
        /// This class used to own and rebuild it per build(); before that it owned four
        /// private copies of the world's geometry.
        UavBufferPacked *mGeometryBuffer;
        // Aabb Calculator
        uint32           mNumPartSubMeshes;
        TexBufferPacked *mGpuPartitionedSubMeshes;
        UavBufferPacked *mMeshAabb;

        bool mNeedsAlbedoMipmaps;
        bool mNeedsAllMipmaps;

        uint32 mDefaultIndexCountSplit;

        ComputeTools *mComputeTools;

        struct QueuedInstance
        {
            MovableObject *movableObject;
            /// Row in the geometry table - which (mesh, LEVEL, submesh) this instance
            /// draws. `vertexBufferStart` is gone: the row's address already points at
            /// this submesh's own vertex buffer.
            uint32         geomRow;
            uint32         firstIndex;
            uint32         numIndices;
            uint32         aabbSubMeshIdx;
            uint32         materialIdx;
            /// The LOD level this instance is voxelized at - a reading, for the
            /// (mesh, level) histogram AT-A10 asks for.
            uint32         lodLevel;
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

        void createComputeJobs();
        void clearComputeJobResources( bool calculatorDataOnly );

        /// Enumerates one (mesh, level)'s PARTITIONS and records the host's row index
        /// for each submesh. It no longer describes geometry - that is
        /// describeGeometryRow, called once per mesh by the host at attach.
        void partitionMeshLevel( const MeshPtr &mesh, QueuedMesh &queuedMesh, uint32 level,
                                 uint32 geomRowBase );
        void prepareAabbCalculatorMeshData();
        void destroyAabbCalculatorMeshData();

        void freeBuffers( bool bForceFree );

        /// Enumerates the queue's partitions and the AABB calculator's inputs. It no
        /// longer builds a geometry table: the host's is bound by setGeometrySource.
        void buildPartitionTable();
        void createVoxelTextures();
        /// Jahshaka patch 0065: drops mMergeAccumTex too.
        void destroyVoxelTextures() override;

        void   placeItemsInBuckets();
        size_t countSubMeshPartitionsIn( const QueuedItem &queuedItem ) const;
        void   createInstanceBuffers();
        void   destroyInstanceBuffers();
        void   fillInstanceBuffers();

        void computeMeshAabbs();

        void clearVoxels();

    public:
        /** @param materialStore
                THE MATERIAL STORE THIS VOXELIZER USES, AND DOES NOT OWN. Required.

                It used to `new` its own, which made (bucketIdx, slotIdx) a PER-STORE
                fact: `findFreeBucketFor` fills buckets in insertion order, and a chain's
                cascades see different attach sets in different orders, so the SAME
                datablock got a different pool and slot in every cascade. Nothing
                scene-wide could then name a material - which is exactly what a GPU
                instance feed needs, one word per instance, for every cascade at once.

                Sharing one store across a chain is also the better shape on its own
                terms: one conversion per datablock instead of one per cascade, one set
                of pool const buffers, one texture pool, one by-pointer alias cache and
                one eviction call. AND IT CLOSES A HAZARD: the compute jobs the
                dispatches use are CLONED PER VARIANT AND SHARED between voxelizers, so a
                job could hold one store's texture pool in its descriptor while another
                voxelizer dispatched it. With one store there is one pool.

                The owner creates it before any voxelizer and destroys it after all of
                them, and brackets a whole chain's builds with ONE
                initTempResources/destroyTempResources pair.
        */
        VctVoxelizer( IdType id, RenderSystem *renderSystem, HlmsManager *hlmsManager,
                      bool correctAreaLightShadows, VctMaterial *materialStore );
        ~VctVoxelizer();

        void _setNeedsAllMipmaps( bool bNeedsAllMipmaps ) { mNeedsAllMipmaps = bNeedsAllMipmaps; }

        /**
        @param item
            The item to voxelize.
        @param indexCountSplit
            0 to use mDefaultIndexCountSplit. Use a different value to override.
            This value is ignored if the mesh had already been added.

            Use std::numeric_limits<uint32>::max to avoid partitioning at all.
        @param lodLevel
            Which LOD level of the item's mesh to voxelize. 0 (the default) is the
            finest level. Higher levels are clamped to the levels the mesh actually
            has, so a mesh with no LOD chain is voxelized exactly as before.

            THE LEVEL IS THE ITEM'S (Jahshaka, ATOM P4 / AT-A10). It used to be the
            MESH's inside this voxelizer, and the FINEST request won, because the
            mesh's geometry was downloaded and converted once per mesh: one mesh
            instanced at 1x near the eye and 10x far away paid the near level for all
            eleven. Nothing is downloaded now - the shader reads each instance's own
            level through its geometry row - so each instance gets the level it asked
            for and a volume's cost follows its own error rule.

            A voxel grid cannot represent detail finer than its own cell, so a coarse
            volume voxelizing a simplified level produces the same voxels for a
            fraction of the raster cost. The caller decides which level that is; this
            class only spends it.

        @remarks
            The `bCompressed` argument is GONE. It chose between two PRIVATE vertex
            formats this class no longer has: the shader reads the pool's own vertices
            through the layout in the geometry row, so "compressed" is whatever the
            mesh was baked as and is not a decision a caller can make here.
        */
        /** @param geomRowBase
                The host's geometry-table row for (this mesh, `lodLevel`, submesh 0);
                submesh s is geomRowBase + s. 0xFFFFFFFF means the host has no rows for
                this mesh at this level, and the item is refused with a log line - the
                same honest "GI is empty" path a device with no buffer device addresses
                takes.
        */
        void addItem( Item *item, uint32 indexCountSplit = 0u, uint32 lodLevel = 0u,
                      uint32 geomRowBase = 0xFFFFFFFFu );

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

        /// JAHSHAKA - HOW MANY INDICES THE LAST build() ACTUALLY BOUND.
        ///
        /// `QueuedInstance::numIndices` is what sizes each raster dispatch, and it is
        /// filled in `placeItemsInBuckets` from the LEVEL THAT INSTANCE ASKED FOR,
        /// clamped here to the levels the mesh has. Summing it is therefore a READING
        /// of the geometry the volume holds, where a host walking the mesh's VAOs from
        /// outside can only ever produce a prediction that re-implements the clamp and
        /// drifts from it. Since the level is per instance, this number now also
        /// reflects that a mesh instanced at two distances pays two different costs.
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
        /// JAHSHAKA (ATOM P4 / AT-A10) - THE (mesh, LEVEL) HISTOGRAM, MEASURED.
        ///
        /// `out[level]` = how many queued instances the last build() voxelized at that
        /// LOD level. On a mesh instanced at 1x and 10x this reads two non-zero entries;
        /// under the old rule (the finest request winning for the whole mesh) it could
        /// only ever read one, which is why a host-side histogram of what it ASKED for
        /// was not evidence of what happened.
        void getLevelHistogram( FastArray<uint32> &out ) const
        {
            out.clear();
            VoxelizerBucketMap::const_iterator itor = mBuckets.begin();
            VoxelizerBucketMap::const_iterator endt = mBuckets.end();
            while( itor != endt )
            {
                FastArray<QueuedInstance>::const_iterator itInst = itor->second.queuedInst.begin();
                FastArray<QueuedInstance>::const_iterator enInst = itor->second.queuedInst.end();
                while( itInst != enInst )
                {
                    if( out.size() <= itInst->lodLevel )
                        out.resize( itInst->lodLevel + 1u, 0u );
                    ++out[itInst->lodLevel];
                    ++itInst;
                }
                ++itor;
            }
        }

    public:
        /// JAHSHAKA PATCH 0081: the material store, so a host can evict a dying
        /// datablock (VctMaterial::removeDatablock) instead of re-voxelising every
        /// volume. NOT OWNED - see the constructor. The host that owns the store
        /// should evict on the STORE, once, rather than through each voxelizer.
        VctMaterial *getVctMaterial() const { return mVctMaterial; }

    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
