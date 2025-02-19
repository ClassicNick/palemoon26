/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * nsWindowGfx - Painting and aceleration.
 */

// XXX Future: this should really be a stand alone class stored as
// a member of nsWindow with getters and setters for things like render
// mode and methods for handling paint.

/**************************************************************
 **************************************************************
 **
 ** BLOCK: Includes
 **
 ** Include headers.
 **
 **************************************************************
 **************************************************************/

#include "mozilla/plugins/PluginInstanceParent.h"
using mozilla::plugins::PluginInstanceParent;

#include "nsWindowGfx.h"
#include <windows.h>
#include "gfxImageSurface.h"
#include "gfxWindowsSurface.h"
#include "gfxWindowsPlatform.h"
#include "nsGfxCIID.h"
#include "gfxContext.h"
#include "nsRenderingContext.h"
#include "prmem.h"
#include "WinUtils.h"
#include "nsIWidgetListener.h"
#include "mozilla/unused.h"

#include "LayerManagerOGL.h"
#include "BasicLayers.h"
#ifdef MOZ_ENABLE_D3D9_LAYER
#include "LayerManagerD3D9.h"
#endif
#ifdef MOZ_ENABLE_D3D10_LAYER
#include "LayerManagerD3D10.h"
#endif

#include "nsUXThemeData.h"
#include "nsUXThemeConstants.h"

extern "C" {
#define PIXMAN_DONT_DEFINE_STDINT
#include "pixman.h"
}

using namespace mozilla;
using namespace mozilla::layers;
using namespace mozilla::widget;

/**************************************************************
 **************************************************************
 **
 ** BLOCK: Variables
 **
 ** nsWindow Class static initializations and global variables.
 **
 **************************************************************
 **************************************************************/

/**************************************************************
 *
 * SECTION: nsWindow statics
 *
 **************************************************************/

static nsAutoPtr<uint8_t>  sSharedSurfaceData;
static gfxIntSize          sSharedSurfaceSize;

struct IconMetrics {
  int32_t xMetric;
  int32_t yMetric;
  int32_t defaultSize;
};

// Corresponds 1:1 to the IconSizeType enum
static IconMetrics sIconMetrics[] = {
  {SM_CXSMICON, SM_CYSMICON, 16}, // small icon
  {SM_CXICON,   SM_CYICON,   32}  // regular icon
};

/**************************************************************
 **************************************************************
 **
 ** BLOCK: nsWindowGfx impl.
 **
 ** Misc. graphics related utilities.
 **
 **************************************************************
 **************************************************************/

static bool
IsRenderMode(gfxWindowsPlatform::RenderMode rmode)
{
  return gfxWindowsPlatform::GetPlatform()->GetRenderMode() == rmode;
}

/**************************************************************
 **************************************************************
 **
 ** BLOCK: nsWindow impl.
 **
 ** Paint related nsWindow methods.
 **
 **************************************************************
 **************************************************************/

// GetRegionToPaint returns the invalidated region that needs to be painted
nsIntRegion nsWindow::GetRegionToPaint(bool aForceFullRepaint,
                                       PAINTSTRUCT ps, HDC aDC)
{
  if (aForceFullRepaint) {
    RECT paintRect;
    ::GetClientRect(mWnd, &paintRect);
    return nsIntRegion(WinUtils::ToIntRect(paintRect));
  }

  HRGN paintRgn = ::CreateRectRgn(0, 0, 0, 0);
  if (paintRgn != NULL) {
    int result = GetRandomRgn(aDC, paintRgn, SYSRGN);
    if (result == 1) {
      POINT pt = {0,0};
      ::MapWindowPoints(NULL, mWnd, &pt, 1);
      ::OffsetRgn(paintRgn, pt.x, pt.y);
    }
    nsIntRegion rgn(WinUtils::ConvertHRGNToRegion(paintRgn));
    ::DeleteObject(paintRgn);
    return rgn;
  }
  return nsIntRegion(WinUtils::ToIntRect(ps.rcPaint));
}

