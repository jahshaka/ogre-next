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

#ifndef _OgreVulkanQueue_H_
#define _OgreVulkanQueue_H_

#include "OgreVulkanPrerequisites.h"

#include "OgreTextureGpu.h"
#include "Vao/OgreBufferPacked.h"

#include "vulkan/vulkan_core.h"

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class _OgreVulkanExport VulkanQueue
    {
    public:
        enum QueueFamily
        {
            Graphics,
            Compute,
            Transfer,
            NumQueueFamilies
        };

        struct PerFrameData
        {
            VkCommandPool mCmdPool;
            FastArray<VkCommandBuffer> mCommands;
            size_t mCurrentCmdIdx;
            FastArray<VkFence> mProtectingFences;
        };

        enum EncoderState
        {
            EncoderGraphicsOpen,
            EncoderComputeOpen,
            EncoderCopyOpen,
            EncoderClosed
        };

        VkDevice mDevice;
        QueueFamily mFamily;

        uint32 mFamilyIdx;

        uint32 getFamilyIdx() const { return mFamilyIdx; }

        uint32 mQueueIdx;

        VkQueue mQueue;

        VulkanDevice *mOwnerDevice;

    protected:
        struct RefCountedFence
        {
            uint32 refCount;
            /// When true, the VkFence is no longer inside PerFrameData::mProtectingFences and it is
            /// VulkanQueue::releaseFence's responsibility to put the fence back into mAvailableFences
            bool recycleAfterRelease;
            RefCountedFence() {}
            RefCountedFence( uint32 _refCount ) : refCount( _refCount ), recycleAfterRelease( false ) {}
        };

        // clang-format off
        // One per buffered frame
        FastArray<PerFrameData> mPerFrameData;

        /// Collection of semaphore we need to wait on before our queue executes
        /// pending commands when commitAndNextCommandBuffer is called
        VkSemaphoreArray                mGpuWaitSemaphForCurrCmdBuff;
        FastArray<VkPipelineStageFlags> mGpuWaitFlags;
        /// Collection of semaphore we will signal when our queue
        /// submitted in commitAndNextCommandBuffer is done. Semaphores are owned.
        VkSemaphoreArray                mGpuSignalSemaphForCurrCmdBuff;
        // clang-format on

        /// Jahshaka (ENGINE-UPGRADE-ASYNC): the TIMELINE value of each entry of
        /// mGpuWaitSemaphForCurrCmdBuff (0 for a binary semaphore -- a swapchain's acquire),
        /// kept index-aligned with it and with mGpuWaitFlags.
        FastArray<uint64> mGpuWaitValues;
        /// Jahshaka (ENGINE-UPGRADE-ASYNC): semaphores the NEXT submission signals, of ANY
        /// submission type (the present semaphores above are only signalled at NewFrameIdx and
        /// later). Not owned. mJahSignalValues is index-aligned (0 = binary).
        VkSemaphoreArray  mJahSignalSemaph;
        FastArray<uint64> mJahSignalValues;
        /// Jahshaka (ENGINE-UPGRADE-ASYNC): waits held back until the next submission that
        /// advances the frame (NewFrameIdx / EndFrameAndSwap), so the frame's fence also
        /// proves what they wait for.
        VkSemaphoreArray                mJahFrameEndWaitSemaph;
        FastArray<VkPipelineStageFlags> mJahFrameEndWaitFlags;
        FastArray<uint64>               mJahFrameEndWaitValues;
        /// Jahshaka (ENGINE-UPGRADE-ASYNC): THE OWN-SLOT MODE of the async compute queue. Its
        /// command buffers come from ITS OWN ring of mPerFrameData slots, chosen by the
        /// owner (VulkanAsyncComputeQueue::begin) instead of the VaoManager's graphics frame
        /// index; it records only between _jahBeginSlot and the next commit (no command
        /// buffer is open in between), never waits the graphics ring and never advances a
        /// frame. Off (every other queue): upstream's behaviour exactly.
        bool   mJahOwnSlots;
        size_t mJahSlot;
        /// Jahshaka (ENGINE-UPGRADE-ASYNC stage 2): see _jahSetBlocked.
        bool   mJahBlocked;

        typedef map<VkFence, RefCountedFence>::type RefCountedFenceMap;

        VkFenceArray mAvailableFences;
        /// Fences which haven't been released.
        /// mCurrentFence is not included even if mCurrentFenceRefCount > 0
        RefCountedFenceMap mRefCountedFences;

    public:
        FastArray<VulkanWindowSwapChainBased *> mWindowsPendingSwap;

    protected:
        VkCommandBuffer mCurrentCmdBuffer;
        FastArray<VkCommandBuffer> mPendingCmds;

        VulkanVaoManager *mVaoManager;
        VulkanRenderSystem *mRenderSystem;

        VkFence mCurrentFence;
        /// Starts at 0. Increases with each acquireCurrentFence
        /// If commitAndNextCommandBuffer gets called with mCurrentFenceRefCount == 0,
        /// then mCurrentFence can enter into the recycle pool
        uint32 mCurrentFenceRefCount;

        typedef map<const BufferPacked *, bool>::type BufferPackedDownloadMap;
        typedef map<VulkanTextureGpu *, bool>::type TextureGpuDownloadMap;

        EncoderState mEncoderState;
        VkAccessFlags mCopyEndReadSrcBufferFlags;
        VkAccessFlags mCopyEndReadDstBufferFlags;
        VkAccessFlags mCopyEndReadDstTextureFlags;
        VkAccessFlags mCopyStartWriteSrcBufferFlags;
        FastArray<VkImageMemoryBarrier> mImageMemBarriers;
        FastArray<TextureGpu *> mImageMemBarrierPtrs;
        /// When mCopyDownloadTextures[buffer] = true, this buffer has been last downloaded
        /// When mCopyDownloadTextures[buffer] = false, this buffer has been last uploaded
        /// When mCopyDownloadTextures[buffer] = not_found, this buffer hasn't been downloaded/uploaded
        TextureGpuDownloadMap mCopyDownloadTextures;
        /// When mCopyDownloadBuffers[buffer] = true, this buffer has been last downloaded
        /// When mCopyDownloadBuffers[buffer] = false, this buffer has been last uploaded
        /// When mCopyDownloadBuffers[buffer] = not_found, this buffer hasn't been downloaded/uploaded
        BufferPackedDownloadMap mCopyDownloadBuffers;

        /// Returns a signaled fence, could be recycled or new
        VkFence getFence();
        /// Puts all input fences into mAvailableFences for recycling,
        /// unless their external reference count isn't 0
        ///
        /// Clears fences.
        void recycleFences( FastArray<VkFence> &fences );

        inline VkFence getCurrentFence();

        VkCommandBuffer getCmdBuffer( size_t currFrame );

        static VkPipelineStageFlags deriveStageFromBufferAccessFlags( VkAccessFlags accessFlags );
        static VkPipelineStageFlags deriveStageFromTextureAccessFlags( VkAccessFlags accessFlags );

        /// Schedules a barrier to be issued in endCopyEncoder
        /// This is to RESTORE the texture's layout and force future rendering cmds to wait
        /// for our transfer to finish.
        void insertRestoreBarrier( VulkanTextureGpu *vkTexture, const VkImageLayout newTransferLayout );

        /** There will be two barriers that prepareForUpload will generate:

                1. We must wait until previous commands are done reading from/writing to dst
                   before we can proceed with a copy command what writes to dst.<br/>
                   That includes previous copy commands, but we don't because we assume two
                   consecutive uploads to the same buffer (or textures) are always done to
                   non-overlapping regions.
                2. At endCopyEncoder: we must ensure future GPU commands wait for our copy
                   command to end writing to dst before they can start.

            prepareForUpload tries its best to generate no more than 2 barriers (except for
            texture layout transitions) between getCopyEncoder and endCopyEncoder, but sometimes
            more than 2 barriers may end up being issued
        @param buffer
        @param texture
        */
        void prepareForUpload( const BufferPacked *buffer, TextureGpu *texture,
                               CopyEncTransitionMode::CopyEncTransitionMode transitionMode );

        /** There between zero to two barriers that prepareForDownload will generate:

                1. We must wait until previous commands are done writing to src before we can
                   proceed with a copy command that reads from src. Some buffers/textures are
                   guaranteed to be read-only, in this case we can avoid this barrier.
                   Textures may still need to transition their layout though.
                2. At endCopyEncoder: we must ensure future commands don't write to src before
                   we're done reading from src. Again, buffers/textures which are guaranteed to
                   be read-only don't need this.
                   Textures may still need to transition their layout though

            prepareForDownload tries its best to generate no more than 2 barriers (except for
            texture layout transitions) between getCopyEncoder and endCopyEncoder, but sometimes
            more than 2 barriers may end up being issued
        @param buffer
        @param texture
        */
        void prepareForDownload( const BufferPacked *buffer, TextureGpu *texture,
                                 CopyEncTransitionMode::CopyEncTransitionMode transitionMode );

    public:
        VulkanQueue();
        ~VulkanQueue();

        /// Jahshaka: SHADER_READ for a vertex/index buffer when buffer device addresses
        /// are on - a compute shader can read the pool through a pointer and the barrier
        /// solver cannot see it. See the definition for the full reasoning.
        VkAccessFlags jahBufferDeviceAddressAccess( const BufferPacked *buffer ) const;

        void setQueueData( VulkanDevice *owner, QueueFamily family, uint32 familyIdx, uint32 queueIdx );
        void setExternalQueue( VulkanDevice *owner, QueueFamily family, VkQueue queue );

        void init( VkDevice device, VkQueue queue, VulkanRenderSystem *renderSystem );
        void destroy();

    protected:
        void newCommandBuffer();
        void endCommandBuffer();

    public:
        VkCommandBuffer getCurrentCmdBuffer()
        {
            if( mJahBlocked )
                _jahThrowBlocked( "a command on the graphics queue (getCurrentCmdBuffer)" );
            if( !mCurrentCmdBuffer )  // device lost or any other error happens near vkSubmitQueue()
                checkVkResult( mOwnerDevice, VK_ERROR_DEVICE_LOST, "getCurrentCmdBuffer" );
            return mCurrentCmdBuffer;
        }
        /// Jahshaka (ENGINE-UPGRADE-ASYNC stage 2): THE CATCH-ALL. While an async compute
        /// bracket is open the graphics queue is BLOCKED: anything that reaches for its command
        /// buffer or an encoder throws, naming what it reached for (the render system's named
        /// checks fire first where they apply). A runtime check, not an OGRE_ASSERT: the
        /// install is OGRE_DEBUG_MODE 0.
        void _jahSetBlocked( bool blocked ) { mJahBlocked = blocked; }
        bool _jahIsBlocked() const { return mJahBlocked; }
        void _jahThrowBlocked( const char *what ) const;
        /// Throws when a copy encoder is asked for on the graphics queue inside a bracket (either arm).
        void _jahCheckCopy() const;
        EncoderState getEncoderState() const { return mEncoderState; }

        void getGraphicsEncoder();
        void getComputeEncoder();
        /** Call this function when you need to start copy/transfer operations
        @remarks
            buffer and texture pointers cannot be both nullptr at the same time

            You don't have to pair every getCopyEncoder call with an endCopyEncoder call. In
            fact this is discouraged.

            Keep calling getCopyEncoder until you're done with all transfer operations

            @see    VulkanQueue::prepareForUpload
            @see    VulkanQueue::prepareForDownload
        @param buffer
            The buffer we're copying from/to. Can be nullptr
        @param texture
            The texture we're copying from/to. Can be nullptr

            If uploading, the texture will be transitioned to VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
            If downloading, the texture will be transitioned to VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
        @param bDownload
            True if we plan to do CPU -> GPU transfers
            False if we plan to do GPU -> CPU transfers

            If you want to perform GPU -> GPU transfers, then you need to call:
            @code
                queue->getCopyEncoder( src, src, true );
                queue->getCopyEncoder( dst, dst, false );
            @endcode
        @param transitionMode
            Only used if texture != nullptr
            See CopyEncTransitionMode::CopyEncTransitionMode
        */
        void getCopyEncoder( const BufferPacked *buffer, TextureGpu *texture, const bool bDownload,
                             CopyEncTransitionMode::CopyEncTransitionMode transitionMode );
        void getCopyEncoderAsyncTextureTicketUpload();
        void getCopyEncoderV1Buffer( const bool bDownload );

        void endCopyEncoder();
        void endRenderEncoder( const bool endRenderPassDesc = true );
        void endComputeEncoder();

        void endAllEncoders( bool endRenderPassDesc = true );

        void notifyTextureDestroyed( VulkanTextureGpu *texture );

        VkFence acquireCurrentFence();
        void releaseFence( VkFence fence );

        /// When we'll call commitAndNextCommandBuffer, we'll have to wait for
        /// this semaphore on to execute STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        void addWindowToWaitFor( VkSemaphore imageAcquisitionSemaph );

        /// If this function returns false, waiting on the fence would cause a deadlock since
        /// it will never signal. You must call commitAndNextCommandBuffer before waiting.
        bool isFenceFlushed( VkFence fence ) const;

        void _waitOnFrame( uint8 frameIdx );
        bool _isFrameFinished( uint8 frameIdx );

        void commitAndNextCommandBuffer(
            SubmissionType::SubmissionType submissionType = SubmissionType::FlushOnly );

        /// Jahshaka (ENGINE-UPGRADE-ASYNC): a GENERAL wait for the next submission (upstream's
        /// only wait was addWindowToWaitFor: a binary acquire semaphore at COLOR_ATTACHMENT_OUTPUT).
        /// timelineValue != 0 waits a TIMELINE semaphore for that value.
        void addWaitSemaphore( VkSemaphore semaphore, VkPipelineStageFlags dstStage,
                               uint64 timelineValue = 0u );
        /// Jahshaka (ENGINE-UPGRADE-ASYNC): the next submission (of any type) signals this
        /// semaphore (timelineValue != 0: a timeline set to that value). A pending signal
        /// forces the submission even with no command recorded.
        void addSignalSemaphore( VkSemaphore semaphore, uint64 timelineValue = 0u );
        /// Jahshaka (ENGINE-UPGRADE-ASYNC): a wait attached to the next frame-advancing
        /// submission only (NewFrameIdx / EndFrameAndSwap).
        void addFrameEndWaitSemaphore( VkSemaphore semaphore, VkPipelineStageFlags dstStage,
                                       uint64 timelineValue );

        /// Jahshaka (ENGINE-UPGRADE-ASYNC): the own-slot mode (see mJahOwnSlots). Set before init().
        void _jahSetOwnSlots( bool ownSlots ) { mJahOwnSlots = ownSlots; }
        bool _jahHasOwnSlots() const { return mJahOwnSlots; }
        /// Own-slot mode: open a command buffer from `slot`. The slot's command index is
        /// rewound first, so its pool is RESET -- the caller guarantees the GPU is done with
        /// everything submitted from that slot.
        void _jahBeginSlot( size_t slot );
        bool _jahIsRecording() const { return mCurrentCmdBuffer != 0; }
        size_t _jahNumSlots() const { return mPerFrameData.size(); }
        /// Counts for the rows: every command buffer ever allocated (all slots), and every
        /// fence this queue owns (available + protecting + externally referenced).
        size_t _jahNumAllocatedCmdBuffers() const;
        size_t _jahNumFences() const;

        /// Jahshaka (ENGINE-UPGRADE-ASYNC, the stage-2 read's W1): the pipeline stages a
        /// barrier recorded on THIS queue may name -- the device's (mSupportedStages) ANDed with
        /// what the queue's family supports. A compute-only family has no vertex, fragment,
        /// attachment or early/late test stages (VUID-vkCmdPipelineBarrier-srcStageMask-06461 /
        /// -dstStageMask-06462); the graphics family keeps the device's mask unchanged.
        VkPipelineStageFlags _jahSupportedStages() const;
        /// The access bits a barrier on this queue may keep on a side whose (already fitted)
        /// stage mask is `stages` (VUID-vkCmdPipelineBarrier-srcAccessMask-02815 / -02816).
        /// The graphics family: every bit (upstream's behaviour).
        VkAccessFlags _jahSupportedAccess( VkPipelineStageFlags stages ) const;
        /// Fits a barrier to this queue: both stage masks ANDed with _jahSupportedStages()
        /// (a source side emptied by the fit becomes ALL_COMMANDS so it chains to the timeline
        /// wait; an empty side otherwise TOP_OF_PIPE / BOTTOM_OF_PIPE) and, on a family without
        /// graphics, every access mask to what its fitted side's stages can access. What a
        /// dropped graphics stage ordered on the other queue is ordered by the timeline wait
        /// that brought the work here (its signal makes every write available and visible).
        void _jahFitBarrier( VkPipelineStageFlags &srcStage, VkPipelineStageFlags &dstStage,
                             VkMemoryBarrier *memBarriers, uint32 numMemBarriers,
                             VkImageMemoryBarrier *imageBarriers, uint32 numImageBarriers ) const;
        /// Jahshaka (ENGINE-UPGRADE-ASYNC-3 stage 1): the copy encoder's barriers, recorded on THIS
        /// queue -- upstream's on the graphics family, fitted (_jahFitBarrier) on another one.
        void _jahCopyBarrier( VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                              uint32 numMemBarriers, VkMemoryBarrier *memBarriers,
                              uint32 numImageBarriers, VkImageMemoryBarrier *imageBarriers );

        VulkanVaoManager *getVaoManager() { return mVaoManager; }
    };

}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
