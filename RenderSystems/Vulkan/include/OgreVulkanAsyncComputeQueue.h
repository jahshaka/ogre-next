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

#ifndef _OgreVulkanAsyncComputeQueue_H_
#define _OgreVulkanAsyncComputeQueue_H_

#include "OgreVulkanPrerequisites.h"

#include "OgreVulkanQueue.h"

#include "vulkan/vulkan_core.h"

namespace Ogre
{
    /** Jahshaka (ENGINE-UPGRADE-ASYNC): THE ASYNC COMPUTE QUEUE, the fork's Component for work that
        runs on a second hardware queue beside the graphics queue.

        Two TIMELINE semaphores order the two queues: G, signalled by the graphics queue at any
        named point of its frame (signalPoint), and C, signalled by the compute queue at the end
        of each bracket (end). Nothing about WHICH point the compute work waits for, or where
        graphics waits for its result, is decided here: the caller passes the values (the
        protocol lives with the caller -- ENGINE-UPGRADE-ASYNC's "the GI one frame behind its
        inputs" is one such protocol).

            const uint64 g = acq->signalPoint( "after the last GI reader" ); // graphics, frame N-1
            acq->begin( g );          // compute records (stage 2: the render system follows)
            const uint64 c = acq->end();                                     // submitted, C = c
            ...
            acq->waitAt( c, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT );         // graphics, frame N

        THE OFF ARM: no compute-capable second queue on the device, timeline semaphores
        unavailable, JAH_VK_ASYNC_COMPUTE=0 in the environment, or setEnabled( false ). Every
        call keeps its values (the counters advance exactly as on) and does no GPU work: the
        bracket's work records in order on the graphics queue, so the picture is the in-order
        picture by construction; waitAt / waitAtFrameEnd wait for nothing (order on the one
        queue does it). With a compute queue present but disabled, end() still has the graphics
        queue signal C = its value, so C stays ONE monotonic timeline across switches.

        RECORDING (stage 2): between begin and end the render system's compute work -- the
        compute PSO, the root layout's descriptor sets, the dispatches, the resource barriers,
        the labels and the GPU timestamps -- records into the compute queue (VulkanDevice::
        _jahRecordQueue); the graphics queue is BLOCKED and anything graphics-only (a render
        pass, a draw, a graphics PSO, a copy or an upload, a mipmap blit, a texture creation)
        THROWS an Ogre::Exception naming it -- in the off arm too, so a bracket that works off
        works on. Resources both queues touch are CONCURRENT (TextureFlags::AsyncComputeShared;
        every VBO pool but the readback one): no ownership transfers, the timelines carry the
        memory dependency, each queue records its own barriers through the one BarrierSolver.

        THE FRAME THAT WAITS OWNS THE BRACKET'S FRAME DATA: what a bracket allocates from the
        graphics frame ring (dynamic buffers, auto-param buffers, descriptor sets) lives in the
        slot of the frame that RECORDED it, but it is consumed until C passes the bracket and
        read by the graphics frame that WAITED for it. So that slot is recycled only after C has
        passed every bracket recorded in it AND the graphics frame that waited each one has
        finished (_notifySlotReuse, from VulkanVaoManager::waitForTailFrameToFinish).

        LIFETIME: the bracket's command buffers come from a ring of the compute queue's own
        slots (as many as the graphics ring has frames); begin() reuses a slot only after C
        has passed the value of the bracket that last used it (normally long done: a host wait
        of zero). So the command buffers and fences are bounded by the slot count whatever
        protocol the caller runs -- the compute queue never advances a frame.
    */
    class _OgreVulkanExport VulkanAsyncComputeQueue
    {
    public:
        VulkanAsyncComputeQueue( VulkanDevice *device, VulkanQueue *computeQueue,
                                 bool timelineSemaphores );
        ~VulkanAsyncComputeQueue();

        /// True when the device has the second queue and the timelines (whatever setEnabled says).
        bool hasComputeQueue() const { return mQueue != 0; }
        /// True when brackets really run on the compute queue (hasComputeQueue and enabled).
        bool isAsync() const { return mQueue != 0 && mEnabled; }
        bool isEnabled() const { return mEnabled; }
        /// Switch between the async arm and the in-order arm. Only outside a bracket.
        /// Disabling makes the next graphics submission wait for every bracket submitted so
        /// far; enabling signals a graphics point no later bracket may start before.
        void setEnabled( bool bEnabled );

