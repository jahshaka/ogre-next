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

#ifndef _OgreVulkanMetalWindow_H_
#define _OgreVulkanMetalWindow_H_

#include "OgreVulkanPrerequisites.h"

#include "OgreVulkanWindow.h"

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    /** Vulkan window on Apple platforms: the VkSurfaceKHR comes from a
        CAMetalLayer through VK_EXT_metal_surface (MoltenVK).

        Accepted "externalWindowHandle"/"parentWindowHandle" values mirror what
        the Metal RenderSystem accepts (see OgreMetalWindow::create):
          - a CAMetalLayer*  : used as-is, the host owns it and its sizing;
          - an NSWindow*     : its contentView is used as the parent view;
          - an NSView*       : the parent view.

        For a parent view the window creates and hosts its OWN CAMetalLayer-backed
        child NSView, exactly like MetalWindow does for foreign views. Replacing
        a foreign view's layer is not an option in general: toolkits that manage
        their own layer (Qt's QNSView overrides -setLayer:/-makeBackingLayer)
        silently refuse the replacement, and the surface would then be created
        from a non-Metal layer.

        HiDPI: the hosted layer's contentsScale follows the screen's
        backingScaleFactor and getViewPointToPixelScale() reports the same value,
        so callers keep passing view POINTS (as the rest of Ogre's window API
        expects) while the swapchain is sized in PIXELS.

        Resize: requestResolution() re-syncs the layer with the view and calls
        windowMovedOrResized(), which rebuilds the swapchain — the surface's
        currentExtent is authoritative (see VulkanWindowSwapChainBased::createSwapchain).
        No window recreation is needed.
    */
    class _OgreVulkanExport VulkanMetalWindow : public VulkanWindowSwapChainBased
    {
        /// Objective-C state (the layer, our hosted view, the host's view). Lives
        /// in the .mm so this header stays plain C++ and can be included from
        /// OgreVulkanRenderSystem.cpp.
        struct Impl;
        Impl *mImpl;

        bool mVisible;
        bool mHidden;
        /// True when the host handed us a CAMetalLayer directly: it owns the
        /// layer's geometry and scale, we only build a swapchain on it.
        bool mIsExternalLayer;

        void createSurface() override;

        /// Re-syncs our hosted layer with the view it lives in (frame,
        /// contentsScale, drawableSize) and returns its size in PIXELS.
        void syncLayerToView( uint32 &outWidthPx, uint32 &outHeightPx );

    public:
        VulkanMetalWindow( const String &title, uint32 width, uint32 height, bool fullscreenMode );
        ~VulkanMetalWindow() override;

        static const char *getRequiredExtensionName();

        void destroy() override;
        void _initialize( TextureGpuManager *textureGpuManager,
                          const NameValuePairList *miscParams ) override;

        float getViewPointToPixelScale() const override;

        void reposition( int32 leftPt, int32 topPt ) override;
        void requestResolution( uint32 widthPt, uint32 heightPt ) override;
        void windowMovedOrResized() override;
        void setFsaa( const String &fsaa ) override;

        void _setVisible( bool visible ) override;
        bool isVisible() const override;
        void setHidden( bool hidden ) override;
        bool isHidden() const override;

        void getCustomAttribute( IdString name, void *pData ) override;
    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
