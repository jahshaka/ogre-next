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

#ifndef _OgreVulkanDevice_H_
#define _OgreVulkanDevice_H_

#include "OgreVulkanPrerequisites.h"

#include "OgreVulkanQueue.h"

#include "vulkan/vulkan_core.h"

#include <atomic>
#include <mutex>

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    /// Use it to pass an external instance
    ///
    /// We will verify if the layers and extensions you claim
    /// were enabled are actually supported.
    ///
    /// This is so because in Qt you can request these layers/extensions
    /// but you get no feedback from Qt whether they were present and
    /// thus successfully enabled.
    ///
    /// However if the instance actually supports the layer/extension
    /// you requested but the third party library explicitly chose not to
    /// enable it for any random reason, then we will wrongly think
    /// it's enabled / present.
    struct VulkanExternalInstance
    {
        VkInstance instance;
        FastArray<VkLayerProperties> instanceLayers;
        FastArray<VkExtensionProperties> instanceExtensions;
    };

    /// Use it to pass an external device
    ///
    /// See VulkanExternalInstance on extensions verification.
    struct VulkanExternalDevice
    {
        VkPhysicalDevice physicalDevice;
        VkDevice device;
        FastArray<VkExtensionProperties> deviceExtensions;
        VkQueue graphicsQueue;
        VkQueue presentQueue;
    };

    /**
       We need the ability to re-enumerate devices to handle physical device removing, that
       requires fresh VkInstance instance, as otherwise Vulkan returns obsolete physical devices list.
    */
    // Jahshaka (VR-START-1): EXPORTED, so the OpenXR connection can check the
    // runtime's instance-extension list against what this instance enabled.
    class _OgreVulkanExport VulkanInstance final
    {
    public:
        static void enumerateExtensionsAndLayers( VulkanExternalInstance *externalInstance );
        static bool hasExtension( const char *extension );
        /// Jahshaka (ENGINE-UPGRADE-ASYNC stage 0): THE PASS-LABEL SWITCH. True when the
        /// environment sets JAH_VK_PASS_LABELS (not "0"): VK_EXT_debug_utils is enabled at any
        /// OGRE_DEBUG_MODE, every compositor pass pushes its profiling id as a command-buffer
        /// label, and the render system splits the frame's head (everything recorded between
        /// the previous frame's swap and the compositor's first pass) into its own submission
        /// named "jah.frame.head" -- what a Nsight capture needs to see a pre-frame job on its
        /// own. Off (the default): no extension, no label, no extra submission.
        static bool jahPassLabelsRequested();

        VulkanInstance( const String &appName, VulkanExternalInstance *externalInstance,
                        PFN_vkDebugReportCallbackEXT debugCallback, RenderSystem *renderSystem );
        ~VulkanInstance();

        void initDebugFeatures( PFN_vkDebugReportCallbackEXT callback, void *userdata,
                                bool hasRenderDocApi );

        void initPhysicalDeviceList();

        // never fail but can return default driver if requested is not found
        const VulkanPhysicalDevice *findByName( const String &name ) const;

    public:
        VkInstance mVkInstance;
        bool mVkInstanceIsExternal;
        /// Jahshaka (VR-START-1): the VkApplicationInfo::apiVersion this instance was
        /// created with (0 for an external instance - its creator knows). An OpenXR
        /// runtime's XR_KHR_vulkan_enable minimum is checked against it.
        uint32 mApiVersion;

        FastArray<VulkanPhysicalDevice> mVulkanPhysicalDevices;

        PFN_vkCreateDebugReportCallbackEXT CreateDebugReportCallback;
        PFN_vkDestroyDebugReportCallbackEXT DestroyDebugReportCallback;
        VkDebugReportCallbackEXT mDebugReportCallback;

        PFN_vkCmdBeginDebugUtilsLabelEXT CmdBeginDebugUtilsLabelEXT;
        PFN_vkCmdEndDebugUtilsLabelEXT CmdEndDebugUtilsLabelEXT;

        static FastArray<const char *> enabledExtensions;  // sorted
        static FastArray<const char *> enabledLayers;      // sorted
#if OGRE_DEBUG_MODE >= OGRE_DEBUG_HIGH
        static bool hasValidationLayers;
#endif
    };

    struct _OgreVulkanExport VulkanDevice
    {
        struct SelectedQueue
        {
            VulkanQueue::QueueFamily usage;
            uint32 familyIdx;
            uint32 queueIdx;
            SelectedQueue();
        };

        struct ExtraVkFeatures
        {
            // VkPhysicalDevice16BitStorageFeatures
            VkBool32 storageInputOutput16;

            // VkPhysicalDeviceShaderFloat16Int8Features
            VkBool32 shaderFloat16;
            VkBool32 shaderInt8;

            // VkPhysicalDevicePipelineCreationCacheControlFeatures
            VkBool32 pipelineCreationCacheControl;

            // VkPhysicalDeviceMultiviewFeatures (VK_KHR_multiview, core in 1.1)
            VkBool32 multiview;
            // VkPhysicalDeviceMultiviewProperties::maxMultiviewViewCount (0 without multiview)
            uint32 maxMultiviewViewCount;

            // Jahshaka (ENGINE-UPGRADE-ASYNC): VkPhysicalDeviceTimelineSemaphoreFeatures (core
            // in Vulkan 1.2; asked only when the instance AND the device are 1.2)
            VkBool32 timelineSemaphore;
        };

        /// Jahshaka (fork d014b064f (was 0038)): the feature structs a hardware ray-query tier
        /// needs at vkCreateDevice time. They are MEMBERS, not locals, because the
        /// pNext chain they are linked into must outlive fillDeviceFeatures2() and
        /// stay valid until vkCreateDevice() reads it.
        struct RayQueryVkFeatures
        {
            VkPhysicalDeviceAccelerationStructureFeaturesKHR accelStruct;
            VkPhysicalDeviceRayQueryFeaturesKHR rayQuery;
            VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddress;
            VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexing;
            /// True only when every extension the tier needs was found AND the
            /// driver reported both accelerationStructure and rayQuery.
            bool enabled;
            /// Jahshaka: TRUE WHEN BUFFER DEVICE ADDRESSES ARE ON, WHICH IS NOT THE
            /// SAME QUESTION AS `enabled`. The two used to be one bit, and that made
            /// the whole feature a property of the ray-query tier: a device brought up
            /// with rays OFF (the selftest's no-rays pose, a driver without
            /// VK_KHR_ray_query, lavapipe) had no addresses either, so a compute job
            /// that reads geometry through an address - the voxelizer - would have had
            /// to keep a second, non-address code path alive forever. Addresses are a
            /// plain buffer feature; they are enabled whenever the driver has them.
            bool bufferDeviceAddressEnabled;
        };

        // clang-format off
        std::shared_ptr<VulkanInstance> mInstance;
        VkPhysicalDevice    mPhysicalDevice;
        VkDevice            mDevice;
        VkPipelineCache     mPipelineCache;

        VkQueue             mPresentQueue;
        /// Graphics queue is *guaranteed by spec* to also be able to run compute and transfer
        /// A GPU may not have a graphics queue though (Ogre can't run there)
        VulkanQueue             mGraphicsQueue;
        /// Additional compute queues to run async compute (besides the main graphics one)
        FastArray<VulkanQueue>  mComputeQueues;
        /// Additional transfer queues to run async transfers (besides the main graphics one)
        FastArray<VulkanQueue>  mTransferQueues;
        // clang-format on

        VkPhysicalDeviceProperties mDeviceProperties;
        VkPhysicalDeviceMemoryProperties mDeviceMemoryProperties;
        VkPhysicalDeviceFeatures mDeviceFeatures;
        ExtraVkFeatures mDeviceExtraFeatures;
        RayQueryVkFeatures mRayQueryFeatures;
        /// Jahshaka (ENGINE-UPGRADE-ASYNC): a member for the same reason as mRayQueryFeatures
        /// (the chain must live until vkCreateDevice reads it).
        VkPhysicalDeviceTimelineSemaphoreFeatures mTimelineFeatures;
        FastArray<VkQueueFamilyProperties> mQueueProps;

        /// Jahshaka (ENGINE-UPGRADE-ASYNC): THE ASYNC COMPUTE QUEUE Component, made with the
        /// queues (never null after initQueues; its off arm when there is no second queue).
        VulkanAsyncComputeQueue *mAsyncCompute;
        VulkanAsyncComputeQueue *getAsyncCompute() const { return mAsyncCompute; }
        /// Jahshaka (ENGINE-UPGRADE-ASYNC stage 2): THE CURRENT RECORDING QUEUE. The graphics
        /// queue, except inside an async compute bracket on the async arm, where it is the
        /// compute queue: the render system's compute work (the compute PSO, the root layout's
        /// descriptor sets, the dispatches, the barriers, the labels and the timestamps)
        /// records into it. mJahInBracket is true inside a bracket in EITHER arm, so the
        /// graphics-only checks fire the same way on and off.
        VulkanQueue *mJahRecordQueue;
        bool mJahInBracket;
        VulkanQueue &_jahRecordQueue() { return *mJahRecordQueue; }
        /// Jahshaka (ENGINE-UPGRADE-ASYNC-3 stage 1): THE ROUTED COPIES. A buffer copy is legal on a
        /// compute family (every compute queue supports transfer), so the staging uploads
        /// (VulkanStagingBuffer::unmapImpl), the buffer-to-buffer copies (VulkanBufferInterface::
        /// copyTo) and the buffer downloads (VulkanStagingBuffer::_asyncDownload) record on
        /// _jahRecordQueue() and say so with this scope: inside a bracket the copy check
        /// (VulkanQueue::_jahCheckCopy) refuses every copy that is NOT routed -- on the off arm too,
        /// where the record queue is the graphics queue -- so a taker of the graphics queue's copy
        /// encoder is never silent. Texture copies, the texture tickets and the v1 buffers stay
        /// graphics (they throw in a bracket).
        uint32 mJahRoutedCopy;
        struct JahRoutedCopy
        {
            VulkanDevice *d;
            explicit JahRoutedCopy( VulkanDevice *device ) : d( device ) { ++d->mJahRoutedCopy; }
            ~JahRoutedCopy() { --d->mJahRoutedCopy; }
        };
        /// True while the record queue is not the graphics queue (an async bracket is open).
        bool _jahRecordingOnCompute() const { return mJahRecordQueue != &mGraphicsQueue; }
        /// Jahshaka (ENGINE-UPGRADE-ASYNC stage 2): THE SHARED FAMILIES. True when the device has
        /// the async compute queue in a family other than the graphics one (and timelines):
        /// outCount/outFamilies then describe VK_SHARING_MODE_CONCURRENT over both. Decided at
        /// device creation (before any pool or image), constant for the device's life.
        bool _jahShareFamilies( uint32 &outCount, const uint32 *&outFamilies ) const
        {
            outCount = mJahShareFamilyCount;
            outFamilies = mJahShareFamilies;
            return mJahShareFamilyCount > 1u;
        }
        uint32 mJahShareFamilies[2];
        uint32 mJahShareFamilyCount;
        /// Throws (Ogre::Exception, ERR_INVALID_STATE) naming `what` when called inside a bracket.
        void _jahRequireGraphics( const char *what ) const
        {
            if( mJahInBracket )
                mGraphicsQueue._jahThrowBlocked( what );
        }
        /// Jahshaka (ENGINE-UPGRADE-ASYNC): false when the environment sets JAH_VK_ASYNC_COMPUTE=0
        /// (no second queue is requested: the off arm a device without one has).
        static bool jahAsyncComputeRequested();

        /// Extensions requested when created. Sorted
        FastArray<IdString> mDeviceExtensions;

        VulkanVaoManager *mVaoManager;
        VulkanRenderSystem *mRenderSystem;

        uint32 mSupportedStages;

        VkResult mDeviceLostReason;
        bool mIsExternal;

        /// Jahshaka: THE OUT-OF-MEMORY RECORD. onVulkanFailure no longer latches
        /// VK_ERROR_OUT_OF_HOST_MEMORY / VK_ERROR_OUT_OF_DEVICE_MEMORY as a device loss;
        /// it counts them here instead (from any thread), with the last one's text, so an
        /// application can tell that an OOM happened inside a window of its own - e.g. a
        /// frame, where a caller deep inside may have caught and swallowed the exception -
        /// and decide what that means. Read with getOutOfMemoryFailures() /
        /// getLastOutOfMemory().
        std::atomic<uint32> mOutOfMemoryFailures;
        mutable std::mutex mLastOutOfMemoryMutex;
        String mLastOutOfMemory;

        void _notifyOutOfMemory( const String &description );
        uint32 getOutOfMemoryFailures() const { return mOutOfMemoryFailures.load(); }
        String getLastOutOfMemory() const;

        void fillDeviceFeatures();
        bool fillDeviceFeatures2(
            VkPhysicalDeviceFeatures2 &deviceFeatures2,
            VkPhysicalDevice16BitStorageFeatures &device16BitStorageFeatures,
            VkPhysicalDeviceShaderFloat16Int8Features &deviceShaderFloat16Int8Features,
            VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT &deviceCacheControlFeatures,
            VkPhysicalDeviceMultiviewFeatures &deviceMultiviewFeatures );

        static void destroyQueues( FastArray<VulkanQueue> &queueArray );

        void findGraphicsQueue( FastArray<uint32> &inOutUsedQueueCount );
        void findComputeQueue( FastArray<uint32> &inOutUsedQueueCount, uint32 maxNumQueues );
        void findTransferQueue( FastArray<uint32> &inOutUsedQueueCount, uint32 maxNumQueues );

        void fillQueueCreationInfo( uint32 maxComputeQueues, uint32 maxTransferQueues,
                                    FastArray<VkDeviceQueueCreateInfo> &outQueueCiArray );

    public:
        VulkanDevice( VulkanRenderSystem *renderSystem );
        ~VulkanDevice();

        void destroy();

        void setPhysicalDevice( const std::shared_ptr<VulkanInstance> &instance,
                                const VulkanPhysicalDevice &physicalDevice,
                                const VulkanExternalDevice *externalDevice );

        void createDevice( const FastArray<VkExtensionProperties> &availableExtensions,
                           uint32 maxComputeQueues, uint32 maxTransferQueues );

        /// Jahshaka: THE NO-RAYS SWITCH, honoured at the DEVICE. false = nothing the
        /// ray-query work adds happens at all: not one ray extension is requested and
        /// not one ray feature bit is set, so the process runs on EXACTLY the device
        /// it would have had without that work - a real fallback picture, not a
        /// cosmetic one. It is also the escape hatch for a driver that cannot build a
        /// ray-tracing device in a particular process (MEASURED on NVIDIA 595.84:
        /// vkCreateDevice returns VK_ERROR_INITIALIZATION_FAILED with these features
        /// enabled under AddressSanitizer, on the very GPU where the same call
        /// succeeds without it). A process-wide static like Mesh::
        /// msOptimizeForShadowMapping, because the device can be built before any
        /// render system exists: the application sets it before it loads the render
        /// system. Default true.
        static bool msRayQueryAllowed;

        /// Jahshaka (fork d014b064f+1bccc3f93 (was 0068)): the device-extension names createDevice()
        /// would request on this physical device, in its exact order.
        static void fillDeviceExtensionRequest(
            VkPhysicalDevice physicalDevice,
            const FastArray<VkExtensionProperties> &availableExtensions,
            FastArray<const char *> &outExtensions );

        /// Jahshaka (fork d014b064f+1bccc3f93 (was 0068)): the VkPhysicalDeviceFeatures createDevice()
        /// would enable (Ogre's opt-in subset of what the driver reports).
        /// fillDeviceFeatures() calls this.
        static void fillDeviceFeaturesFor( VkPhysicalDevice physicalDevice,
                                           VkPhysicalDeviceFeatures &outFeatures );

        bool hasDeviceExtension( const IdString extension ) const;

        /// Jahshaka (fork d014b064f (was 0038)): true when VK_KHR_acceleration_structure +
        /// VK_KHR_ray_query were requested at vkCreateDevice and the driver reported
        /// their features. Nothing in Ogre reads this; it is the one honest way for a
        /// Jahshaka-owned compute pass to know whether it may build acceleration
        /// structures on this device.
        bool hasRayQuery() const { return mRayQueryFeatures.enabled; }

        /// Jahshaka: true when VK_KHR_buffer_device_address was requested at
        /// vkCreateDevice and the driver reported the feature - independently of the
        /// ray-query tier. The VBO pools a v2 vertex/index buffer lives in carry
        /// VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT exactly when this is true.
        bool hasBufferDeviceAddress() const { return mRayQueryFeatures.bufferDeviceAddressEnabled; }

        void initQueues();

        void commitAndNextCommandBuffer(
            SubmissionType::SubmissionType submissionType = SubmissionType::FlushOnly );

        /// Waits for the GPU to finish all pending commands.
        void stall();
        void stallIgnoringDeviceLost();

        bool isDeviceLost() const { return mDeviceLostReason != VK_SUCCESS; }

    private:
        /// Jahshaka (fork d014b064f+1bccc3f93 (was 0068)): the one implementation of the
        /// VkPhysicalDeviceFeatures2 pNext chain (fillDeviceFeatures2's).
        static bool buildFeatureChain(
            VkInstance instance, VkPhysicalDevice physicalDevice,
            const FastArray<IdString> &sortedDeviceExtensions,
            VkPhysicalDeviceFeatures2 &deviceFeatures2,
            VkPhysicalDevice16BitStorageFeatures &device16BitStorageFeatures,
            VkPhysicalDeviceShaderFloat16Int8Features &deviceShaderFloat16Int8Features,
            VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT &deviceCacheControlFeatures,
            VkPhysicalDeviceMultiviewFeatures &deviceMultiviewFeatures,
            RayQueryVkFeatures &rayQueryFeatures, ExtraVkFeatures &outExtraFeatures,
            uint32 instanceApiVersion, VkPhysicalDeviceTimelineSemaphoreFeatures &timelineFeatures );
    };

    // Mask away read flags from srcAccessMask
    static const uint32 c_srcValidAccessFlags =
        0xFFFFFFFF ^
        ( VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
          VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
          VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT |
          VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT |
          VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT |
          VK_ACCESS_COLOR_ATTACHMENT_READ_NONCOHERENT_BIT_EXT |
          VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADING_RATE_IMAGE_READ_BIT_NV |
          VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT | VK_ACCESS_COMMAND_PREPROCESS_READ_BIT_NV );
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
