#include "pch.h"

#include "SkiaView.h"

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cmath>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "RNDawnContext.h"
#include "RNSkPresentThread.h"
#include "RNSkPresentThreadTimer.h"
#include "RNSkiaModule.h"
#include "utils/RNSkLog.h"

#pragma comment(lib, "dxgi.lib")

namespace RNSkia {

namespace {

// WinUI 3 ISwapChainPanelNative COM interface (from
// microsoft.ui.xaml.media.dxinterop.h, IID 63aad0b8-7c24-40ff-85a8-640d944cc325;
// distinct from UWP's windows.ui.xaml.media.dxinterop.h IID).
// Dawn's SwapChainD3D (see src/dawn/native/d3d/SwapChainD3D.cpp) queries this
// interface on the pointer in SurfaceDescriptorFromWindowsWinUISwapChainPanel
// to bind its IDXGISwapChain1 to the WinUI 3 SwapChainPanel.
MIDL_INTERFACE("63aad0b8-7c24-40ff-85a8-640d944cc325")
IWinUISwapChainPanelNative : public IUnknown {
public:
  virtual HRESULT STDMETHODCALLTYPE SetSwapChain(
      /* [annotation][in] */
      _In_ IDXGISwapChain * swapChain) = 0;
};

/**
 * Proxy COM object handed to Dawn's
 * SurfaceDescriptorFromWindowsWinUISwapChainPanel in place of the raw
 * SwapChainPanel pointer.
 *
 * Why a proxy is needed for WinUI 3 + Dawn D3D12 composition swapchains:
 * 1. High-DPI inverse scaling: A WinUI 3 SwapChainPanel is laid out in DIPs
 *    (points) and scaled by the XAML compositor by CompositionScaleX /
 *    CompositionScaleY (e.g. 1.5x at 150% DPI). We size the Dawn / D3D12
 *    swapchain buffers in physical pixels (ActualWidth * CompositionScaleX,
 *    ActualHeight * CompositionScaleY) so Skia renders at native display
 *    resolution. Without an inverse scale transform on the IDXGISwapChain2,
 *    XAML stretches the physical-pixel swapchain by CompositionScale again,
 *    rendering it oversized and clipped. Because Dawn creates and owns the
 *    IDXGISwapChain3 internally inside Surface::Configure() and hands it to
 *    IWinUISwapChainPanelNative::SetSwapChain(swapChain), intercepting
 *    SetSwapChain here lets us call IDXGISwapChain2::SetMatrixTransform with
 *    the inverse scale matrix (1 / CompositionScaleX, 1 / CompositionScaleY)
 *    every time Dawn creates or recreates the swapchain, and re-apply it
 *    whenever CompositionScaleChanged fires as the window moves across
 *    monitors.
 * 2. Clean COM teardown: When a view detaches or unmounts, calling
 *    SetSwapChain(nullptr) on the underlying SwapChainPanel breaks the COM
 *    reference between the XAML visual tree and the DXGI swapchain before the
 *    XamlIsland closes.
 */
class SwapChainPanelNativeProxy final : public IWinUISwapChainPanelNative {
public:
  explicit SwapChainPanelNativeProxy(IUnknown *innerPanel)
      : _inner(innerPanel) {}

  // Updates the composition scale from the UI thread (CompositionScaleChanged)
  // or before attach, and applies the inverse matrix to the active swapchain if
  // Dawn has already bound one.
  void setCompositionScale(float scaleX, float scaleY) {
    _scaleX.store(scaleX > 0.0f ? scaleX : 1.0f, std::memory_order_release);
    _scaleY.store(scaleY > 0.0f ? scaleY : 1.0f, std::memory_order_release);
    applyScaleToCurrentSwapChain();
  }

  // Detaches the DXGI swapchain from the underlying WinUI 3 SwapChainPanel when
  // RNSkWindowSurface::detach() runs its releaser on the present thread.
  void clearSwapChain() {
    std::lock_guard<std::mutex> lock(_mutex);
    _swapChain = nullptr;
    if (_inner != nullptr) {
      Microsoft::WRL::ComPtr<IWinUISwapChainPanelNative> nativePanel;
      if (SUCCEEDED(_inner->QueryInterface(IID_PPV_ARGS(&nativePanel))) &&
          nativePanel != nullptr) {
        nativePanel->SetSwapChain(nullptr);
      }
    }
  }