#define WORDSSIZE(x) ((x).width * (x).height)
static bool
EnsureSharedSurfaceSize(gfxIntSize size)
{
  gfxIntSize screenSize;
  screenSize.height = GetSystemMetrics(SM_CYSCREEN);
  screenSize.width = GetSystemMetrics(SM_CXSCREEN);

  if (WORDSSIZE(screenSize) > WORDSSIZE(size))
    size = screenSize;

  if (WORDSSIZE(screenSize) < WORDSSIZE(size))
    NS_WARNING("Trying to create a shared surface larger than the screen");

  if (!sSharedSurfaceData || (WORDSSIZE(size) > WORDSSIZE(sSharedSurfaceSize))) {
    sSharedSurfaceSize = size;
    sSharedSurfaceData = nullptr;
    sSharedSurfaceData = (uint8_t *)malloc(WORDSSIZE(sSharedSurfaceSize) * 4);
  }

  return (sSharedSurfaceData != nullptr);
}

nsIWidgetListener* nsWindow::GetPaintListener()
{
  if (mDestroyCalled)
    return nullptr;
  return mAttachedWidgetListener ? mAttachedWidgetListener : mWidgetListener;
}

bool nsWindow::OnPaint(HDC aDC, uint32_t aNestingLevel)
{
  // We never have reentrant paint events, except when we're running our RPC
  // windows event spin loop. If we don't trap for this, we'll try to paint,
  // but view manager will refuse to paint the surface, resulting is black
  // flashes on the plugin rendering surface.
  if (mozilla::ipc::RPCChannel::IsSpinLoopActive() && mPainting)
    return false;

  if (mWindowType == eWindowType_plugin) {

    /**
     * After we CallUpdateWindow to the child, occasionally a WM_PAINT message
     * is posted to the parent event loop with an empty update rect. Do a
     * dummy paint so that Windows stops dispatching WM_PAINT in an inifinite
     * loop. See bug 543788.
     */
    RECT updateRect;
    if (!GetUpdateRect(mWnd, &updateRect, FALSE) ||
        (updateRect.left == updateRect.right &&
         updateRect.top == updateRect.bottom)) {
      PAINTSTRUCT ps;
      BeginPaint(mWnd, &ps);
      EndPaint(mWnd, &ps);
      return true;
    }

    PluginInstanceParent* instance = reinterpret_cast<PluginInstanceParent*>(
      ::GetPropW(mWnd, L"PluginInstanceParentProperty"));
    if (instance) {
      unused << instance->CallUpdateWindow();
    } else {
      // We should never get here since in-process plugins should have
      // subclassed our HWND and handled WM_PAINT, but in some cases that
      // could fail. Return without asserting since it's not our fault.
      NS_WARNING("Plugin failed to subclass our window");
    }

    ValidateRect(mWnd, NULL);
    return true;
  }

  nsIWidgetListener* listener = GetPaintListener();
  if (listener) {
    listener->WillPaintWindow(this);
  }
  // Re-get the listener since the will paint notification may have killed it.
  listener = GetPaintListener();
  if (!listener)
    return false;

  bool result = true;
  PAINTSTRUCT ps;

#ifdef MOZ_XUL
  if (!aDC && (eTransparencyTransparent == mTransparencyMode))
  {
    // For layered translucent windows all drawing should go to memory DC and no
    // WM_PAINT messages are normally generated. To support asynchronous painting
    // we force generation of WM_PAINT messages by invalidating window areas with
    // RedrawWindow, InvalidateRect or InvalidateRgn function calls.
    // BeginPaint/EndPaint must be called to make Windows think that invalid area
    // is painted. Otherwise it will continue sending the same message endlessly.
    ::BeginPaint(mWnd, &ps);
    ::EndPaint(mWnd, &ps);

    aDC = mMemoryDC;
  }
#endif

  mPainting = true;

#ifdef WIDGET_DEBUG_OUTPUT
  HRGN debugPaintFlashRegion = NULL;
  HDC debugPaintFlashDC = NULL;

  if (debug_WantPaintFlashing())
  {
    debugPaintFlashRegion = ::CreateRectRgn(0, 0, 0, 0);
    ::GetUpdateRgn(mWnd, debugPaintFlashRegion, TRUE);
    debugPaintFlashDC = ::GetDC(mWnd);
  }
#endif // WIDGET_DEBUG_OUTPUT

  HDC hDC = aDC ? aDC : (::BeginPaint(mWnd, &ps));
  mPaintDC = hDC;

#ifdef MOZ_XUL
  bool forceRepaint = aDC || (eTransparencyTransparent == mTransparencyMode);
#else
  bool forceRepaint = NULL != aDC;
#endif
  nsIntRegion region = GetRegionToPaint(forceRepaint, ps, hDC);
  if (!region.IsEmpty() && listener)
  {
    // Should probably pass in a real region here, using GetRandomRgn
    // http://msdn.microsoft.com/library/default.asp?url=/library/en-us/gdi/clipping_4q0e.asp

#ifdef WIDGET_DEBUG_OUTPUT
    debug_DumpPaintEvent(stdout,
                         this,
                         region,
                         nsAutoCString("noname"),
                         (int32_t) mWnd);
#endif // WIDGET_DEBUG_OUTPUT

    switch (GetLayerManager()->GetBackendType()) {
      case LAYERS_BASIC:
        {
          nsRefPtr<gfxASurface> targetSurface;

#if defined(MOZ_XUL)
          // don't support transparency for non-GDI rendering, for now
          if ((IsRenderMode(gfxWindowsPlatform::RENDER_GDI) ||
               IsRenderMode(gfxWindowsPlatform::RENDER_DIRECT2D)) &&
              eTransparencyTransparent == mTransparencyMode) {
            if (mTransparentSurface == nullptr)
              SetupTranslucentWindowMemoryBitmap(mTransparencyMode);
            targetSurface = mTransparentSurface;
          }
#endif

#ifdef CAIRO_HAS_D2D_SURFACE
          if (!targetSurface &&
              IsRenderMode(gfxWindowsPlatform::RENDER_DIRECT2D))
          {
            if (!mD2DWindowSurface) {
              gfxASurface::gfxContentType content = gfxASurface::CONTENT_COLOR;
#if defined(MOZ_XUL)
              if (mTransparencyMode != eTransparencyOpaque) {
                content = gfxASurface::CONTENT_COLOR_ALPHA;
              }
#endif
              mD2DWindowSurface = new gfxD2DSurface(mWnd, content);
            }
            if (!mD2DWindowSurface->CairoStatus()) {
              targetSurface = mD2DWindowSurface;
            } else {
              mD2DWindowSurface = nullptr;
            }
          }
#endif

          nsRefPtr<gfxWindowsSurface> targetSurfaceWin;
          if (!targetSurface &&
              (IsRenderMode(gfxWindowsPlatform::RENDER_GDI) ||
               IsRenderMode(gfxWindowsPlatform::RENDER_DIRECT2D)))
          {
            uint32_t flags = (mTransparencyMode == eTransparencyOpaque) ? 0 :
                gfxWindowsSurface::FLAG_IS_TRANSPARENT;
            targetSurfaceWin = new gfxWindowsSurface(hDC, flags);
            targetSurface = targetSurfaceWin;
          }

          nsRefPtr<gfxImageSurface> targetSurfaceImage;
          if (!targetSurface &&
              (IsRenderMode(gfxWindowsPlatform::RENDER_IMAGE_STRETCH32) ||
               IsRenderMode(gfxWindowsPlatform::RENDER_IMAGE_STRETCH24)))
          {
            gfxIntSize surfaceSize(ps.rcPaint.right - ps.rcPaint.left,
                                   ps.rcPaint.bottom - ps.rcPaint.top);

            if (!EnsureSharedSurfaceSize(surfaceSize)) {
              NS_ERROR("Couldn't allocate a shared image surface!");
              return false;
            }

            // don't use the shared surface directly; instead, create a new one
            // that just reuses its buffer.
            targetSurfaceImage = new gfxImageSurface(sSharedSurfaceData.get(),
                                                     surfaceSize,
                                                     surfaceSize.width * 4,
                                                     gfxASurface::ImageFormatRGB24);

            if (targetSurfaceImage && !targetSurfaceImage->CairoStatus()) {
              targetSurfaceImage->SetDeviceOffset(gfxPoint(-ps.rcPaint.left, -ps.rcPaint.top));
              targetSurface = targetSurfaceImage;
            }
          }

          if (!targetSurface) {
            NS_ERROR("Invalid RenderMode!");
            return false;
          }

          nsRefPtr<gfxContext> thebesContext = new gfxContext(targetSurface);
          
          // don't need to double buffer with anything but GDI
          BufferMode doubleBuffering = mozilla::layers::BUFFER_NONE;
          if (IsRenderMode(gfxWindowsPlatform::RENDER_GDI) ||
              IsRenderMode(gfxWindowsPlatform::RENDER_DIRECT2D)) {
#ifdef MOZ_XUL
            switch (mTransparencyMode) {
              case eTransparencyGlass:
              case eTransparencyBorderlessGlass:
              default:
                // If we're not doing translucency, then double buffer
                doubleBuffering = mozilla::layers::BUFFER_BUFFERED;
                break;
              case eTransparencyTransparent:
                // If we're rendering with translucency, we're going to be
                // rendering the whole window; make sure we clear it first
                thebesContext->SetOperator(gfxContext::OPERATOR_CLEAR);
                thebesContext->Paint();
                thebesContext->SetOperator(gfxContext::OPERATOR_OVER);
                break;
            }
#else
            doubleBuffering = mozilla::layers::BUFFER_BUFFERED;
#endif
          }

          {
            AutoLayerManagerSetup
                setupLayerManager(this, thebesContext, doubleBuffering);
            result = listener->PaintWindow(this, region);
          }

#ifdef MOZ_XUL
          if ((IsRenderMode(gfxWindowsPlatform::RENDER_GDI) ||
               IsRenderMode(gfxWindowsPlatform::RENDER_DIRECT2D))&&
              eTransparencyTransparent == mTransparencyMode) {
            // Data from offscreen drawing surface was copied to memory bitmap of transparent
            // bitmap. Now it can be read from memory bitmap to apply alpha channel and after
            // that displayed on the screen.
            UpdateTranslucentWindow();
          } else
#endif
#ifdef CAIRO_HAS_D2D_SURFACE
          if (result) {
            if (mD2DWindowSurface) {
              mD2DWindowSurface->Present();
            }
          }
#endif
          if (result) {
            if (IsRenderMode(gfxWindowsPlatform::RENDER_IMAGE_STRETCH24) ||
                IsRenderMode(gfxWindowsPlatform::RENDER_IMAGE_STRETCH32))
            {
              gfxIntSize surfaceSize = targetSurfaceImage->GetSize();

              // Just blit this directly
              BITMAPINFOHEADER bi;
              memset(&bi, 0, sizeof(BITMAPINFOHEADER));
              bi.biSize = sizeof(BITMAPINFOHEADER);
              bi.biWidth = surfaceSize.width;
              bi.biHeight = - surfaceSize.height;
              bi.biPlanes = 1;
              bi.biBitCount = 32;
              bi.biCompression = BI_RGB;

              if (IsRenderMode(gfxWindowsPlatform::RENDER_IMAGE_STRETCH24)) {
                // On Windows CE/Windows Mobile, 24bpp packed-pixel sources
                // seem to be far faster to blit than 32bpp (see bug 484864).
                // So, convert the bits to 24bpp by stripping out the unused
                // alpha byte.  24bpp DIBs also have scanlines that are 4-byte
                // aligned though, so that must be taken into account.
                int srcstride = surfaceSize.width*4;
                int dststride = surfaceSize.width*3;
                dststride = (dststride + 3) & ~3;

                // Convert in place
                for (int j = 0; j < surfaceSize.height; ++j) {
                  unsigned int *src = (unsigned int*) (targetSurfaceImage->Data() + j*srcstride);
                  unsigned int *dst = (unsigned int*) (targetSurfaceImage->Data() + j*dststride);

                  // go 4 pixels at a time, since each 4 pixels
                  // turns into 3 DWORDs when converted into BGR:
                  // BGRx BGRx BGRx BGRx -> BGRB GRBG RBGR
                  //
                  // However, since we're dealing with little-endian ints, this is actually:
                  // xRGB xrgb xRGB xrgb -> bRGB GBrg rgbR
                  int width_left = surfaceSize.width;
                  while (width_left >= 4) {
                    unsigned int a = *src++;
                    unsigned int b = *src++;
                    unsigned int c = *src++;
                    unsigned int d = *src++;

                    *dst++ =  (a & 0x00ffffff)        | (b << 24);
                    *dst++ = ((b & 0x00ffff00) >> 8)  | (c << 16);
                    *dst++ = ((c & 0x00ff0000) >> 16) | (d << 8);

                    width_left -= 4;
                  }

                  // then finish up whatever number of pixels are left,
                  // using bytes.
                  unsigned char *bsrc = (unsigned char*) src;
                  unsigned char *bdst = (unsigned char*) dst;
                  switch (width_left) {
                    case 3:
                      *bdst++ = *bsrc++;
                      *bdst++ = *bsrc++;
                      *bdst++ = *bsrc++;
                      bsrc++;
                    case 2:
                      *bdst++ = *bsrc++;
                      *bdst++ = *bsrc++;
                      *bdst++ = *bsrc++;
                      bsrc++;
                    case 1:
                      *bdst++ = *bsrc++;
                      *bdst++ = *bsrc++;
                      *bdst++ = *bsrc++;
                      bsrc++;
                    case 0:
                      break;
                  }
                }

                bi.biBitCount = 24;
              }

              StretchDIBits(hDC,
                            ps.rcPaint.left, ps.rcPaint.top,
                            surfaceSize.width, surfaceSize.height,
                            0, 0,
                            surfaceSize.width, surfaceSize.height,
                            targetSurfaceImage->Data(),
                            (BITMAPINFO*) &bi,
                            DIB_RGB_COLORS,
                            SRCCOPY);
            }
          }
        }
        break;
      case LAYERS_OPENGL:
        static_cast<mozilla::layers::LayerManagerOGL*>(GetLayerManager())->
          SetClippingRegion(region);
        result = listener->PaintWindow(this, region);
        break;
#ifdef MOZ_ENABLE_D3D9_LAYER
      case LAYERS_D3D9:
        {
          nsRefPtr<LayerManagerD3D9> layerManagerD3D9 =
            static_cast<mozilla::layers::LayerManagerD3D9*>(GetLayerManager());
          layerManagerD3D9->SetClippingRegion(region);
          result = listener->PaintWindow(this, region);
          if (layerManagerD3D9->DeviceWasRemoved()) {
            mLayerManager->Destroy();
            mLayerManager = nullptr;
            // When our device was removed, we should have gfxWindowsPlatform
            // check if its render mode is up to date!
            gfxWindowsPlatform::GetPlatform()->UpdateRenderMode();
            Invalidate();
          }
        }
        break;
#endif
#ifdef MOZ_ENABLE_D3D10_LAYER
      case LAYERS_D3D10:
        {
          gfxWindowsPlatform::GetPlatform()->UpdateRenderMode();
          LayerManagerD3D10 *layerManagerD3D10 = static_cast<mozilla::layers::LayerManagerD3D10*>(GetLayerManager());
          if (layerManagerD3D10->device() != gfxWindowsPlatform::GetPlatform()->GetD3D10Device()) {
            Invalidate();
          } else {
            result = listener->PaintWindow(this, region);
          }
        }
        break;
#endif
      case LAYERS_CLIENT:
        // Do nothing, the compositor will handle drawing.
        break;

      default:
        NS_ERROR("Unknown layers backend used!");
        break;
    }
  }

  if (!aDC) {
    ::EndPaint(mWnd, &ps);
  }

  mPaintDC = nullptr;
  mLastPaintEndTime = TimeStamp::Now();

#if defined(WIDGET_DEBUG_OUTPUT)
  if (debug_WantPaintFlashing())
  {
    // Only flash paint events which have not ignored the paint message.
    // Those that ignore the paint message aren't painting anything so there
    // is only the overhead of the dispatching the paint event.
    if (result) {
      ::InvertRgn(debugPaintFlashDC, debugPaintFlashRegion);
      PR_Sleep(PR_MillisecondsToInterval(30));
      ::InvertRgn(debugPaintFlashDC, debugPaintFlashRegion);
      PR_Sleep(PR_MillisecondsToInterval(30));
    }
    ::ReleaseDC(mWnd, debugPaintFlashDC);
    ::DeleteObject(debugPaintFlashRegion);
  }
#endif // WIDGET_DEBUG_OUTPUT

  mPainting = false;

  // Re-get the listener since painting may have killed it.
  listener = GetPaintListener();
  if (listener)
    listener->DidPaintWindow();

  if (aNestingLevel == 0 && ::GetUpdateRect(mWnd, NULL, false)) {
    OnPaint(aDC, 1);
  }

  return result;
}

