#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <winrt/Microsoft.ReactNative.h>

#include "RNSkPlatformContext.h"

namespace facebook {
namespace react {
class CallInvoker;
}
} // namespace facebook

namespace RNSkia {

/**
 * The Windows platform context (see RNSkPlatformContext.h and
 * RNSkApplePlatformContext.h):
 * - Dispatches main-thread work onto the WinUI 3 UI thread via
 *   IReactContext::UIDispatcher().
 * - Schedules JS-thread work through the CallInvoker handed over by
 *   RNSkiaModule (which wraps React Native Windows' ExecuteJsi).
 * - Creates D3D12-backed offscreen SkSurfaces through DawnContext.
 * - Creates the DirectWrite SkFontMgr, enumerates installed Windows font
 *   families, and maps generic font family names ("System", "sans-serif",
 *   "serif", "monospace") to their Windows counterparts.
 * - Loads external streams asynchronously over HTTP/HTTPS (Metro or remote
 *   URLs), packaged ms-appx:/// and ms-appdata:/// URIs, file:// URIs, and
 *   local bundle paths.
 */
class RNSkWindowsPlatformContext : public RNSkPlatformContext {
public:
  RNSkWindowsPlatformContext(
      winrt::Microsoft::ReactNative::IReactContext reactContext,
      std::shared_ptr<facebook::react::CallInvoker> jsCallInvoker);

  ~RNSkWindowsPlatformContext() override = default;

  /**
   * The primary monitor's scale factor (physical pixels per DIP, where 96 DPI
   * is 1.0). A mounted view records its frames with the CompositionScaleX of
   * its own SwapChainPanel (see RNSkWindowSurface::setLayoutSize and
   * SkiaView::onPanelSizeOrScaleChanged), which follows per-monitor DPI changes
   * as the window moves between displays; this scale serves offscreen surfaces
   * and any frame recorded before the view is laid out.
   */
  static float mainScreenScaleFactor();

  /**
   * Where Dawn's shader blob cache and the Graphite pipeline warmup record are
   * kept on Windows (see DawnContext::setCacheDirectory): the package's
   * LocalCacheFolder\rnskia for packaged MSIX apps, or
   * %LOCALAPPDATA%\<AppName>\rnskia for unpackaged Win32 apps (mirroring the
   * bundle-scoped cache path on non-sandboxed macOS in apple/RNSkiaModule.mm).
   */
  static std::string getCacheDirectory();

  /**
   * Follows system DPI changes; a view's own density is read from its
   * SwapChainPanel layout (see RNSkView::getPixelDensity).
   */
  float getPixelDensity() override;

  /**
   * Whether views render in Display P3 rather than sRGB by default: false on
   * Windows, where DXGI composition swapchains composite in sRGB unless the
   * colorSpace prop explicitly asks for "display-p3".
   */
  bool prefersP3ColorSpace() override { return false; }

  void runOnMainThread(std::function<void()> func) override;

  void performStreamOperation(
      const std::string &sourceUri,
      const std::function<void(std::unique_ptr<SkStreamAsset>)> &op) override;

  void raiseError(const std::exception &err) override;

  sk_sp<SkSurface> makeOffscreenSurface(int width, int height,
                                        bool useP3ColorSpace = false) override;

  sk_sp<SkFontMgr> createFontMgr() override;

  std::vector<std::string> getSystemFontFamilies() override;

  std::string resolveFontFamily(const std::string &familyName) override;

private:
  // Kept to post main-thread work onto the instance's UIDispatcher.
  winrt::Microsoft::ReactNative::IReactContext _reactContext{nullptr};
};

} // namespace RNSkia
