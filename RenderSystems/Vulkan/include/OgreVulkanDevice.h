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

    struct VulkanDeviceCreationRequest;

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

        /// Jahshaka (ogre-patch 0068): OPTIONAL. When the caller built this device
        /// from VulkanDevice::buildDeviceCreationRequest() (the OpenXR route:
        /// xrCreateVulkanDeviceKHR is handed exactly the VkDeviceCreateInfo Ogre
        /// would have used), point this at that request and Ogre records the
        /// features that were actually ENABLED instead of re-querying what the
        /// physical device merely SUPPORTS. Without it the old behaviour stands:
        /// Ogre believes the hardware's capabilities and can compile shaders using
        /// features the handed-in device never enabled.
        const VulkanDeviceCreationRequest *creationRequest = 0;
    };

    /**
       We need the ability to re-enumerate devices to handle physical device removing, that
       requires fresh VkInstance instance, as otherwise Vulkan returns obsolete physical devices list.
    */
    class VulkanInstance final
    {
    public:
        static void enumerateExtensionsAndLayers( VulkanExternalInstance *externalInstance );
        static bool hasExtension( const char *extension );

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
        };

        /// Jahshaka (ogre-patch 0038): the feature structs a hardware ray-query tier
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
        FastArray<VkQueueFamilyProperties> mQueueProps;

        /// Extensions requested when created. Sorted
        FastArray<IdString> mDeviceExtensions;

        VulkanVaoManager *mVaoManager;
        VulkanRenderSystem *mRenderSystem;

        uint32 mSupportedStages;

        VkResult mDeviceLostReason;
        bool mIsExternal;

        void fillDeviceFeatures();
        bool fillDeviceFeatures2(
            VkPhysicalDeviceFeatures2 &deviceFeatures2,
            VkPhysicalDevice16BitStorageFeatures &device16BitStorageFeatures,
            VkPhysicalDeviceShaderFloat16Int8Features &deviceShaderFloat16Int8Features,
            VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT &deviceCacheControlFeatures );

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

        /// Jahshaka (ogre-patch 0068): the device-extension names createDevice()
        /// would request on this physical device, in its exact order. createDevice()
        /// calls this, so an external creator that calls it too cannot drift.
        static void fillDeviceExtensionRequest(
            VkPhysicalDevice physicalDevice,
            const FastArray<VkExtensionProperties> &availableExtensions,
            FastArray<const char *> &outExtensions );

        /// Jahshaka (ogre-patch 0068): the VkPhysicalDeviceFeatures createDevice()
        /// would enable (Ogre's opt-in subset of what the driver reports).
        /// fillDeviceFeatures() calls this.
        static void fillDeviceFeaturesFor( VkPhysicalDevice physicalDevice,
                                           VkPhysicalDeviceFeatures &outFeatures );

        /// Jahshaka (ogre-patch 0068): EVERYTHING vkCreateDevice would have been
        /// given by createDevice() - the extension list, the base features and the
        /// whole VkPhysicalDeviceFeatures2 pNext chain - filled into a caller-owned
        /// request, so an external device creator (OpenXR's xrCreateVulkanDeviceKHR)
        /// can create a device Ogre will believe.
        ///
        /// Call it AFTER the render system exists (the plugin's construction runs
        /// VulkanInstance::enumerateExtensionsAndLayers, which is what decides
        /// whether VK_KHR_get_physical_device_properties2 is usable) and BEFORE the
        /// first createRenderWindow. The request OWNS its chain and the chain points
        /// into it: never copy a filled request, pass it by pointer/reference.
        static void buildDeviceCreationRequest(
            VkInstance instance, VkPhysicalDevice physicalDevice,
            const FastArray<VkExtensionProperties> &availableExtensions,
            VulkanDeviceCreationRequest &outRequest );

        bool hasDeviceExtension( const IdString extension ) const;

        /// Jahshaka (ogre-patch 0038): true when VK_KHR_acceleration_structure +
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
        /// Jahshaka (ogre-patch 0068): the one implementation of the
        /// VkPhysicalDeviceFeatures2 pNext chain, shared by the plain path
        /// (fillDeviceFeatures2) and the exported request
        /// (buildDeviceCreationRequest) so the two can never disagree.
        static bool buildFeatureChain(
            VkInstance instance, VkPhysicalDevice physicalDevice,
            const FastArray<IdString> &sortedDeviceExtensions,
            VkPhysicalDeviceFeatures2 &deviceFeatures2,
            VkPhysicalDevice16BitStorageFeatures &device16BitStorageFeatures,
            VkPhysicalDeviceShaderFloat16Int8Features &deviceShaderFloat16Int8Features,
            VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT &deviceCacheControlFeatures,
            RayQueryVkFeatures &rayQueryFeatures, ExtraVkFeatures &outExtraFeatures );
    };

    /// Jahshaka (ogre-patch 0068): a caller-owned copy of everything
    /// VulkanDevice::createDevice() hands to vkCreateDevice. Fill it with
    /// VulkanDevice::buildDeviceCreationRequest(), point a VkDeviceCreateInfo at
    /// extensions.begin() / pNext() / features, create the device (yourself or via a
    /// third party such as OpenXR), then hand the SAME request back to Ogre through
    /// VulkanExternalDevice::creationRequest.
    ///
    /// The pNext chain points into this object, so it is non-copyable by declaration
    /// (below). Keep it alive for as long as Ogre holds the external device.
    struct _OgreVulkanExport VulkanDeviceCreationRequest
    {
        /// Static string literals, Ogre's order. Safe to hand to Vulkan directly.
        FastArray<const char *> extensions;
        /// The same names, sorted, in the form Ogre keeps them (VulkanDevice::mDeviceExtensions).
        FastArray<IdString> sortedExtensions;

        VkPhysicalDeviceFeatures features;

        VkPhysicalDeviceFeatures2 features2;
        VkPhysicalDevice16BitStorageFeatures storage16Bit;
        VkPhysicalDeviceShaderFloat16Int8Features shaderFloat16Int8;
        VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT cacheControl;
        VulkanDevice::RayQueryVkFeatures rayQuery;
        VulkanDevice::ExtraVkFeatures extraFeatures;
#ifdef VK_KHR_present_mode_fifo_latest_ready
        VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR fifoLatestReady;
#endif
        /// False when VK_KHR_get_physical_device_properties2 is unavailable: there is
        /// no chain at all and vkCreateDevice must take pEnabledFeatures = &features.
        bool hasFeatures2;

        VulkanDeviceCreationRequest();

        /// NOT COPYABLE, and the compiler says so rather than this comment: the pNext
        /// chain above points INTO this object, so a copy would hand vkCreateDevice a
        /// chain that walks the original - or a dead one.
        VulkanDeviceCreationRequest( const VulkanDeviceCreationRequest & ) = delete;
        VulkanDeviceCreationRequest &operator=( const VulkanDeviceCreationRequest & ) = delete;

        /// VkDeviceCreateInfo::pNext (null when !hasFeatures2).
        const void *pNext() const { return hasFeatures2 ? &features2 : 0; }
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