gfxIntSize nsWindowGfx::GetIconMetrics(IconSizeType aSizeType) {
  int32_t width = ::GetSystemMetrics(sIconMetrics[aSizeType].xMetric);
  int32_t height = ::GetSystemMetrics(sIconMetrics[aSizeType].yMetric);

  if (width == 0 || height == 0) {
    width = height = sIconMetrics[aSizeType].defaultSize;
  }

  return gfxIntSize(width, height);
}

nsresult nsWindowGfx::CreateIcon(imgIContainer *aContainer,
                                  bool aIsCursor,
                                  uint32_t aHotspotX,
                                  uint32_t aHotspotY,
                                  gfxIntSize aScaledSize,
                                  HICON *aIcon) {

  // Get the image data
  nsRefPtr<gfxASurface> surface;
  aContainer->GetFrame(imgIContainer::FRAME_CURRENT,
                       imgIContainer::FLAG_SYNC_DECODE,
                       getter_AddRefs(surface));
  NS_ENSURE_TRUE(surface, NS_ERROR_NOT_AVAILABLE);

  nsRefPtr<gfxImageSurface> frame(surface->GetAsReadableARGB32ImageSurface());
  NS_ENSURE_TRUE(frame, NS_ERROR_NOT_AVAILABLE);

  int32_t width = frame->Width();
  int32_t height = frame->Height();
  if (!width || !height)
    return NS_ERROR_FAILURE;

  uint8_t *data;
  nsRefPtr<gfxImageSurface> dest;

  if ((aScaledSize.width == 0 && aScaledSize.height == 0) ||
      (aScaledSize.width == width && aScaledSize.height == height)) {
    // We're not scaling the image. The data is simply what's in the frame.
    data = frame->Data();
  }
  else {
    NS_ENSURE_ARG(aScaledSize.width > 0);
    NS_ENSURE_ARG(aScaledSize.height > 0);
    // Draw a scaled version of the image to a temporary surface
    dest = new gfxImageSurface(aScaledSize, gfxASurface::ImageFormatARGB32);
    if (!dest)
      return NS_ERROR_OUT_OF_MEMORY;

    gfxContext ctx(dest);

    // Set scaling
    gfxFloat sw = (double) aScaledSize.width / width;
    gfxFloat sh = (double) aScaledSize.height / height;
    ctx.Scale(sw, sh);

    // Paint a scaled image
    ctx.SetOperator(gfxContext::OPERATOR_SOURCE);
    ctx.SetSource(frame);
    ctx.Paint();

    data = dest->Data();
    width = aScaledSize.width;
    height = aScaledSize.height;
  }

  HBITMAP bmp = DataToBitmap(data, width, -height, 32);
  uint8_t* a1data = Data32BitTo1Bit(data, width, height);
  if (!a1data) {
    return NS_ERROR_FAILURE;
  }

  HBITMAP mbmp = DataToBitmap(a1data, width, -height, 1);
  PR_Free(a1data);

  ICONINFO info = {0};
  info.fIcon = !aIsCursor;
  info.xHotspot = aHotspotX;
  info.yHotspot = aHotspotY;
  info.hbmMask = mbmp;
  info.hbmColor = bmp;

  HCURSOR icon = ::CreateIconIndirect(&info);
  ::DeleteObject(mbmp);
  ::DeleteObject(bmp);
  if (!icon)
    return NS_ERROR_FAILURE;
  *aIcon = icon;
  return NS_OK;
}