  // IUnknown -----------------------------------------------------------------

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override {
    if (ppvObject == nullptr) {
      return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IWinUISwapChainPanelNative)) {
      *ppvObject = static_cast<IWinUISwapChainPanelNative *>(this);
      AddRef();
      return S_OK;
    }
    // Forward any other interface queries to the underlying SwapChainPanel.
    if (_inner != nullptr) {
      return _inner->QueryInterface(riid, ppvObject);
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return _refCount.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG count = _refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (count == 0) {
      delete this;
    }
    return count;
  }

  // IWinUISwapChainPanelNative -----------------------------------------------

  // Called by Dawn's SwapChain::InitializeSwapChainFromScratch() on the present
  // thread when Surface::Configure() creates the composition swapchain.
  HRESULT STDMETHODCALLTYPE SetSwapChain(IDXGISwapChain *swapChain) override {
    if (_inner == nullptr) {
      return E_FAIL;
    }
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _swapChain = swapChain;
    }
    applyScaleToCurrentSwapChain();
    Microsoft::WRL::ComPtr<IWinUISwapChainPanelNative> nativePanel;
    HRESULT hr = _inner->QueryInterface(IID_PPV_ARGS(&nativePanel));
    if (FAILED(hr) || nativePanel == nullptr) {
      return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    return nativePanel->SetSwapChain(swapChain);
  }

private:
  ~SwapChainPanelNativeProxy() = default;

  // Applies the 1/CompositionScaleX by 1/CompositionScaleY inverse matrix to
  // the composition swapchain via IDXGISwapChain2::SetMatrixTransform.
  void applyScaleToCurrentSwapChain() {
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain2;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      if (_swapChain != nullptr) {
        _swapChain.As(&swapChain2);
      }
    }
    if (swapChain2 != nullptr) {
      const float sx = _scaleX.load(std::memory_order_acquire);
      const float sy = _scaleY.load(std::memory_order_acquire);
      DXGI_MATRIX_3X2_F matrix{};
      matrix._11 = 1.0f / sx;
      matrix._22 = 1.0f / sy;
      swapChain2->SetMatrixTransform(&matrix);
    }
  }