        /// Graphics queue: everything recorded so far is submitted and signals G = the
        /// returned value when done. `name` labels the point (the pass-label switch).
        uint64 signalPoint( const char *name );
        /// Opens the bracket. Its work starts on the GPU once G >= afterGraphicsPoint
        /// (0 = no wait; values below the enable point are raised to it).
        void begin( uint64 afterGraphicsPoint, const char *name = 0 );
        bool isOpen() const { return mOpen; }
        /// Closes and submits the bracket; C = the returned value when its work is done.
        uint64 end();
        /// Graphics queue: what is recorded from here on starts its `dstStage` work only once
        /// C >= computeValue (everything recorded before is submitted first, unblocked).
        void waitAt( uint64 computeValue, VkPipelineStageFlags dstStage );
        /// Graphics queue: the next frame-advancing submission waits C >= computeValue, so
        /// that frame's fence also proves the compute work done.
        void waitAtFrameEnd( uint64 computeValue );

        uint64 getLastGraphicsPoint() const { return mGraphicsValue; }
        uint64 getLastComputeValue() const { return mComputeValue; }
        /// What the GPU has reached on C (the counter itself in the off arm).
        uint64 getCompletedComputeValue() const;
        /// Brackets that really ran on the compute queue.
        uint64 getNumAsyncBrackets() const { return mNumAsyncBrackets; }

        VulkanQueue *getQueue() const { return mQueue; }
        /// The compute queue's family (the graphics family's in the off arm).
        uint32 getFamilyIdx() const;
        uint32 getQueueIdx() const;
        size_t getNumSlots() const;
        size_t getNumCommandBuffers() const;
        size_t getNumFences() const;

        /// Called by VulkanDevice before the semaphores' device dies.
        void _destroy();

        /// True when the compute family writes timestamps (timestampValidBits != 0): the
        /// render system's GPU samples inside a bracket are then timed on the compute queue.
        bool hasTimestamps() const { return mTimestamps; }

        /// The graphics frame ring is about to recycle the slot of frame `frameCount` minus the
        /// ring's length (VulkanVaoManager::waitForTailFrameToFinish, its graphics fences
        /// already waited): holds it until the brackets recorded in it are consumed.
        void _notifySlotReuse( uint64 frameCount );
        /// THE COUNTS of the frame-data rule: brackets whose recording slot was recycled, how
        /// many of those recycles had to wait for C or for the waiting graphics frame, and
        /// recycles that found the rule broken after the waits (must be 0).
        uint64 getNumSlotRecycles() const { return mSlotRecycles; }
        uint64 getNumRecyclesHeld() const { return mRecyclesHeld; }
        uint64 getNumRecycleViolations() const { return mRecycleViolations; }

    protected:
        VkSemaphore createTimeline();
        void hostWaitCompute( uint64 value );
        void markWaited( uint64 computeValue );

        VulkanDevice *mDevice;
        VulkanQueue *mQueue;  ///< null = the off arm for good
        VkSemaphore mGraphicsTimeline;
        VkSemaphore mComputeTimeline;
        uint64 mGraphicsValue;
        uint64 mComputeValue;
        uint64 mMinAfter;
        uint64 mAfter;
        bool mEnabled;
        bool mOpen;
        bool mOpenAsync;
        size_t mSlot;
        uint64 mNumAsyncBrackets;
        FastArray<uint64> mSlotValues;
        bool mTimestamps;
        String mOpenName;

        struct FrameBracket
        {
            uint64 recordFrame;  ///< VaoManager frame count while it recorded
            uint64 value;        ///< its C value
            uint64 waitedFrame;  ///< the graphics frame whose waitAt covered it (~0 = none yet)
        };
        FastArray<FrameBracket> mFrameBrackets;  ///< oldest first
        uint64 mSlotRecycles;
        uint64 mRecyclesHeld;
        uint64 mRecycleViolations;
    };
}  // namespace Ogre

#endif
