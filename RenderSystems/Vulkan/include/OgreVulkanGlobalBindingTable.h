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
#ifndef _OgreVulkanGlobalBindingTable_H_
#define _OgreVulkanGlobalBindingTable_H_

#include "OgreVulkanPrerequisites.h"

#include "vulkan/vulkan_core.h"

namespace Ogre
{
#define NUM_BIND_CONST_BUFFERS 16u
#define NUM_BIND_TEX_BUFFERS 16u
// We don't use OGRE_MAX_TEXTURE_LAYERS. That's overkill and thus
// reserved for DescriptorSetTextures kind of textures
// Jahshaka (PHOTON-VOXEL-3, BUGS-1): THE EMULATION TABLE IS INDEXED BY ABSOLUTE SLOT
// NUMBER, so its size bounds the LAST slot a pass numbers, not the count of textures
// it binds. VulkanRootLayout::bind hands the descriptor write
// &textures[range.start] for (end - start) entries, and the Hlms number buffers and
// tex buffers in the same slot space ahead of the textures: a pass whose textures
// start at t11 (a derived Hlms with nine reserved buffer slots) and binds 54 textures
// needs a table of 65. The device's limit is not this array (an RTX 4080's
// maxPerStageDescriptorSampledImages is 1,048,576): 32 -> 64 when a four-cascade
// anisotropic voxel chain put 40 volumes in a PBS pass (past 32 the writes ran into
// `samplers` and the buffer tables - destroyed descriptors and a SIGSEGV in
// VulkanRootLayout::bind; the only guard was an OGRE_ASSERT_MEDIUM, compiled out),
// 64 -> 128 when that pass's derived-Hlms twin asked for 65/66. The dirty-slot
// counters below are uint8 (255 = clean), so 128 fits them. VulkanRootLayout::
// createVulkanHandles refuses a dynamic set whose range ends past the table.
#define NUM_BIND_TEXTURES 128u
#define NUM_BIND_SAMPLERS 128u
#define NUM_BIND_READONLY_BUFFERS 16u

    namespace BakedDescriptorSets
    {
        enum BakedDescriptorSets
        {
            ReadOnlyBuffers,
            TexBuffers,
            Textures,
            Samplers,
            UavBuffers,
            UavTextures,
            NumBakedDescriptorSets
        };
    }

    /// This table holds an emulation of D3D11/Metal style of resource binding
    /// @see    VulkanRootLayout::bind
    struct VulkanGlobalBindingTable
    {
        VkDescriptorBufferInfo paramsBuffer[NumShaderTypes + 1u];
        VkDescriptorBufferInfo constBuffers[NUM_BIND_CONST_BUFFERS];
        VkBufferView texBuffers[NUM_BIND_TEX_BUFFERS];

        VkDescriptorImageInfo textures[NUM_BIND_TEXTURES];
        VkDescriptorImageInfo samplers[NUM_BIND_SAMPLERS];

        VkDescriptorBufferInfo readOnlyBuffers[NUM_BIND_READONLY_BUFFERS];
#ifdef OGRE_VK_WORKAROUND_ADRENO_6xx_READONLY_IS_TBUFFER
        VkBufferView readOnlyBuffers2[NUM_BIND_READONLY_BUFFERS];
#endif

        VkWriteDescriptorSet *bakedDescriptorSets[BakedDescriptorSets::NumBakedDescriptorSets];

        bool dirtyParamsBuffer;
        uint8 minDirtySlotConst;
        uint8 minDirtySlotTexBuffer;
        uint8 minDirtySlotTextures;
        uint8 minDirtySlotSamplers;
        uint8 minDirtySlotReadOnlyBuffer;
        bool dirtyBakedTextures;
        bool dirtyBakedSamplers;
        bool dirtyBakedUavs;

        void reset()
        {
            dirtyParamsBuffer = false;
            minDirtySlotConst = 255u;
            minDirtySlotTexBuffer = 255u;
            minDirtySlotTextures = 255u;
            minDirtySlotSamplers = 255u;
            minDirtySlotReadOnlyBuffer = 255u;

            dirtyBakedTextures = false;
            dirtyBakedSamplers = false;
            dirtyBakedUavs = false;
        }

        void setAllDirty()
        {
            dirtyParamsBuffer = true;
            minDirtySlotConst = 0u;
            minDirtySlotTexBuffer = 0u;
            minDirtySlotTextures = 0u;
            minDirtySlotSamplers = 0u;
            minDirtySlotReadOnlyBuffer = 0u;

            dirtyBakedTextures = true;
            dirtyBakedSamplers = true;
            dirtyBakedUavs = true;
        }
    };
}  // namespace Ogre

#endif
