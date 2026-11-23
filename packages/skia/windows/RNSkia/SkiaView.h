#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <winrt/Microsoft.ReactNative.Composition.h>
#include <winrt/Microsoft.ReactNative.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>

#include "RNSkAnimationFrame.h"
#include "RNSkManager.h"
#include "RNSkView.h"
#include "RNSkWindowSurface.h"

namespace RNSkia {

/**
 * The Fabric component behind <Canvas> and <SkiaGraphiteView> on Windows:
 * a WinUI 3 SwapChainPanel hosted inside a XamlIsland and connected to a React
 * Native Windows ContentIslandComponentView, presenting Graphite recordings on
 * D3D12 through Dawn (see apple/SkiaView.h and apple/SkiaView.mm).
 *
 * The frames are recorded by the native render thread pool (declarative
 * <Canvas> content) or by JS (SkiaViewApi.makeGraphiteContext) and replayed
 * onto the SwapChainPanel's DXGI composition swapchain on the shared present
 * thread (see RNSkPresentThread.h).
 *
 * Threading mirrors Apple's SkiaView.mm and Android's JniSkiaView.h:
 * - The XamlIsland and SwapChainPanel live on the UI thread, where WinUI 3
 *   layout and composition notifications (SizeChanged, CompositionScaleChanged,
 *   Loaded, Unloaded) and Fabric lifecycle hooks (initializeIsland,
 *   updateProps, finalizeUpdates, Destroying) run.
 * - The Dawn surface (wgpu::Surface over the SwapChainPanel) and all D3D12
 *   swapchain operations (attach, configure, replay, present, detach) run
 *   exclusively on the shared present thread, posted in FIFO order so that
 *   Graphite pipeline compilation never blocks the UI thread.
 */
class SkiaView : public std::enable_shared_from_this<SkiaView> {
public:
  SkiaView();
  ~SkiaView();

  SkiaView(const SkiaView &) = delete;
  SkiaView &operator=(const SkiaView &) = delete;

  /**
   * Registers the "SkiaView" Fabric component on the React Native Windows
   * package builder (see ReactPackageProvider::CreatePackage).
   */
  static void RegisterComponent(
      winrt::Microsoft::ReactNative::IReactPackageBuilder const
          &packageBuilder) noexcept;

  /**
   * UI thread: creates the WinUI 3 SwapChainPanel and XamlIsland for a mounted
   * ContentIslandComponentView, connects the island to the React Native
   * composition tree, and subscribes to panel layout, DPI scale, and tree
   * load/unload events.
   */
  void initializeIsland(
      winrt::Microsoft::ReactNative::Composition::ContentIslandComponentView const
          &islandView);

  /**
   * UI thread: applies props from React Native (nativeID, highBitDepth,
   * colorSpace). As on Apple (SkiaView.mm) and Android (SkiaView.java),
   * highBitDepth and colorSpace are read when the native view is created and
   * kept for its lifetime.
   */
  void updateProps(size_t nativeId, bool highBitDepth,
                   const std::string &colorSpace);

  /**
   * UI thread: refreshes the active RNSkManager reference when the component
   * commits updates (e.g. when a view is inserted or after a bundle reload)
   * and ensures the native view is created.
   */
  void finalizeUpdates();

  /**
   * UI thread: unregisters the view from RNSkManager, abandons and detaches the
   * D3D12 swapchain surface on the present thread, and closes the XamlIsland so
   * no input routing or COM references leak.
   */
  void releaseNativeView();

private:
  void ensureNativeView();
  void bindNativeId(size_t nativeId);
  void unregisterView();
  void setHighBitDepth(bool highBitDepth);
  void setColorSpace(const std::string &colorSpace);

  void onPanelSizeOrScaleChanged();
  void updateWindowSize(int width, int height, float pixelDensity);
  void attachWindow(int width, int height, float scaleX, float scaleY);
  void detachWindow();
  void leavePresentThread();

  static void applySwapChainTransform(IUnknown *panelUnknown, float scaleX,
                                      float scaleY);

  /**
   * The animation frames of the view, on the present thread: requests the next
   * display frame from AnimationFrameSource (see RNSkAnimationFrame.h). At most
   * one callback is in flight at a time; a callback that fires after
   * cancelFrames() is ignored via RNSkView::hasPendingAnimationFrame().
   */
  class AnimationFrames : public std::enable_shared_from_this<AnimationFrames> {
  public:
    explicit AnimationFrames(std::weak_ptr<RNSkView> view)
        : _view(std::move(view)) {}

    void request();

  private:
    static void onFrame(const AnimationFrameSource::AnimationFrame &frame,
                        void *data);

    std::weak_ptr<RNSkView> _view;
    // A callback was asked of AnimationFrameSource and has not fired yet.
    // Present thread, as the rest.
    bool _pending = false;
  };

  // Owned by the module (see finalizeUpdates). A view can outlive its manager:
  // on a reload the module is torn down while Fabric still holds the views of
  // the old bundle, which it destroys afterwards on the UI thread. The weak
  // reference makes that late unregistration a no-op, and a concurrent one keep
  // the manager alive for the duration of the call.
  std::weak_ptr<RNSkManager> _manager;
  // The native view and its window surface, from the view's first mount until
  // it is destroyed. _surface is declared before _view, which unbinds itself
  // from _surface in its destructor (see ~RNSkView).
  std::shared_ptr<RNSkWindowSurface> _surface;
  std::shared_ptr<RNSkView> _view;
  std::shared_ptr<AnimationFrames> _animationFrames;

  // The WinUI 3 XamlIsland and SwapChainPanel connected to React Native
  // Windows' ContentIslandComponentView on the UI thread, plus their event
  // revokers (revoked in releaseNativeView before the island is closed).
  winrt::Microsoft::UI::Xaml::Hosting::XamlIsland _xamlIsland{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel _swapChainPanel{nullptr};
  winrt::Microsoft::UI::Xaml::FrameworkElement::SizeChanged_revoker
      _sizeChangedRevoker{};
  winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel::
      CompositionScaleChanged_revoker _scaleChangedRevoker{};
  winrt::Microsoft::UI::Xaml::FrameworkElement::Loaded_revoker _loadedRevoker{};
  winrt::Microsoft::UI::Xaml::FrameworkElement::Unloaded_revoker
      _unloadedRevoker{};

  size_t _nativeId = 0;
  // The highBitDepth and colorSpace props (see usesP3ColorSpace) as they are
  // when the native view is created: the format and the color space of the
  // canvas are chosen then and kept, as on Apple and Android (see
  // setHighBitDepth).
  bool _highBitDepth = false;
  std::string _colorSpace;
  // Whether the window can show a frame. Written on the UI thread, read on the
  // present thread by the surface, which holds this rather than the view
  // (see leavePresentThread).
  std::shared_ptr<std::atomic<bool>> _canPresent;
  // Whether the present thread presents the view: from the bind task in
  // ensureNativeView to the leave task in leavePresentThread. What a frame
  // request that reaches the present thread checks before scheduling a tick.
  // Present thread.
  std::shared_ptr<bool> _presenting;
  // The UI thread's record of the window: whether one is attached (the attach
  // itself runs on the present thread, see attachWindow), at which size in
  // physical pixels, and at which composition scale. A new size is handed over
  // with resize(), which never waits for the present thread, rather than by
  // attaching again.
  bool _windowAttached = false;
  int _attachedWidth = 0;
  int _attachedHeight = 0;
  float _lastScaleX = 1.0f;
  float _lastScaleY = 1.0f;
};

} // namespace RNSkia