  std::atomic<ULONG> _refCount{1};
  Microsoft::WRL::ComPtr<IUnknown> _inner;
  std::mutex _mutex;
  Microsoft::WRL::ComPtr<IDXGISwapChain> _swapChain;
  std::atomic<float> _scaleX{1.0f};
  std::atomic<float> _scaleY{1.0f};
};

// Custom Fabric component properties parsed from JS for SkiaView
// (see src/specs/SkiaViewNativeComponent.ts).
// - nativeID: string id allocated by SkiaViewApi.allocateNativeId() and passed
//   as nativeID={`${nativeId}`} (parsed to size_t, as in apple/SkiaView.mm).
// - highBitDepth: requests the 16-bit float format (RGBA16Float) rather than
//   8-bit BGRA8Unorm.
// - colorSpace: "srgb", "display-p3", or empty for the platform default.
// The android* props (backing view, opacity, z-order) select and set up the
// backing view on Android; a WinUI 3 SwapChainPanel composites with alpha in
// the Windows composition tree like a CAMetalLayer on Apple platforms.
struct SkiaViewProps : winrt::implements<
                           SkiaViewProps,
                           winrt::Microsoft::ReactNative::IComponentProps> {
  SkiaViewProps(
      const winrt::Microsoft::ReactNative::ViewProps & /*viewProps*/,
      const winrt::Microsoft::ReactNative::IComponentProps &cloneFrom) {
    if (cloneFrom) {
      if (auto other = cloneFrom.try_as<SkiaViewProps>()) {
        nativeId = other->nativeId;
        highBitDepth = other->highBitDepth;
        colorSpace = other->colorSpace;
      }
    }
  }

  void SetProp(
      uint32_t /*hash*/, winrt::hstring propName,
      winrt::Microsoft::ReactNative::IJSValueReader value) noexcept {
    const std::string name = winrt::to_string(propName);
    if (name == "nativeID" || name == "nativeId") {
      if (value.ValueType() ==
          winrt::Microsoft::ReactNative::JSValueType::String) {
        try {
          nativeId = static_cast<size_t>(
              std::stoull(winrt::to_string(value.GetString())));
        } catch (...) {
          nativeId = 0;
        }
      } else if (value.ValueType() ==
                 winrt::Microsoft::ReactNative::JSValueType::Int64) {
        nativeId = static_cast<size_t>(value.GetInt64());
      } else if (value.ValueType() ==
                 winrt::Microsoft::ReactNative::JSValueType::Double) {
        nativeId = static_cast<size_t>(value.GetDouble());
      }
    } else if (name == "highBitDepth") {
      if (value.ValueType() ==
          winrt::Microsoft::ReactNative::JSValueType::Boolean) {
        highBitDepth = value.GetBoolean();
      }
    } else if (name == "colorSpace") {
      if (value.ValueType() ==
          winrt::Microsoft::ReactNative::JSValueType::String) {
        colorSpace = winrt::to_string(value.GetString());
      }
    }
  }

  size_t nativeId = 0;
  bool highBitDepth = false;
  std::string colorSpace;
};

// Active SkiaView instances keyed by their Fabric component Tag, so that
// SetUpdatePropsHandler, SetFinalizeUpdateHandler, and the Destroying hook can
// look up the per-mount C++ view instance.
std::mutex &instancesMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<int64_t, std::shared_ptr<SkiaView>> &instancesByTag() {
  static std::unordered_map<int64_t, std::shared_ptr<SkiaView>> map;
  return map;
}

// Active SwapChainPanelNativeProxy instances keyed by the raw IUnknown* of
// their SwapChainPanel, so that CompositionScaleChanged on the UI thread can
// update the inverse scale matrix on the proxy while the present thread owns
// the Dawn surface.
std::unordered_map<IUnknown *,
                   Microsoft::WRL::ComPtr<SwapChainPanelNativeProxy>> &
proxiesByPanel() {
  static std::unordered_map<
      IUnknown *, Microsoft::WRL::ComPtr<SwapChainPanelNativeProxy>>
      map;
  return map;
}

std::mutex &proxiesMutex() {
  static std::mutex mutex;
  return mutex;
}

// The color space the colorSpace prop asks for: "srgb" or "display-p3",
// else (the prop is not set) the platform's default (sRGB on Windows, see
// RNSkPlatformContext::prefersP3ColorSpace).
bool usesP3ColorSpace(
    const std::string &colorSpace,
    const std::shared_ptr<RNSkPlatformContext> &context) {
  if (colorSpace == "srgb") {
    return false;
  }
  if (colorSpace == "display-p3") {
    return true;
  }
  return context != nullptr ? context->prefersP3ColorSpace() : false;
}

} // namespace

#pragma region Frames

// The view's frame scheduler asks for the next frame of the display (see
// RNSkView::FrameHost): one callback is outstanding at most, so a request made
// while a previous callback has not fired yet reuses that callback rather than
// scheduling a second (which would tick twice per frame from then on). The
// weak_ptr token outlives this object so a callback that fires after the view
// is torn down finds nothing. Present thread.
void SkiaView::AnimationFrames::request() {
  if (_pending) {
    return;
  }
  _pending = true;
  auto *token = new std::weak_ptr<AnimationFrames>(weak_from_this());
  if (AnimationFrameSource::post(onFrame, token)) {
    return;
  }
  presentThread().post(
      [token]() { onFrame(AnimationFrameSource::unpaced(), token); });
}

