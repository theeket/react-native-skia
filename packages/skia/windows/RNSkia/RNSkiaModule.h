#pragma once

#include <memory>
#include <mutex>

#include <JSI/JsiApiContext.h>
#include <NativeModules.h>
#include <ReactCommon/CallInvoker.h>
#include <winrt/Microsoft.ReactNative.h>

#include "RNSkManager.h"

namespace RNSkia {

/**
 * The Windows TurboModule / native module behind NativeSkiaModuleSpec
 * (see src/specs/NativeSkiaModule.ts and apple/RNSkiaModule.h):
 * - On Initialize (REACT_INIT), captures the ReactContext, builds a
 *   facebook::react::CallInvoker over the instance's JS dispatcher, and sets
 *   Dawn's persistent shader blob cache and Graphite pipeline warmup directory
 *   before the GPU is first probed.
 * - On install (REACT_SYNC_METHOD, called synchronously by JS when the module
 *   is first loaded), runs on the JS thread via ExecuteJsi to create the
 *   RNSkManager and install `SkiaApi`, `SkiaViewApi`, and
 *   `__skiaWorkletHelpers` on the global JS object.
 * - Exposes latestActiveSkManager() so Fabric SkiaView instances created on the
 *   UI thread can register and unregister themselves with the active manager.
 */
REACT_MODULE(RNSkiaModule, L"RNSkiaModule")
struct RNSkiaModule {
  REACT_INIT(Initialize)
  void Initialize(
      winrt::Microsoft::ReactNative::ReactContext const &reactContext) noexcept;

  ~RNSkiaModule();

  REACT_SYNC_METHOD(install)
  bool install() noexcept;

  std::shared_ptr<RNSkManager> skManager() const noexcept { return _skManager; }

  // Fabric components do not have a better way to interact with TurboModules.
  // Workaround to get the SkManager instance from singleton, matching Apple's
  // +[RNSkiaModule latestActiveSkManager].
  static std::shared_ptr<RNSkManager> latestActiveSkManager() noexcept;

private:
  // Handed to us by React Native Windows when the module is initialized
  // (see Initialize), before JS calls install(). Valid until the module is
  // destroyed when the React instance tears down or reloads.
  winrt::Microsoft::ReactNative::ReactContext _reactContext{nullptr};
  std::shared_ptr<facebook::react::CallInvoker> _jsInvoker;
  std::shared_ptr<RNSkManager> _skManager;
};

} // namespace RNSkia
