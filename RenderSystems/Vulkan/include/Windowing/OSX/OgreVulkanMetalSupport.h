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

#ifndef _OgreVulkanMetalSupport_H_
#define _OgreVulkanMetalSupport_H_

#define DEFINING_VK_SUPPORT_IMPL
#include "OgreVulkanSupport.h"

namespace Ogre
{
    /** The "metal" interface: VK_EXT_metal_surface windows (VulkanMetalWindow).

        Video modes are not enumerated (unlike xcb's randr query): Ogre never
        changes the display mode on macOS — fullscreen goes through the window
        server — so the base config (Device/Interface/FSAA/sRGB) is all there is.
    */
    class _OgreVulkanExport VulkanMetalSupport final : public VulkanSupport
    {
    public:
        IdString getInterfaceName() const override { return "metal"; }
        String getInterfaceNameStr() const override { return "metal"; }
    };
}  // namespace Ogre

#endif