void SkiaView::AnimationFrames::onFrame(
    const AnimationFrameSource::AnimationFrame &frame, void *data) {
  auto *token = static_cast<std::weak_ptr<AnimationFrames> *>(data);
  if (auto frames = token->lock()) {
    frames->_pending = false;
    auto view = frames->_view.lock();
    // A frame of the display, from this tick to the next. The scheduler hears
    // of the animation frames it asked for only (after cancelFrames(),
    // hasPendingAnimationFrame() is false), and asks for the next while frames
    // are left or keep coming.
    if (view != nullptr && view->hasPendingAnimationFrame()) {
      view->onAnimationFrame(frame);
    }
  }
  delete token;
}

#pragma endregion

#pragma region Lifecycle

SkiaView::SkiaView()
    : _manager(RNSkiaModule::latestActiveSkManager()),
      _canPresent(std::make_shared<std::atomic<bool>>(true)),
      _presenting(std::make_shared<bool>(false)) {}

SkiaView::~SkiaView() { releaseNativeView(); }

void SkiaView::initializeIsland(
    winrt::Microsoft::ReactNative::Composition::ContentIslandComponentView const
        &islandView) {
  // Host a WinUI 3 SwapChainPanel inside a XamlIsland and connect its
  // ContentIsland to React Native Windows' ContentIslandComponentView:
  // ContentIslandComponentView synchronizes layout bounds, clipping, scroll
  // transforms, focus, and DPI scaling from the Fabric composition tree without
  // creating a separate child HWND (avoiding HWND airspace clipping issues).
  _swapChainPanel = winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel();
  _xamlIsland = winrt::Microsoft::UI::Xaml::Hosting::XamlIsland();
  _xamlIsland.Content(_swapChainPanel);
  islandView.Connect(_xamlIsland.ContentIsland());

  ensureNativeView();

  std::weak_ptr<SkiaView> weakSelf = weak_from_this();

  // SizeChanged fires on the UI thread whenever Fabric updates the island's
  // layout bounds.
  _sizeChangedRevoker = _swapChainPanel.SizeChanged(
      winrt::auto_revoke,
      [weakSelf](winrt::Windows::Foundation::IInspectable const &,
                 winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const &) {
        if (auto self = weakSelf.lock()) {
          self->onPanelSizeOrScaleChanged();
        }
      });

  // CompositionScaleChanged fires on the UI thread when the window moves to a
  // monitor with a different DPI scale factor or the user changes display
  // scaling in Windows Settings.
  _scaleChangedRevoker = _swapChainPanel.CompositionScaleChanged(
      winrt::auto_revoke,
      [weakSelf](
          winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const &,
          winrt::Windows::Foundation::IInspectable const &) {
        if (auto self = weakSelf.lock()) {
          self->onPanelSizeOrScaleChanged();
        }
      });

  // When the panel enters the live XAML tree, mark the surface able to present,
  // attach the window at its current layout size, and let the present thread
  // flush any redraw or recording that waited meanwhile.
  _loadedRevoker = _swapChainPanel.Loaded(
      winrt::auto_revoke,
      [weakSelf](winrt::Windows::Foundation::IInspectable const &,
                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const &) {
        if (auto self = weakSelf.lock()) {
          self->_canPresent->store(true, std::memory_order_release);
          self->onPanelSizeOrScaleChanged();
          if (self->_view != nullptr) {
            auto view = self->_view;
            presentThread().post([view]() { view->onCanPresent(); });
          }
        }
      });

  // When the panel leaves the XAML tree (e.g. clipped out by a virtualized
  // container), detach the D3D12 swapchain surface so it holds no back buffers
  // while out of the hierarchy; what _view holds stays with it and is presented
  // again if the panel loads back.
  _unloadedRevoker = _swapChainPanel.Unloaded(
      winrt::auto_revoke,
      [weakSelf](winrt::Windows::Foundation::IInspectable const &,
                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const &) {
        if (auto self = weakSelf.lock()) {
          self->detachWindow();
        }
      });
}

