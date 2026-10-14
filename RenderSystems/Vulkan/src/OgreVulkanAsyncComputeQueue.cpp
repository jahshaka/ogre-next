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


#include "OgreVulkanAsyncComputeQueue.h"

#include "OgreVulkanDevice.h"
#include "OgreVulkanRenderSystem.h"
#include "OgreVulkanUtils.h"

#include "OgreException.h"
#include "OgreLogManager.h"
#include "OgreStringConverter.h"

#include <algorithm>

namespace Ogre
{
    VulkanAsyncComputeQueue::VulkanAsyncComputeQueue( VulkanDevice *device, VulkanQueue *computeQueue,
                                                      bool timelineSemaphores ) :
        mDevice( device ),
        mQueue( timelineSemaphores ? computeQueue : 0 ),
        mGraphicsTimeline( 0 ),
        mComputeTimeline( 0 ),
        mGraphicsValue( 0u ),
        mComputeValue( 0u ),
        mMinAfter( 0u ),
        mAfter( 0u ),
        mEnabled( true ),
        mOpen( false ),
        mOpenAsync( false ),
        mSlot( 0u ),
        mNumAsyncBrackets( 0u )
    {
        if( mQueue )
        {
            OGRE_ASSERT_LOW( mQueue->_jahHasOwnSlots() );
            mGraphicsTimeline = createTimeline();
            mComputeTimeline = createTimeline();
            mSlotValues.resize( mQueue->_jahNumSlots(), 0u );
        }
        LogManager::getSingleton().logMessage(
            mQueue ? "Vulkan: async compute ON: queue family " +
                         StringConverter::toString( mQueue->getFamilyIdx() ) + " index " +
                         StringConverter::toString( mQueue->mQueueIdx ) + ", " +
                         StringConverter::toString( mSlotValues.size() ) +
                         " slots, timeline semaphores G and C"
                   : String( "Vulkan: async compute OFF (" ) +
                         ( !computeQueue ? "no second compute-capable queue"
                                         : "timeline semaphores unavailable" ) +
                         "): every bracket records in order on the graphics queue" );
    }
    //-------------------------------------------------------------------------
    VulkanAsyncComputeQueue::~VulkanAsyncComputeQueue() { _destroy(); }
    //-------------------------------------------------------------------------
    void VulkanAsyncComputeQueue::_destroy()
    {
        // The device has been waited idle by VulkanDevice::destroy (or is lost).
        if( mGraphicsTimeline )
            vkDestroySemaphore( mDevice->mDevice, mGraphicsTimeline, 0 );
        if( mComputeTimeline )
            vkDestroySemaphore( mDevice->mDevice, mComputeTimeline, 0 );
        mGraphicsTimeline = 0;
        mComputeTimeline = 0;
        mQueue = 0;
    }
    //-------------------------------------------------------------------------
    VkSemaphore VulkanAsyncComputeQueue::createTimeline()
    {
        VkSemaphoreTypeCreateInfo typeCi;
        makeVkStruct( typeCi, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO );
        typeCi.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        typeCi.initialValue = 0u;
        VkSemaphoreCreateInfo ci;
        makeVkStruct( ci, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO );
        ci.pNext = &typeCi;
        VkSemaphore sem = 0;
        VkResult result = vkCreateSemaphore( mDevice->mDevice, &ci, 0, &sem );
        checkVkResult( mDevice, result, "vkCreateSemaphore (timeline)" );
        return sem;
    }
    //-------------------------------------------------------------------------
    void VulkanAsyncComputeQueue::hostWaitCompute( uint64 value )
    {
        if( !mComputeTimeline || value == 0u )
            return;
        VkSemaphoreWaitInfo waitInfo;
        makeVkStruct( waitInfo, VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO );
        waitInfo.semaphoreCount = 1u;
        waitInfo.pSemaphores = &mComputeTimeline;
        waitInfo.pValues = &value;
        // Bounded steps, never one infinite wait: a lost device may never answer one
        // (DOCS: VulkanDevice::destroy's note on this driver after an Xid).
        for( ;; )
        {
            const VkResult result = vkWaitSemaphores( mDevice->mDevice, &waitInfo, 1000000000ull );
            if( result != VK_TIMEOUT )
            {
                checkVkResult( mDevice, result, "vkWaitSemaphores (async compute slot)" );
                return;
            }
            if( mDevice->isDeviceLost() )
                return;
        }
    }
    //-------------------------------------------------------------------------
    void VulkanAsyncComputeQueue::setEnabled( bool bEnabled )
    {
        OGRE_ASSERT_LOW( !mOpen && "VulkanAsyncComputeQueue::setEnabled inside a bracket" );
        if( bEnabled == mEnabled )
            return;
        if( mQueue && !bEnabled && mComputeValue )
        {
            // The in-order arm writes what the last brackets wrote: it waits for all of them.
            mDevice->mGraphicsQueue.addWaitSemaphore( mComputeTimeline,
                                                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, mComputeValue );
        }
        mEnabled = bEnabled;
        if( mQueue && bEnabled )
        {
            // ...and the async arm must not start before what the in-order arm recorded.
            mMinAfter = signalPoint( "jah.async.enable" );
        }
    }
    //-------------------------------------------------------------------------
    uint64 VulkanAsyncComputeQueue::signalPoint( const char *name )
    {
        ++mGraphicsValue;
        if( !isAsync() )
            return mGraphicsValue;
        if( name )
        {
            // A zero-length label under the pass-label switch names the point in a capture.
            mDevice->mRenderSystem->debugAnnotationPush( name );
            mDevice->mRenderSystem->debugAnnotationPop();
        }
        mDevice->mGraphicsQueue.addSignalSemaphore( mGraphicsTimeline, mGraphicsValue );
        mDevice->commitAndNextCommandBuffer( SubmissionType::FlushOnly );
        return mGraphicsValue;
    }
    //-------------------------------------------------------------------------
    void VulkanAsyncComputeQueue::begin( uint64 afterGraphicsPoint )
    {
        OGRE_ASSERT_LOW( !mOpen && "VulkanAsyncComputeQueue::begin: a bracket is already open" );
        OGRE_ASSERT_LOW( afterGraphicsPoint <= mGraphicsValue &&
                         "VulkanAsyncComputeQueue::begin: a point that was never signalled" );
        mOpen = true;
        mOpenAsync = isAsync();
        if( !mOpenAsync )
            return;
        mAfter = std::max( afterGraphicsPoint, mMinAfter );
        mSlot = static_cast<size_t>( mNumAsyncBrackets % mSlotValues.size() );
        // The slot's command buffers are reused only after the bracket that last used them
        // has finished on the GPU.
        hostWaitCompute( mSlotValues[mSlot] );
        mQueue->_jahBeginSlot( mSlot );
    }
    //-------------------------------------------------------------------------
    uint64 VulkanAsyncComputeQueue::end()
    {
        OGRE_ASSERT_LOW( mOpen && "VulkanAsyncComputeQueue::end without begin" );
        mOpen = false;
        ++mComputeValue;
        if( !mOpenAsync )
        {
            // THE IN-ORDER ARM KEEPS THE TIMELINE: the graphics queue's next submission (the
            // one that follows the bracket's in-order work) signals C = this value, so a
            // caller's waitAt on it is satisfied by submission order on the one queue and C
            // stays one monotonic timeline across setEnabled switches. No compute queue: no
            // timeline, nothing to signal.
            if( mComputeTimeline )
                mDevice->mGraphicsQueue.addSignalSemaphore( mComputeTimeline, mComputeValue );
            return mComputeValue;
        }
        if( mAfter )
            mQueue->addWaitSemaphore( mGraphicsTimeline, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, mAfter );
        mQueue->addSignalSemaphore( mComputeTimeline, mComputeValue );
        mQueue->endAllEncoders();
        mQueue->commitAndNextCommandBuffer( SubmissionType::FlushOnly );
        mSlotValues[mSlot] = mComputeValue;
        ++mNumAsyncBrackets;
        return mComputeValue;
    }
    //-------------------------------------------------------------------------
    void VulkanAsyncComputeQueue::waitAt( uint64 computeValue, VkPipelineStageFlags dstStage )
    {
        OGRE_ASSERT_LOW( computeValue <= mComputeValue &&
                         "VulkanAsyncComputeQueue::waitAt: a value no bracket will signal" );
        // The in-order arm waits for nothing: its brackets precede on the one queue, and the
        // async brackets before a setEnabled( false ) are waited by that switch.
        if( !isAsync() || !computeValue )
            return;
        // Everything recorded so far goes first, unblocked; the wait holds what follows.
        mDevice->commitAndNextCommandBuffer( SubmissionType::FlushOnly );
        mDevice->mGraphicsQueue.addWaitSemaphore( mComputeTimeline, dstStage, computeValue );
    }
    //-------------------------------------------------------------------------
    void VulkanAsyncComputeQueue::waitAtFrameEnd( uint64 computeValue )
    {
        OGRE_ASSERT_LOW( computeValue <= mComputeValue );
        if( !isAsync() || !computeValue )
            return;
        mDevice->mGraphicsQueue.addFrameEndWaitSemaphore(
            mComputeTimeline, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, computeValue );
    }
    //-------------------------------------------------------------------------
    uint64 VulkanAsyncComputeQueue::getCompletedComputeValue() const
    {
        if( !mComputeTimeline )
            return mComputeValue;
        uint64 value = 0u;
        const VkResult result = vkGetSemaphoreCounterValue( mDevice->mDevice, mComputeTimeline, &value );
        checkVkResult( mDevice, result, "vkGetSemaphoreCounterValue" );
        return value;
    }
    //-------------------------------------------------------------------------
    uint32 VulkanAsyncComputeQueue::getFamilyIdx() const
    {
        return mQueue ? mQueue->getFamilyIdx() : mDevice->mGraphicsQueue.getFamilyIdx();
    }
    //-------------------------------------------------------------------------
    uint32 VulkanAsyncComputeQueue::getQueueIdx() const
    {
        return mQueue ? mQueue->mQueueIdx : mDevice->mGraphicsQueue.mQueueIdx;
    }
    //-------------------------------------------------------------------------
    size_t VulkanAsyncComputeQueue::getNumSlots() const { return mQueue ? mQueue->_jahNumSlots() : 0u; }
    //-------------------------------------------------------------------------
    size_t VulkanAsyncComputeQueue::getNumCommandBuffers() const
    {
        return mQueue ? mQueue->_jahNumAllocatedCmdBuffers() : 0u;
    }
    //-------------------------------------------------------------------------
    size_t VulkanAsyncComputeQueue::getNumFences() const
    {
        return mQueue ? mQueue->_jahNumFences() : 0u;
    }
}  // namespace Ogre