// Adjust cursor image data
uint8_t* nsWindowGfx::Data32BitTo1Bit(uint8_t* aImageData,
                                      uint32_t aWidth, uint32_t aHeight)
{
  // We need (aWidth + 7) / 8 bytes plus zero-padding up to a multiple of
  // 4 bytes for each row (HBITMAP requirement). Bug 353553.
  uint32_t outBpr = ((aWidth + 31) / 8) & ~3;

  // Allocate and clear mask buffer
  uint8_t* outData = (uint8_t*)PR_Calloc(outBpr, aHeight);
  if (!outData)
    return NULL;

  int32_t *imageRow = (int32_t*)aImageData;
  for (uint32_t curRow = 0; curRow < aHeight; curRow++) {
    uint8_t *outRow = outData + curRow * outBpr;
    uint8_t mask = 0x80;
    for (uint32_t curCol = 0; curCol < aWidth; curCol++) {
      // Use sign bit to test for transparency, as alpha byte is highest byte
      if (*imageRow++ < 0)
        *outRow |= mask;

      mask >>= 1;
      if (!mask) {
        outRow ++;
        mask = 0x80;
      }
    }
  }

  return outData;
}

bool nsWindowGfx::IsCursorTranslucencySupported()
{
  static bool didCheck = false;
  static bool isSupported = false;
  if (!didCheck) {
    didCheck = true;
    // Cursor translucency is supported on Windows XP and newer
    isSupported = WinUtils::GetWindowsVersion() >= WinUtils::WINXP_VERSION;
  }

  return isSupported;
}