void SkiaView::ensureNativeView() {
  if (_view != nullptr) {
    return;
  }
  auto manager = _manager.lock();
  if (manager == nullptr) {
    manager = RNSkiaModule::latestActiveSkManager();
    _manager = manager;
  }
  if (manager == nullptr) {
    return;
  }

  auto context = manager->getPlatformContext();
  _surface = std::make_shared<RNSkWindowSurface>();
  _surface->setHighBitDepth(_highBitDepth);
  _surface->setUseP3ColorSpace(usesP3ColorSpace(_colorSpace, context));
  _surface->setCanPresent(
      [canPresent = _canPresent]() { return canPresent->load(); });

  _view = std::make_shared<RNSkView>(context, _surface);
  // Presents, redraws and frame requests go to the shared present thread, where
  // the view is the one the tasks posted before see.
  _view->setPresentThread([](std::function<void()> task) {
    presentThread().post(std::move(task));
  });

  // The entries of the view and its window that belong to the present thread
  // are bound to it there (see RNSkView::bindPresentThread and
  // RNSkWindowSurface::bindPresentingThread), ahead of everything posted behind
  // this.
  {
    auto surface = _surface;
    auto view = _view;
    auto presenting = _presenting;
    presentThread().post([surface, view, presenting]() {
      *presenting = true;
      surface->bindPresentingThread();
      view->bindPresentThread();
    });
  }

  // A DXGI flip-model swapchain configured with PresentMode::Fifo shows each
  // frame presented in turn. The view's frame scheduler asks for the display's
  // frames on the present thread via AnimationFrames; a request from a view let
  // go of since (see leavePresentThread) stops at _presenting.
  _view->setPresentMode(rnsurface::FrameScheduler::PresentMode::kFifo);
  _animationFrames = std::make_shared<AnimationFrames>(_view);
  std::weak_ptr<AnimationFrames> weakFrames = _animationFrames;
  RNSkView::FrameHost host;
  host.requestAnimationFrame = [weakFrames, presenting = _presenting]() {
    if (*presenting) {
      if (auto frames = weakFrames.lock()) {
        frames->request();
      }
    }
  };
  // The timer the view polls the D3D12 queue with while a present is deferred
  // (see RNSkPacingPolicy.h and RNSkView::pollSubmittedWorkDone) and waits for
  // the end of a compositor slot with.
  host.setTimeout = [](std::chrono::nanoseconds delay,
                       std::function<void()> task) {
    PresentThreadTimer::getInstance().setTimeout(delay, std::move(task));
  };
  _view->setFrameHost(std::move(host));

  if (_nativeId != 0) {
    manager->registerSkiaView(_nativeId, _view);
  }
}

// UI thread, as the view leaves the tree or is destroyed: the present thread
// stops presenting the view and lets go of the window after the present in
// flight, if any, so that the UI thread never blocks waiting for a D3D12
// pipeline compile or present. Calling _surface->abandon() before posting
// ensures an in-flight frame replay on the present thread skips its final
// present onto a XamlIsland that is about to close (see
// RNSkWindowSurface::abandon).
void SkiaView::leavePresentThread() {
  if (_surface == nullptr) {
    return;
  }
  _surface->abandon();
  auto surface = _surface;
  auto view = _view;
  auto presenting = _presenting;
  presentThread().post([surface, view, presenting]() {
    *presenting = false;
    // Whatever the scheduler asked for goes with the window.
    if (view != nullptr) {
      view->cancelFrames();
    }
    surface->detach();
  });
  _windowAttached = false;
}

// Unregistered first, so that the native view is not destroyed under the
// registry's lock: it goes as the present thread lets go of it. The XamlIsland
// is explicitly closed on the UI thread so that WinUI 3 releases its input and
// composition connections immediately.
void SkiaView::releaseNativeView() {
  _sizeChangedRevoker.revoke();
  _scaleChangedRevoker.revoke();
  _loadedRevoker.revoke();
  _unloadedRevoker.revoke();

  unregisterView();
  leavePresentThread();

  if (_xamlIsland) {
    _xamlIsland.Content(nullptr);
    _xamlIsland.Close();
    _xamlIsland = nullptr;
  }
  _swapChainPanel = nullptr;
  _view = nullptr;
  _surface = nullptr;
  _animationFrames = nullptr;
}

void SkiaView::finalizeUpdates() {
  // Refresh the manager reference if the view was created before
  // RNSkiaModule.install() ran or if the bundle was reloaded.
  if (_manager.expired()) {
    _manager = RNSkiaModule::latestActiveSkManager();
  }
  ensureNativeView();
}

