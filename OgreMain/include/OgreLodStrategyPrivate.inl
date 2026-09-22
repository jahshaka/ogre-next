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

namespace Ogre
{
    inline void LodStrategy::lodSet( ObjectData &objData, Real lodValues[ARRAY_PACKED_REALS],
                                     Real hysteresis )
    {
        for( size_t j = 0; j < ARRAY_PACKED_REALS; ++j )
        {
            MovableObject *owner = objData.mOwner[j];

            // This may look like a lot of ugly indirections, but mLodMerged is a pointer that allows
            // sharing with many MovableObjects (it should perfectly fit even in small caches).
            {
                FastArray<Real>::const_iterator it =
                    std::lower_bound( owner->mLodMesh->begin(), owner->mLodMesh->end(), lodValues[j] );
                ptrdiff_t newLod = std::max<ptrdiff_t>( it - owner->mLodMesh->begin() - 1, 0 );

                // JAHSHAKA (ogre-patch 0075): THE HYSTERESIS BAND, PER PASS.
                // Without it this comparison is a step in both directions, so
                // an object parked on a threshold — a camera breathing at a
                // switch distance, a dolly crawling past one — changes level
                // every frame. The band is measured on the threshold actually
                // being crossed, which is the only one that matters here, and it
                // can only ever hold the BANDED PASS'S OWN last level: a value
                // past the band moves, and a several-level jump moves all the
                // way.
                //
                // `mHysteresisLod` AND NOT `mCurrentMeshLod` IS THE DIRECTION
                // STATE, which is the whole of this amendment. `mCurrentMeshLod`
                // is written by EVERY pass that updates LOD lists — a planar
                // reflector's mirrored camera, a PiP inset, a probe cube face,
                // a thumbnail — so a band that read it would compare the
                // watched view's value against another camera's level: it would
                // hold a level the view never chose whenever the two cameras sit
                // within a band of one threshold, and its memory of the
                // direction of travel would be erased by every sibling pass, so
                // the band stopped suppressing pops in any scene that has a
                // mirror or an inset (measured: spikes/atom-3/FINDINGS.md §7).
                // A pass with no band (hysteresis == 0) leaves this slot alone
                // and writes only `mCurrentMeshLod`, exactly as upstream does.
                //
                // 0xFF = "no state yet" (a fresh object, or one whose mesh was
                // swapped under it): out of range for any real mesh, so the
                // guard below declines the band and the pass takes the exact
                // level — which is what makes the first frame of a capture, and
                // every one-shot offscreen render, upstream's answer to the bit.
                if( hysteresis > 0 )
                {
                    const ptrdiff_t heldLod = static_cast<ptrdiff_t>( owner->mHysteresisLod );
                    if( newLod != heldLod &&
                        heldLod < static_cast<ptrdiff_t>( owner->mLodMesh->size() ) )
                    {
                        // newLod > heldLod implies heldLod + 1 <= newLod <= size - 1,
                        // so both reads are inside the array.
                        const Real threshold = newLod > heldLod
                                                   ? ( *owner->mLodMesh )[size_t( heldLod + 1 )]
                                                   : ( *owner->mLodMesh )[size_t( heldLod )];
                        const Real band = Math::Abs( threshold ) * hysteresis;
                        if( newLod > heldLod ? ( lodValues[j] < threshold + band )
                                             : ( lodValues[j] > threshold - band ) )
                        {
                            newLod = heldLod;
                        }
                    }
                    owner->mHysteresisLod = static_cast<uint8>( newLod );
                }

                owner->mCurrentMeshLod = static_cast<uint8>( newLod );
            }

            RenderableArray::iterator itor = owner->mRenderables.begin();
            RenderableArray::iterator end = owner->mRenderables.end();

            while( itor != end )
            {
                const FastArray<Real> *lodVec = ( *itor )->mLodMaterial;

                FastArray<Real>::const_iterator it =
                    std::lower_bound( lodVec->begin(), lodVec->end(), lodValues[j] );
                ( *itor )->mCurrentMaterialLod =
                    static_cast<uint8>( std::max<ptrdiff_t>( it - lodVec->begin() - 1, 0 ) );
                ++itor;
            }
        }
    }
}  // namespace Ogre
