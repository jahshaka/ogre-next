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
#include "Vao/OgreVulkanVaoManager.h"

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
        mNumAsyncBrackets( 0u ),
        mTimestamps( false ),
        mSlotRecycles( 0u ),
        mRecyclesHeld( 0u ),
        mRecycleViolations( 0u )
    {
        if( mQueue )
        {
            if( !mQueue->_jahHasOwnSlots() )
            {
                OGRE_EXCEPT( Exception::ERR_INVALID_STATE, "the compute queue is not in the own-slot mode",
                             "VulkanAsyncComputeQueue" );
            }
            mGraphicsTimeline = createTimeline();
            mComputeTimeline = createTimeline();
            mSlotValues.resize( mQueue->_jahNumSlots(), 0u );
            const uint32 family = mQueue->getFamilyIdx();
            mTimestamps = family < mDevice->mQueueProps.size() &&
                          mDevice->mQueueProps[family].timestampValidBits != 0u;
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
        // Runtime-active (the install is OGRE_DEBUG_MODE 0, where an assert is nothing).
        if( mOpen )
        {
            OGRE_EXCEPT( Exception::ERR_INVALID_STATE, "setEnabled inside a bracket",
                         "VulkanAsyncComputeQueue::setEnabled" );
        }
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
    void VulkanAsyncComputeQueue::begin( uint64 afterGraphicsPoint, const char *name )
    {
        if( mOpen )
        {
            OGRE_EXCEPT( Exception::ERR_INVALID_STATE, "a bracket is already open",
                         "VulkanAsyncComputeQueue::begin" );
        }
        if( afterGraphicsPoint > mGraphicsValue )
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS, "a graphics point that was never signalled",
                         "VulkanAsyncComputeQueue::begin" );
        }
        mOpenAsync = isAsync();
        VulkanRenderSystem *rs = mDevice->mRenderSystem;
        if( mOpenAsync )
        {
            mAfter = std::max( afterGraphicsPoint, mMinAfter );
            mSlot = static_cast<size_t>( mNumAsyncBrackets % mSlotValues.size() );
            // The slot's command buffers are reused only after the bracket that last used them
            // has finished on the GPU.
            hostWaitCompute( mSlotValues[mSlot] );
            // Whatever the graphics queue has open (a render pass, a compute encoder with its
            // bindings) ends here; the compute bindings are per command buffer.
            mDevice->mGraphicsQueue.endAllEncoders();
            rs->_notifyActiveComputeEnded();
            mQueue->_jahBeginSlot( mSlot );
            mDevice->mJahRecordQueue = mQueue;
            mDevice->mGraphicsQueue._jahSetBlocked( true );
            FrameBracket fb;
            fb.recordFrame = mDevice->mVaoManager->getFrameCount();
            fb.value = mComputeValue + 1u;
            mFrameBrackets.push_back( fb );
        }
        mOpen = true;
        mDevice->mJahInBracket = true;
        mOpenName = name ? name : "";
        if( name )
            rs->debugAnnotationPush( name );
    }
    //-------------------------------------------------------------------------
    uint64 VulkanAsyncComputeQueue::end()
    {
        if( !mOpen )
        {
            OGRE_EXCEPT( Exception::ERR_INVALID_STATE, "end without begin",
                         "VulkanAsyncComputeQueue::end" );
        }
        if( !mOpenName.empty() )
            mDevice->mRenderSystem->debugAnnotationPop();
        mOpen = false;
        mDevice->mJahInBracket = false;
        if( mOpenAsync )
        {
            // The compute queue's encoders end first (its compute bindings die with its command
            // buffer), then the graphics queue takes the recording back.
            mQueue->endAllEncoders();
            mDevice->mJahRecordQueue = &mDevice->mGraphicsQueue;
            mDevice->mGraphicsQueue._jahSetBlocked( false );
        }
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
        mQueue->commitAndNextCommandBuffer( SubmissionType::FlushOnly );
        mSlotValues[mSlot] = mComputeValue;
        ++mNumAsyncBrackets;
        return mComputeValue;
    }
    //-------------------------------------------------------------------------
    void VulkanAsyncComputeQueue::waitAt( uint64 computeValue, VkPipelineStageFlags dstStage )
    {
        // A value no bracket has signalled would make the next graphics submission wait for
        // ever (the ring's fence wait never returns): refused, at runtime.
        if( computeValue > mComputeValue )
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS, "a compute value no bracket has signalled",
                         "VulkanAsyncComputeQueue::waitAt" );
        }
        // The in-order arm waits for nothing: its brackets precede on the one queue, and the
        // async brackets before a setEnabled( false ) are waited by that switch.
        if( !isAsync() || !computeValue )
            return;
        // Everything recorded so far goes first, unblocked; the wait holds what follows.
        mDevice->commitAndNextCommandBuffer( SubmissionType::FlushOnly );
        mDevice->mGraphicsQueue.addWaitSemaphore( mComputeTimeline, dstStage, computeValue );
    }
    //-------------------------------------------------------------------------
    void VulkanAsyncComputeQueue::_notifySlotReuse( uint64 frameCount )
    {
        if( mFrameBrackets.empty() )
            return;
        VulkanVaoManager *vaoManager = mDevice->mVaoManager;
        const uint64 ring = vaoManager->getDynamicBufferMultiplier();
        size_t retired = 0u;
        while( retired < mFrameBrackets.size() &&
               mFrameBrackets[retired].recordFrame + ring <= frameCount )
        {
            const FrameBracket &fb = mFrameBrackets[retired];
            // The bracket's frame data (its descriptor sets, its dynamic and auto-param data) is
            // read by the COMPUTE queue only: C passing the bracket is the whole rule. (The
            // graphics frame that waited reads the bracket's OUTPUTS, never its ring data.)
            bool held = false;
            if( getCompletedComputeValue() < fb.value )
            {
                held = true;
                hostWaitCompute( fb.value );
            }
            if( getCompletedComputeValue() < fb.value )
                ++mRecycleViolations;   // only after a device loss
            ++mSlotRecycles;
            if( held )
                ++mRecyclesHeld;
            ++retired;
        }
        if( retired )
            mFrameBrackets.erase( mFrameBrackets.begin(), mFrameBrackets.begin() + retired );
    }
    //-------------------------------------------------------------------------
    bool VulkanAsyncComputeQueue::_isFrameDataConsumed( uint64 frameCount ) const
    {
        // The brackets are oldest first and their values increase: the last one recorded at or
        // before `frameCount` is the one C must have passed.
        uint64 need = 0u;
        for( const FrameBracket &fb : mFrameBrackets )
        {
            if( fb.recordFrame > frameCount )
                break;
            need = fb.value;
        }
        return need == 0u || getCompletedComputeValue() >= need;
    }
    //-------------------------------------------------------------------------
    void VulkanAsyncComputeQueue::waitAtFrameEnd( uint64 computeValue )
    {
        // A value no bracket has signalled would make the next graphics submission wait for
        // ever (the ring's fence wait never returns): refused, at runtime.
        if( computeValue > mComputeValue )
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS, "a compute value no bracket has signalled",
                         "VulkanAsyncComputeQueue::waitAtFrameEnd" );
        }
        if( !isAsync() || !computeValue )
            return;
        mDevice->mGraphicsQueue.addFrameEndWaitSemaphore(
            mComputeTimeline, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, computeValue );
    }
    //-------------------------------------------------------------------------
    bool VulkanAsyncComputeQueue::_closeLeakedBracket( const char *where )
    {
        if( !mOpen )
            return false;
        LogManager::getSingleton().logMessage(
            "Vulkan: WARNING: an async compute bracket ('" + mOpenName +
                "') was left open by a throw; closed and submitted at " + String( where ),
            LML_CRITICAL );
        end();
        return true;
    }
    //-------------------------------------------------------------------------
    VulkanAsyncComputeBracket::~VulkanAsyncComputeBracket()
    {
        if( !mQueue || !mQueue->isOpen() )
            return;
        try
        {
            mQueue->end();
        }
        catch( Exception &e )
        {
            // A destructor never throws (it may run while another exception unwinds).
            LogManager::getSingleton().logMessage(
                "Vulkan: an async compute bracket closed by its scope failed to submit: " +
                    e.getFullDescription(),
                LML_CRITICAL );
        }
    }
    //-------------------------------------------------------------------------
    uint64 VulkanAsyncComputeBracket::end()
    {
        if( !mQueue )
        {
            OGRE_EXCEPT( Exception::ERR_INVALID_STATE, "the bracket was already ended",
                         "VulkanAsyncComputeBracket::end" );
        }
        VulkanAsyncComputeQueue *queue = mQueue;
        mQueue = 0;
        return queue->end();
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