// Once: a destroyed or recycled view must not unregister an id another view
// may have since.
void SkiaView::unregisterView() {
  auto manager = _manager.lock();
  if (manager != nullptr && _nativeId != 0) {
    manager->unregisterSkiaView(_nativeId);
  }
  _nativeId = 0;
}

#pragma endregion

#pragma region Layout

void SkiaView::onPanelSizeOrScaleChanged() {
  if (!_swapChainPanel) {
    return;
  }
  ensureNativeView();
  if (_surface == nullptr || _view == nullptr) {
    return;
  }

  const double actualWidth = _swapChainPanel.ActualWidth();
  const double actualHeight = _swapChainPanel.ActualHeight();
  const float scaleX = _swapChainPanel.CompositionScaleX();
  const float scaleY = _swapChainPanel.CompositionScaleY();
  const float pixelDensity = scaleX > 0.0f ? scaleX : 1.0f;

  // The size of the SwapChainPanel in physical pixels. The layout is on the
  // pixel grid: round rather than truncate, so that a product like 1169.9999
  // gives the pixel size the layout means (matching -[SkiaView pixelWidth:height:]
  // in apple/SkiaView.mm).
  const int width = static_cast<int>(std::lround(actualWidth * scaleX));
  const int height = static_cast<int>(std::lround(actualHeight * scaleY));

  if (scaleX != _lastScaleX || scaleY != _lastScaleY) {
    _lastScaleX = scaleX;
    _lastScaleY = scaleY;
    IUnknown *rawPanel =
        reinterpret_cast<IUnknown *>(winrt::get_abi(_swapChainPanel));
    applySwapChainTransform(rawPanel, scaleX, scaleY);
  }

  updateWindowSize(width, height, pixelDensity);
}

void SkiaView::applySwapChainTransform(IUnknown *panelUnknown, float scaleX,
                                       float scaleY) {
  if (panelUnknown == nullptr) {
    return;
  }
  Microsoft::WRL::ComPtr<SwapChainPanelNativeProxy> proxy;
  {
    std::lock_guard<std::mutex> lock(proxiesMutex());
    auto it = proxiesByPanel().find(panelUnknown);
    if (it != proxiesByPanel().end()) {
      proxy = it->second;
    }
  }
  if (proxy != nullptr) {
    proxy->setCompositionScale(scaleX, scaleY);
  }
}

// On every layout or DPI scale change: the window is attached the first time
// it has a size, and handed a new size from then on with resize(), which the
// present thread configures the D3D12 swapchain for by its next present. A size
// of zero takes the window away until the next size: Dawn and DXGI refuse an
// empty surface, and without one the frames wait.
void SkiaView::updateWindowSize(int w, int h, float pixelDensity) {
  float previousDensity = 0.0f;
  const bool hadDensity = _surface->getPixelDensity(&previousDensity);
  _surface->setLayoutSize(w, h, pixelDensity);

  if (!_windowAttached || w <= 0 || h <= 0) {
    attachWindow(w, h, _lastScaleX, _lastScaleY);
    return;
  }
  if (w != _attachedWidth || h != _attachedHeight) {
    _attachedWidth = w;
    _attachedHeight = h;
    _surface->resize(w, h);
  } else if (hadDensity && previousDensity != pixelDensity) {
    // A new DPI density alone whose physical pixel dimensions happen to match
    // the previous ones does not resize the swapchain, so ask for a redraw at
    // the new scale (matching JniSkiaView::setLayoutSize on Android).
    _view->requestRedraw();
  }
}

