#include "pch.h"

#include "RNSkiaModule.h"

#include <utility>

#include "RNDawnContext.h"
#include "RNSkWindowsPlatformContext.h"
#include "utils/RNSkLog.h"

namespace RNSkia {

namespace {

// Guards activeManagerSlot(): read on the UI thread when Fabric mounts or
// finalizes a SkiaView (see SkiaView::ensureNativeView and finalizeUpdates),
// written on the JS thread when install() creates the RNSkManager, and cleared
// when the module is torn down on reload. Holding a weak_ptr in the slot ensures
// the singleton never extends the manager's lifetime past module teardown.
std::mutex &activeManagerMutex() {
  static std::mutex mutex;
  return mutex;
}

std::weak_ptr<RNSkManager> &activeManagerSlot() {
  static std::weak_ptr<RNSkManager> slot;
  return slot;
}

/**
 * Bridges React Native Windows' JS dispatcher and ExecuteJsi to
 * facebook::react::CallInvoker.
 *
 * RNSkPlatformContext::runOnJavascriptThread schedules asynchronous work (such
 * as settling GPU readback or stream-loading promises via RNSkRuntimeScheduler)
 * through CallInvoker::invokeAsync(CallFunc), which expects the active
 * facebook::jsi::Runtime& when the callback runs on the JS thread. Using
 * ExecuteJsi rather than a captured jsi::Runtime* guarantees that if a reload
 * destroys the runtime while async work is in flight, the callback is dropped
 * rather than run against a dangling runtime (see RNSkPlatformContext.h).
 */
class RNSkWindowsCallInvoker final : public facebook::react::CallInvoker {
public:
  explicit RNSkWindowsCallInvoker(
      winrt::Microsoft::ReactNative::ReactContext reactContext)
      : _reactContext(std::move(reactContext)) {}

  void invokeAsync(facebook::react::CallFunc &&func) noexcept override {
    if (!_reactContext) {
      return;
    }
    try {
      // ExecuteJsi posts the lambda to the JS thread when called from another
      // thread, and passes the live facebook::jsi::Runtime& when it runs.
      winrt::Microsoft::ReactNative::ExecuteJsi(
          _reactContext,
          [func = std::move(func)](facebook::jsi::Runtime &runtime) mutable {
            func(runtime);
          });
    } catch (...) {
      // Instance is tearing down; drop the async callback.
    }
  }

  void invokeSync(facebook::react::CallFunc &&func) override {
    if (!_reactContext) {
      return;
    }
    // If already on the JS thread, ExecuteJsi runs synchronously in place.
    if (_reactContext.JSDispatcher().HasThreadAccess()) {
      winrt::Microsoft::ReactNative::ExecuteJsi(
          _reactContext,
          [&func](facebook::jsi::Runtime &runtime) { func(runtime); });
      return;
    }
    // From any other thread, post via ExecuteJsi and wait for completion.
    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    winrt::Microsoft::ReactNative::ExecuteJsi(
        _reactContext, [&](facebook::jsi::Runtime &runtime) {
          func(runtime);
          {
            std::lock_guard<std::mutex> lock(mutex);
            completed = true;
          }
          cv.notify_all();
        });
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&]() { return completed; });
  }

private:
  winrt::Microsoft::ReactNative::ReactContext _reactContext;
};

} // namespace

#pragma region Accessors

std::shared_ptr<RNSkManager> RNSkiaModule::latestActiveSkManager() noexcept {
  std::lock_guard<std::mutex> lock(activeManagerMutex());
  return activeManagerSlot().lock();
}

#pragma endregion

#pragma region Setup and invalidation

void RNSkiaModule::Initialize(
    winrt::Microsoft::ReactNative::ReactContext const &reactContext) noexcept {
  _reactContext = reactContext;
  _jsInvoker = std::make_shared<RNSkWindowsCallInvoker>(reactContext);

  // Before the GPU is first asked for: where Dawn keeps the D3D12 shaders and
  // pipeline blobs it compiled for the next launch, and where
  // RNSkPipelineWarmup records the Graphite pipelines used (see DawnContext).
  const std::string cacheDir = RNSkWindowsPlatformContext::getCacheDirectory();
  if (!cacheDir.empty()) {
    DawnContext::setCacheDirectory(cacheDir);
  }
  // Windows DXGI swapchains default to sRGB; warmup builds the sRGB pipelines.
  DawnContext::setPrecompileDisplayP3(false);
  // Desktop Windows apps may submit D3D12 work while unfocused or occluded,
  // just as macOS apps do (see RNSkApplePlatformContext::observeApplicationState).
  DawnContext::setForeground(true);
}

RNSkiaModule::~RNSkiaModule() {
  // Invalidated when the React instance tears down or reloads: drop the active
  // manager reference and release _skManager, which clears ViewRegistry and
  // RNSkGraphiteTargetRegistry (see ~RNSkManager).
  std::lock_guard<std::mutex> lock(activeManagerMutex());
  if (activeManagerSlot().lock() == _skManager) {
    activeManagerSlot().reset();
  }
  _skManager = nullptr;
  _jsInvoker = nullptr;
}

bool RNSkiaModule::install() noexcept {
  if (_skManager != nullptr) {
    // Already initialized, ignore call.
    return true;
  }
  if (!_reactContext || _jsInvoker == nullptr) {
    RNSkLogger::logToConsole(
        "[RNSkiaModule] Failed to install: the ReactContext has not been "
        "provided to the module yet.");
    return false;
  }

  // Called from JS as a synchronous blocking method (REACT_SYNC_METHOD) on the
  // JS thread: ExecuteJsi runs inline on the calling JS thread and hands us the
  // facebook::jsi::Runtime& to install SkiaApi, SkiaViewApi, and
  // __skiaWorkletHelpers on.
  bool installed = false;
  try {
    winrt::Microsoft::ReactNative::ExecuteJsi(
        _reactContext, [this, &installed](facebook::jsi::Runtime &runtime) {
          auto platformContext = std::make_shared<RNSkWindowsPlatformContext>(
              _reactContext.Handle(), _jsInvoker);
          _skManager = std::make_shared<RNSkManager>(&runtime, _jsInvoker,
                                                     std::move(platformContext));
          {
            std::lock_guard<std::mutex> lock(activeManagerMutex());
            activeManagerSlot() = _skManager;
          }
          installed = true;
        });
  } catch (const std::exception &e) {
    RNSkLogger::logToConsole("[RNSkiaModule] Exception during install: %s",
                             e.what());
    return false;
  } catch (...) {
    RNSkLogger::logToConsole("[RNSkiaModule] Unknown exception during install");
    return false;
  }

  return installed;
}

#pragma endregion

} // namespace RNSkia
