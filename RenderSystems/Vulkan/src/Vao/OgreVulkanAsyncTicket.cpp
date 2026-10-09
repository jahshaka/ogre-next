/*
-----------------------------------------------------------------------------
This source file is part of OGRE-Next
(Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org

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

#include "Vao/OgreVulkanAsyncTicket.h"

#include "OgreException.h"
#include "OgreVulkanQueue.h"
#include "OgreVulkanUtils.h"
#include "Vao/OgreStagingBuffer.h"
#include "Vao/OgreVulkanVaoManager.h"

namespace Ogre
{
    VulkanAsyncTicket::VulkanAsyncTicket( BufferPacked *creator, StagingBuffer *stagingBuffer,
                                          size_t elementStart, size_t elementCount,
                                          VulkanQueue *queue, VulkanVaoManager *vaoManager,
                                          bool accurateTracking ) :
        AsyncTicket( creator, stagingBuffer, elementStart, elementCount ),
        mFenceName( 0 ),
        mQueue( queue ),
        mVaoManager( vaoManager ),
        mDownloadFrame( vaoManager->getFrameCount() ),
        mAccurateTracking( accurateTracking ),
        mFrameDone( false )
    {
        // The copy is already recorded (AsyncTicket's ctor -> _asyncDownload).
        if( accurateTracking )
        {
            mFenceName = queue->acquireCurrentFence();
            // Flush now for accuracy with downloads. NOTE (Jahshaka): this flush parks the fence
            // on the frame slot being recorded and newCommandBuffer() then waits on that slot,
            // i.e. the CPU waits for everything submitted so far: a full queue drain.
            mQueue->commitAndNextCommandBuffer();
        }
        // else: nothing flushed, nothing fenced: the frame's own submission carries the copy.
    }
    //-----------------------------------------------------------------------------------
    VulkanAsyncTicket::~VulkanAsyncTicket()
    {
        if( mFenceName )
            mQueue->releaseFence( mFenceName );
    }
    //-----------------------------------------------------------------------------------
    const void *VulkanAsyncTicket::mapImpl()
    {
        if( mFenceName )
            mFenceName = VulkanVaoManager::waitFor( mFenceName, mQueue );
        else if( !mAccurateTracking && !mFrameDone )
        {
            // Mapping before the recording frame finished waits for exactly that frame (a full
            // stall only if it is still the frame being recorded).
            mVaoManager->waitForSpecificFrameToFinish( mDownloadFrame );
            mFrameDone = true;
        }

        return mStagingBuffer->_mapForRead( mStagingBufferMapOffset,
                                            mElementCount * mCreator->getBytesPerElement() );
    }
    //-----------------------------------------------------------------------------------
    bool VulkanAsyncTicket::queryIsTransferDone()
    {
        bool retVal = false;

        if( mFenceName )
        {
            // Ask to return immediately and tell us about the fence
            VkResult result = vkWaitForFences( mQueue->mDevice, 1u, &mFenceName, VK_TRUE, 0 );
            if( result != VK_TIMEOUT )
            {
                mQueue->releaseFence( mFenceName );
                mFenceName = 0;

                checkVkResult( mQueue->mOwnerDevice, result, "vkWaitForFences" );
            }
        }
        else if( !mAccurateTracking && !mFrameDone )
        {
            // Never waits: true once the frame the copy was recorded in has finished.
            mFrameDone = mVaoManager->isFrameFinished( mDownloadFrame );
            retVal = mFrameDone;
        }
        else
        {
            retVal = true;
        }

        return retVal;
    }
}  // namespace Ogre