// Attaches the window at the layout size just set, in physical pixels, or takes
// it away at a size of zero. The attach runs on the present thread, in order
// with the presents, detaches and redraws posted before it: a present in flight
// holds the thread for as long as its pipelines take to compile, and the UI
// thread must not wait for that. It takes the layout size as it is when it
// runs, so that a layout in between is not lost (a resize() before the window
// exists is dropped), and does nothing at a size of zero (that layout posted a
// detach after it). A layout that handed resize() a size while the window was
// being attached was dropped too: the window takes the latest layout size once
// attached. The UI thread records the window as attached right away, so that
// later layouts resize it.
void SkiaView::attachWindow(int w, int h, float scaleX, float scaleY) {
  if (w <= 0 || h <= 0 || !_swapChainPanel) {
    detachWindow();
    return;
  }
  _windowAttached = true;
  _attachedWidth = w;
  _attachedHeight = h;

  IUnknown *rawPanel =
      reinterpret_cast<IUnknown *>(winrt::get_abi(_swapChainPanel));
  if (rawPanel == nullptr) {
    return;
  }

  Microsoft::WRL::ComPtr<SwapChainPanelNativeProxy> proxy;
  {
    std::lock_guard<std::mutex> lock(proxiesMutex());
    auto &slot = proxiesByPanel()[rawPanel];
    if (slot == nullptr) {
      slot.Attach(new SwapChainPanelNativeProxy(rawPanel));
    }
    proxy = slot;
  }
  proxy->setCompositionScale(scaleX, scaleY);
  // AddRef for RNSkWindowSurface, given back by the releaser lambda below once
  // the Dawn surface over it is destroyed.
  proxy->AddRef();
  void *nativeHandle = static_cast<IWinUISwapChainPanelNative *>(proxy.Get());

  auto surface = _surface;
  const bool highBitDepth = _highBitDepth;
  const bool useP3ColorSpace =
      usesP3ColorSpace(_colorSpace, _view->getPlatformContext());

  presentThread().post([surface, nativeHandle, rawPanel, highBitDepth,
                        useP3ColorSpace]() {
    int width = 0;
    int height = 0;
    if (!surface->getLayoutSize(&width, &height) || width <= 0 || height <= 0) {
      static_cast<IWinUISwapChainPanelNative *>(nativeHandle)->Release();
      return;
    }
    surface->attach(
        nativeHandle, width, height, highBitDepth, useP3ColorSpace,
        [rawPanel](void *handle) {
          auto *proxyPtr =
              static_cast<SwapChainPanelNativeProxy *>(
                  static_cast<IWinUISwapChainPanelNative *>(handle));
          if (proxyPtr != nullptr) {
            proxyPtr->clearSwapChain();
            proxyPtr->Release();
          }
          std::lock_guard<std::mutex> lock(proxiesMutex());
          proxiesByPanel().erase(rawPanel);
        });
    int latestWidth = 0;
    int latestHeight = 0;
    if (surface->getLayoutSize(&latestWidth, &latestHeight) &&
        (latestWidth != width || latestHeight != height)) {
      surface->resize(latestWidth, latestHeight);
    }
  });
}

// Takes the window away on the present thread, after the present in flight:
// the layout collapsed to zero or the SwapChainPanel unloaded from the tree.
void SkiaView::detachWindow() {
  if (!_windowAttached || _surface == nullptr) {
    return;
  }
  _windowAttached = false;
  auto surface = _surface;
  presentThread().post([surface]() { surface->detach(); });
}

#pragma endregion

#pragma region Props

void SkiaView::updateProps(size_t nativeId, bool highBitDepth,
                           const std::string &colorSpace) {
  setHighBitDepth(highBitDepth);
  setColorSpace(colorSpace);
  ensureNativeView();
  if (nativeId != 0) {
    bindNativeId(nativeId);
  }
}

void SkiaView::bindNativeId(size_t nativeId) {
  _nativeId = nativeId;
  auto manager = _manager.lock();
  if (_view != nullptr && manager != nullptr && nativeId != 0) {
    manager->registerSkiaView(nativeId, _view);
  }
}

// The bit depth and the color space are read when the native view is created
// and kept until it goes (see releaseNativeView), as on Apple (SkiaView.mm) and
// Android (SkiaView.java): a change on a mounted canvas is ignored so the
// swapchain's pixel format never changes under a frame it shows.
void SkiaView::setHighBitDepth(bool highBitDepth) {
  if (_surface != nullptr) {
    return;
  }
  _highBitDepth = highBitDepth;
}

