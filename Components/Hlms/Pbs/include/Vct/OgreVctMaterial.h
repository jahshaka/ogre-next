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
#ifndef _OgreVctMaterial_H_
#define _OgreVctMaterial_H_

#include "OgreHlmsPbsPrerequisites.h"

#include "OgreId.h"

#include "ogrestd/map.h"
#include "ogrestd/set.h"
#include "ogrestd/vector.h"

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class _OgreHlmsPbsExport VctMaterial : public IdObject
    {
    public:
        struct DatablockConversionResult
        {
            uint32             slotIdx;
            /// Which material POOL the slot is in, as an index into VctMaterial's own
            /// bucket list. Together with slotIdx it names the material without naming
            /// a pointer, which is what lets VctVoxelizer order its dispatches the same
            /// way in every process (see VoxelizerBucket::operator<).
            uint32             bucketIdx;
            ConstBufferPacked *constBuffer;
            uint16             diffuseTexIdx;
            uint16             emissiveTexIdx;
            DatablockConversionResult() :
                slotIdx( (uint32)-1 ),
                bucketIdx( (uint32)-1 ),
                constBuffer( 0 ),
                diffuseTexIdx( std::numeric_limits<uint16>::max() ),
                emissiveTexIdx( std::numeric_limits<uint16>::max() )
            {
            }

            bool hasDiffuseTex() const { return diffuseTexIdx != std::numeric_limits<uint16>::max(); }
            bool hasEmissiveTex() const { return emissiveTexIdx != std::numeric_limits<uint16>::max(); }
        };

    protected:
        typedef set<HlmsDatablock *>::type HlmsDatablockSet;
        struct MaterialBucket
        {
            ConstBufferPacked *buffer;
            bool               hasDiffuse;
            bool               hasEmissive;
            HlmsDatablockSet   datablocks;
        };
        typedef vector<MaterialBucket>::type BucketVec;

        typedef map<HlmsDatablock *, DatablockConversionResult>::type DatablockConversionResultMap;

        DatablockConversionResultMap mDatablockConversionResults;

        BucketVec mBuckets;

        VaoManager *mVaoManager;

        typedef map<TextureGpu *, uint16>::type TextureToPoolEntryMap;

        uint16                mNumUsedPoolSlices;
        TextureGpu           *mTexturePool;
        TextureGpuManager    *mTextureGpuManager;
        CompositorManager2   *mCompositorManager;
        TextureGpu           *mDownsampleTex;
        Pass                 *mDownsampleMatPass2DArray;
        Pass                 *mDownsampleMatPass2D;
        CompositorWorkspace  *mDownsampleWorkspace2DArray;
        CompositorWorkspace  *mDownsampleWorkspace2D;
        TextureToPoolEntryMap mTextureToPoolEntry;

        DatablockConversionResult addDatablockToBucket( HlmsDatablock  *datablock,
                                                        MaterialBucket &bucket );

        uint16 getPoolSliceIdxForTexture( TextureGpu *texture );

        void resizeTexturePool();

        MaterialBucket *findFreeBucketFor( HlmsDatablock *datablock );

    public:
        VctMaterial( IdType id, VaoManager *vaoManager, CompositorManager2 *compositorManager,
                     TextureGpuManager *textureGpuManager );
        ~VctMaterial();

        void initTempResources( SceneManager *sceneManager );
        void destroyTempResources();

        /// Adds a datablock, if not already cached.
        /// If the datablock contains textures, then
        /// initTempResources must already have been called.
        DatablockConversionResult addDatablock( HlmsDatablock *datablock );
        /// JAHSHAKA PATCH 0081: FORGET A DATABLOCK THAT IS ABOUT TO DIE. The
        /// conversion cache is keyed by the raw datablock pointer across builds,
        /// so a datablock destroyed and another created at the same address
        /// would ALIAS the dead one's slot. The host's only answer used to be a
        /// from-scratch re-voxelisation of every volume on every material death;
        /// this erases the cache entry instead. The bucket keeps the dead pointer
        /// in its membership set on purpose: slots are numbered by that set's
        /// size, so removing it would hand the next datablock a slot another
        /// live one already holds. One slot per death leaks until the material
        /// object is recreated (a mode or quality change); stated, not hidden.
        void removeDatablock( const HlmsDatablock *datablock );

        TextureGpu *getTexturePool() const { return mTexturePool; }

        /// Are the temp resources up? A store shared by a whole chain is bracketed ONCE
        /// per rebuild by its owner, so a voxelizer's build can assert it rather than
        /// discover it as an untextured material.
        bool hasTempResources() const { return mDownsampleTex != 0; }

        /// Forgets every conversion and keeps every resource - see the definition. The
        /// owner of a store that outlives a rebuild MUST call this when a material's
        /// parameters may have changed, because addDatablock's cache hit never re-reads
        /// the datablock.
        void clearConversions();
    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