/**
 * Convert the given image data to a HBITMAP. If the requested depth is
 * 32 bit and the OS supports translucency, a bitmap with an alpha channel
 * will be returned.
 *
 * @param aImageData The image data to convert. Must use the format accepted
 *                   by CreateDIBitmap.
 * @param aWidth     With of the bitmap, in pixels.
 * @param aHeight    Height of the image, in pixels.
 * @param aDepth     Image depth, in bits. Should be one of 1, 24 and 32.
 *
 * @return The HBITMAP representing the image. Caller should call
 *         DeleteObject when done with the bitmap.
 *         On failure, NULL will be returned.
 */
HBITMAP nsWindowGfx::DataToBitmap(uint8_t* aImageData,
                                  uint32_t aWidth,
                                  uint32_t aHeight,
                                  uint32_t aDepth)
{
  HDC dc = ::GetDC(NULL);

  if (aDepth == 32 && IsCursorTranslucencySupported()) {
    // Alpha channel. We need the new header.
    BITMAPV4HEADER head = { 0 };
    head.bV4Size = sizeof(head);
    head.bV4Width = aWidth;
    head.bV4Height = aHeight;
    head.bV4Planes = 1;
    head.bV4BitCount = aDepth;
    head.bV4V4Compression = BI_BITFIELDS;
    head.bV4SizeImage = 0; // Uncompressed
    head.bV4XPelsPerMeter = 0;
    head.bV4YPelsPerMeter = 0;
    head.bV4ClrUsed = 0;
    head.bV4ClrImportant = 0;

    head.bV4RedMask   = 0x00FF0000;
    head.bV4GreenMask = 0x0000FF00;
    head.bV4BlueMask  = 0x000000FF;
    head.bV4AlphaMask = 0xFF000000;

    HBITMAP bmp = ::CreateDIBitmap(dc,
                                   reinterpret_cast<CONST BITMAPINFOHEADER*>(&head),
                                   CBM_INIT,
                                   aImageData,
                                   reinterpret_cast<CONST BITMAPINFO*>(&head),
                                   DIB_RGB_COLORS);
    ::ReleaseDC(NULL, dc);
    return bmp;
  }

  char reserved_space[sizeof(BITMAPINFOHEADER) + sizeof(RGBQUAD) * 2];
  BITMAPINFOHEADER& head = *(BITMAPINFOHEADER*)reserved_space;

  head.biSize = sizeof(BITMAPINFOHEADER);
  head.biWidth = aWidth;
  head.biHeight = aHeight;
  head.biPlanes = 1;
  head.biBitCount = (WORD)aDepth;
  head.biCompression = BI_RGB;
  head.biSizeImage = 0; // Uncompressed
  head.biXPelsPerMeter = 0;
  head.biYPelsPerMeter = 0;
  head.biClrUsed = 0;
  head.biClrImportant = 0;
  
  BITMAPINFO& bi = *(BITMAPINFO*)reserved_space;

  if (aDepth == 1) {
    RGBQUAD black = { 0, 0, 0, 0 };
    RGBQUAD white = { 255, 255, 255, 0 };

    bi.bmiColors[0] = white;
    bi.bmiColors[1] = black;
  }

  HBITMAP bmp = ::CreateDIBitmap(dc, &head, CBM_INIT, aImageData, &bi, DIB_RGB_COLORS);
  ::ReleaseDC(NULL, dc);
  return bmp;
}