void SkiaView::setColorSpace(const std::string &colorSpace) {
  if (_surface != nullptr) {
    return;
  }
  _colorSpace = colorSpace;
}

#pragma endregion

#pragma region Registration

void SkiaView::RegisterComponent(
    winrt::Microsoft::ReactNative::IReactPackageBuilder const
        &packageBuilder) noexcept {
  auto fabricBuilder =
      packageBuilder
          .try_as<winrt::Microsoft::ReactNative::IReactPackageBuilderFabric>();
  if (!fabricBuilder) {
    return;
  }

  fabricBuilder.AddViewComponent(
      L"SkiaView",
      [](winrt::Microsoft::ReactNative::IReactViewComponentBuilder const
             &builder) noexcept {
        // Tell React Native Windows to bootstrap WinUI 3 XAML island support
        // for this component.
        builder.XamlSupport(true);

        builder.SetCreateProps(
            [](winrt::Microsoft::ReactNative::ViewProps props,
               const winrt::Microsoft::ReactNative::IComponentProps
                   &cloneFrom) noexcept {
              return winrt::make<SkiaViewProps>(props, cloneFrom);
            });

        auto compBuilder = builder.as<
            winrt::Microsoft::ReactNative::Composition::
                IReactCompositionViewComponentBuilder>();

        // Called per mounted SkiaView instance: create the C++ SkiaView, bind
        // its XamlIsland / SwapChainPanel to the ContentIslandComponentView,
        // and hook Destroying to tear down the swapchain and close the island
        // when Fabric unmounts the component.
        compBuilder.SetContentIslandComponentViewInitializer(
            [](winrt::Microsoft::ReactNative::Composition::
                   ContentIslandComponentView const &islandView) noexcept {
              auto instance = std::make_shared<SkiaView>();
              const int64_t tag = islandView.Tag();
              {
                std::lock_guard<std::mutex> lock(instancesMutex());
                instancesByTag()[tag] = instance;
              }
              instance->initializeIsland(islandView);

              islandView.Destroying(
                  [tag](winrt::Windows::Foundation::IInspectable const &,
                        winrt::Windows::Foundation::IInspectable const &) {
                    std::shared_ptr<SkiaView> removed;
                    {
                      std::lock_guard<std::mutex> lock(instancesMutex());
                      auto it = instancesByTag().find(tag);
                      if (it != instancesByTag().end()) {
                        removed = std::move(it->second);
                        instancesByTag().erase(it);
                      }
                    }
                    if (removed != nullptr) {
                      removed->releaseNativeView();
                    }
                  });
            });

        builder.SetUpdatePropsHandler(
            [](winrt::Microsoft::ReactNative::IComponentView const &view,
               winrt::Microsoft::ReactNative::IComponentProps const &newProps,
               winrt::Microsoft::ReactNative::IComponentProps const
                   & /*oldProps*/) noexcept {
              auto skiaProps = newProps.try_as<SkiaViewProps>();
              if (!skiaProps) {
                return;
              }
              std::shared_ptr<SkiaView> instance;
              {
                std::lock_guard<std::mutex> lock(instancesMutex());
                auto it = instancesByTag().find(view.Tag());
                if (it != instancesByTag().end()) {
                  instance = it->second;
                }
              }
              if (instance != nullptr) {
                instance->updateProps(skiaProps->nativeId,
                                      skiaProps->highBitDepth,
                                      skiaProps->colorSpace);
              }
            });

        builder.SetFinalizeUpdateHandler(
            [](winrt::Microsoft::ReactNative::IComponentView const &view,
               winrt::Microsoft::ReactNative::ComponentViewUpdateMask
                   /*mask*/) noexcept {
              std::shared_ptr<SkiaView> instance;
              {
                std::lock_guard<std::mutex> lock(instancesMutex());
                auto it = instancesByTag().find(view.Tag());
                if (it != instancesByTag().end()) {
                  instance = it->second;
                }
              }
              if (instance != nullptr) {
                instance->finalizeUpdates();
              }
            });
      });
}

#pragma endregion

} // namespace RNSkia
