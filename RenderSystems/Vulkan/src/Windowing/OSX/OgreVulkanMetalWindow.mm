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

#include "Windowing/OSX/OgreVulkanMetalWindow.h"

#include "OgreVulkanDevice.h"
#include "OgreVulkanTextureGpu.h"
#include "OgreVulkanTextureGpuManager.h"

#include "OgreDepthBuffer.h"
#include "OgreException.h"
#include "OgreLogManager.h"
#include "OgrePixelFormatGpuUtils.h"
#include "OgreString.h"
#include "OgreStringConverter.h"
#include "OgreTextureGpuListener.h"

#include "vulkan/vulkan_core.h"
#include "vulkan/vulkan_metal.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

/** The view we host our CAMetalLayer in.

    Ogre never replaces the layer of a view it was handed: a toolkit that manages
    its own layer refuses the replacement (Qt 6's QNSView overrides -setLayer: and
    -makeBackingLayer), and the surface would then be built on the wrong layer
    class. Instead this view is added as a subview and sized to the host's view,
    the same arrangement MetalWindow uses for foreign views.

    -hitTest: returns nil so the host keeps receiving every mouse event in the
    region: this view exists only to own a layer, never to handle input. */
@interface OgreVulkanMetalView : NSView
@end

@implementation OgreVulkanMetalView

+ (Class)layerClass
{
    return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if( self )
    {
        // setLayer BEFORE wantsLayer makes the view layer-HOSTING: AppKit then
        // leaves the layer's contentsScale alone and we drive it ourselves
        // (which is what makes the HiDPI story explicit rather than accidental).
        self.layer = [CAMetalLayer layer];
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    }
    return self;
}

- (BOOL)isOpaque
{
    return YES;
}

- (NSView *)hitTest:(NSPoint)point
{
    return nil;
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    // Moved between displays of different scale. A layer-hosted CAMetalLayer does
    // NOT get its contentsScale updated by AppKit, so do it here; MoltenVK derives
    // the surface's currentExtent from bounds x contentsScale, which makes the next
    // vkAcquireNextImageKHR report VK_ERROR_OUT_OF_DATE_KHR and the window rebuild
    // its swapchain at the new pixel size (VulkanMetalWindow::windowMovedOrResized).
    const CGFloat scale = self.window ? self.window.backingScaleFactor
                                      : [NSScreen mainScreen].backingScaleFactor;
    CAMetalLayer *layer = (CAMetalLayer *)self.layer;
    if( layer && scale > 0.0 )
    {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.contentsScale = scale;
        layer.drawableSize =
            CGSizeMake( self.bounds.size.width * scale, self.bounds.size.height * scale );
        [CATransaction commit];
    }
}

@end

namespace Ogre
{
    struct VulkanMetalWindow::Impl
    {
        /// The layer the swapchain presents to. Ours (hosted by mView) unless the
        /// caller passed a CAMetalLayer* of its own.
        CAMetalLayer *layer = nil;
        /// The view we created and own. nil when the caller passed a layer, or a
        /// view that is already CAMetalLayer-backed.
        OgreVulkanMetalView *ownedView = nil;
        /// The view the layer lives in (ours or the caller's). Weak: the host may
        /// tear its widget down before Ogre destroys the window.
        __weak NSView *view = nil;
    };
    //-------------------------------------------------------------------------
    VulkanMetalWindow::VulkanMetalWindow( const String &title, uint32 width, uint32 height,
                                          bool fullscreenMode ) :
        VulkanWindowSwapChainBased( title, width, height, fullscreenMode ),
        mImpl( new Impl() ),
        mVisible( true ),
        mHidden( false ),
        mIsExternalLayer( false )
    {
    }
    //-------------------------------------------------------------------------
    VulkanMetalWindow::~VulkanMetalWindow()
    {
        destroy();
        delete mImpl;
        mImpl = 0;
    }
    //-------------------------------------------------------------------------
    const char *VulkanMetalWindow::getRequiredExtensionName()
    {
        return VK_EXT_METAL_SURFACE_EXTENSION_NAME;
    }
    //-------------------------------------------------------------------------
    void VulkanMetalWindow::destroy()
    {
        VulkanWindowSwapChainBased::destroy();

        if( mClosed )
            return;

        mClosed = true;
        mFocused = false;

        // Detach OUR view from the host's hierarchy here, while both are known to
        // be alive; never in the destructor, where the host may already be gone.
        if( mImpl->ownedView )
            [mImpl->ownedView removeFromSuperview];
        mImpl->ownedView = nil;
        mImpl->view = nil;
        mImpl->layer = nil;
    }
    //-------------------------------------------------------------------------
    void VulkanMetalWindow::_initialize( TextureGpuManager *textureGpuManager,
                                         const NameValuePairList *miscParams )
    {
        destroy();

        mFocused = true;
        mClosed = false;
        mHidden = false;
        mHwGamma = false;

        size_t windowHandle = 0u;
        if( miscParams )
        {
            NameValuePairList::const_iterator end = miscParams->end();
            NameValuePairList::const_iterator opt = miscParams->find( "externalWindowHandle" );
            if( opt == end )
                opt = miscParams->find( "parentWindowHandle" );
            if( opt != end )
                windowHandle = StringConverter::parseSizeT( opt->second );

            parseSharedParams( miscParams );
        }

        @autoreleasepool
        {
            // Guard against handles that cannot be objects: a Qt platform plugin
            // that is not cocoa (offscreen, minimal) hands out small synthetic
            // window ids, and objc_msgSend on one of those is a segfault, not an
            // exception we could report. Anything below the first page is refused.
            id handle = windowHandle >= 0x10000u
                            ? (__bridge id)reinterpret_cast<void *>( windowHandle )
                            : nil;

            if( [handle isKindOfClass:[CAMetalLayer class]] )
            {
                // The host owns the layer, its size and its scale.
                mImpl->layer = (CAMetalLayer *)handle;
                mIsExternalLayer = true;
            }
            else
            {
                NSView *hostView = nil;
                if( [handle isKindOfClass:[NSWindow class]] )
                    hostView = ( (NSWindow *)handle ).contentView;
                else if( [handle isKindOfClass:[NSView class]] )
                    hostView = (NSView *)handle;

                if( hostView )
                {
                    if( [hostView.layer isKindOfClass:[CAMetalLayer class]] )
                    {
                        // Already a Metal view (e.g. Ogre's own OgreMetalView).
                        mImpl->view = hostView;
                        mImpl->layer = (CAMetalLayer *)hostView.layer;
                    }
                    else
                    {
                        NSRect frame = hostView.bounds;
                        if( frame.size.width <= 0.0 || frame.size.height <= 0.0 )
                            frame = NSMakeRect( 0.0, 0.0, mRequestedWidth, mRequestedHeight );
                        OgreVulkanMetalView *view =
                            [[OgreVulkanMetalView alloc] initWithFrame:frame];
                        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
                        [hostView addSubview:view];
                        mImpl->ownedView = view;
                        mImpl->view = view;
                        mImpl->layer = (CAMetalLayer *)view.layer;
                    }
                }
            }

            if( !mImpl->layer )
            {
                OGRE_EXCEPT( Exception::ERR_RENDERINGAPI_ERROR,
                             "externalWindowHandle does not resolve to a CAMetalLayer, an NSView "
                             "or an NSWindow",
                             "VulkanMetalWindow::_initialize" );
            }

            mImpl->layer.framebufferOnly = mCanDownloadData ? NO : YES;
        }

        VulkanTextureGpuManager *textureManager =
            static_cast<VulkanTextureGpuManager *>( textureGpuManager );

        mTexture = textureManager->createTextureGpuWindow( this );
        if( DepthBuffer::DefaultDepthBufferFormat != PFG_NULL )
        {
            const bool bMemoryLess = requestedMemoryless( miscParams );
            mDepthBuffer = textureManager->createWindowDepthBuffer( bMemoryLess );
        }
        mStencilBuffer = 0;

        uint32 widthPx = 0u, heightPx = 0u;
        syncLayerToView( widthPx, heightPx );
        setFinalResolution( widthPx, heightPx );

        createSurface();
        createSwapchain();
    }
    //-------------------------------------------------------------------------
    void VulkanMetalWindow::createSurface()
    {
        if( mDevice->isDeviceLost() )  // notifyDeviceRestored() will call us again
            return;

        PFN_vkCreateMetalSurfaceEXT createMetalSurface =
            (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr( mDevice->mInstance->mVkInstance,
                                                                "vkCreateMetalSurfaceEXT" );
        if( !createMetalSurface )
        {
            OGRE_EXCEPT( Exception::ERR_RENDERINGAPI_ERROR,
                         "vkCreateMetalSurfaceEXT not found. Was " VK_EXT_METAL_SURFACE_EXTENSION_NAME
                         " enabled on the VkInstance?",
                         "VulkanMetalWindow::createSurface" );
        }

        VkMetalSurfaceCreateInfoEXT metalSurfCreateInfo;
        memset( &metalSurfCreateInfo, 0, sizeof( metalSurfCreateInfo ) );
        metalSurfCreateInfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
        // pLayer is an Objective-C object pointer in this header (VK_USE_PLATFORM_METAL_EXT):
        // assign it directly, no bridging cast.
        metalSurfCreateInfo.pLayer = mImpl->layer;
        VkResult result = createMetalSurface( mDevice->mInstance->mVkInstance, &metalSurfCreateInfo, 0,
                                              &mSurfaceKHR );
        checkVkResult( mDevice, result, "vkCreateMetalSurfaceEXT" );
    }
    //-------------------------------------------------------------------------
    float VulkanMetalWindow::getViewPointToPixelScale() const
    {
        @autoreleasepool
        {
            if( mImpl->layer && mIsExternalLayer )
                return (float)mImpl->layer.contentsScale;
            NSView *view = mImpl->view;
            NSScreen *screen = view.window.screen ?: [NSScreen mainScreen];
            const CGFloat scale = screen ? screen.backingScaleFactor : 1.0;
            return scale > 0.0 ? (float)scale : 1.0f;
        }
    }
    //-------------------------------------------------------------------------
    void VulkanMetalWindow::syncLayerToView( uint32 &outWidthPx, uint32 &outHeightPx )
    {
        outWidthPx = outHeightPx = 0u;
        if( !mImpl->layer )
            return;

        @autoreleasepool
        {
            NSView *view = mImpl->view;
            if( mIsExternalLayer || !view )
            {
                // The host owns the geometry: read it, never write it.
                const CGSize sizePt = mImpl->layer.bounds.size;
                const CGFloat scale = mImpl->layer.contentsScale;
                outWidthPx = std::max( 1u, (uint32)floor( sizePt.width * scale + 0.5 ) );
                outHeightPx = std::max( 1u, (uint32)floor( sizePt.height * scale + 0.5 ) );
                return;
            }

            const CGFloat scale = (CGFloat)getViewPointToPixelScale();
            const NSRect bounds = view.bounds;
            outWidthPx = std::max( 1u, (uint32)floor( bounds.size.width * scale + 0.5 ) );
            outHeightPx = std::max( 1u, (uint32)floor( bounds.size.height * scale + 0.5 ) );

            // Implicit CALayer animations would lag a resize by a quarter second and
            // stretch the last frame while they run.
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            if( mImpl->ownedView )
            {
                NSView *parent = mImpl->ownedView.superview;
                if( parent && !NSEqualRects( mImpl->ownedView.frame, parent.bounds ) )
                    mImpl->ownedView.frame = parent.bounds;
                mImpl->layer.frame = mImpl->ownedView.bounds;
            }
            mImpl->layer.contentsScale = scale;
            // MoltenVK takes the surface's currentExtent from bounds x contentsScale,
            // but the drawables must agree with it or presentation scales the image.
            mImpl->layer.drawableSize = CGSizeMake( outWidthPx, outHeightPx );
            [CATransaction commit];
        }
    }
    //-------------------------------------------------------------------------
    void VulkanMetalWindow::reposition( int32 leftPt, int32 topPt )
    {
        // Our view is pinned to the host's view; the host repositions its own window.
        mLeft = leftPt;
        mTop = topPt;
    }
    //-------------------------------------------------------------------------
    void VulkanMetalWindow::requestResolution( uint32 widthPt, uint32 heightPt )
    {
        if( mClosed )
            return;

        Window::requestResolution( widthPt, heightPt );

        if( widthPt != 0u && heightPt != 0u )
            windowMovedOrResized();
    }
    //-------------------------------------------------------------------------
    void VulkanMetalWindow::windowMovedOrResized()
    {
        if( mClosed || !mImpl->layer )
            return;

        uint32 widthPx = 0u, heightPx = 0u;
        syncLayerToView( widthPx, heightPx );

        if( widthPx == getWidth() && heightPx == getHeight() && !mRebuildingSwapchain )
            return;

        mDevice->stallIgnoringDeviceLost();

        destroySwapchain();
        setFinalResolution( widthPx, heightPx );
        createSwapchain();  // takes the surface's currentExtent as the final word
    }
    //-------------------------------------------------------------------------
    void VulkanMetalWindow::setFsaa( const String &fsaa )
    {
        if( mClosed || !mImpl->layer )
            return;

        SampleDescription requested;
        requested.parseString( fsaa );
        if( requested.getColourSamples() == mRequestedSampleDescription.getColourSamples() )
            return;

        // Vulkan cannot change a swapchain's sample count in place: the MSAA
        // surfaces are (re)created by createSwapchain from mRequestedSampleDescription.
        mRequestedSampleDescription = requested;

        mDevice->stallIgnoringDeviceLost();

        destroySwapchain();
        createSwapchain();
    }
    //-------------------------------------------------------------------------
    void VulkanMetalWindow::_setVisible( bool visible ) { mVisible = visible; }
    //-------------------------------------------------------------------------
    bool VulkanMetalWindow::isVisible() const { return mVisible; }
    //-------------------------------------------------------------------------
    void VulkanMetalWindow::setHidden( bool hidden )
    {
        mHidden = hidden;
        // Showing/hiding is the host's business (it owns the window); all we can
        // do is stop occupying the region.
        if( mImpl->ownedView )
            mImpl->ownedView.hidden = hidden ? YES : NO;
    }
    //-------------------------------------------------------------------------
    bool VulkanMetalWindow::isHidden() const { return mHidden; }
    //-------------------------------------------------------------------------
    void VulkanMetalWindow::getCustomAttribute( IdString name, void *pData )
    {
        if( name == "CAMetalLayer" || name == "RENDERDOC_WINDOW" )
        {
            *static_cast<void **>( pData ) = (__bridge void *)mImpl->layer;
            return;
        }
        else if( name == "NSView" || name == "UIView" )
        {
            *static_cast<void **>( pData ) = (__bridge void *)mImpl->view;
            return;
        }
        else
        {
            VulkanWindowSwapChainBased::getCustomAttribute( name, pData );
        }
    }
}  // namespace Ogre
