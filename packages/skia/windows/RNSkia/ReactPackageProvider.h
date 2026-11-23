#pragma once

#include "ReactPackageProvider.g.h"

namespace winrt::RNSkia::implementation {

/**
 * The package provider of react-native-skia on Windows (see
 * ReactPackageProvider.idl): registers the native module (RNSkiaModule) and the
 * Fabric component view (SkiaView) with the React Native Windows instance as it
 * starts up.
 */
struct ReactPackageProvider : ReactPackageProviderT<ReactPackageProvider> {
  ReactPackageProvider() = default;

  /**
   * Called by React Native Windows when building the native package catalog for
   * a ReactInstanceSettings.
   */
  void CreatePackage(
      winrt::Microsoft::ReactNative::IReactPackageBuilder const
          &packageBuilder) noexcept;
};

} // namespace winrt::RNSkia::implementation

namespace winrt::RNSkia::factory_implementation {

struct ReactPackageProvider
    : ReactPackageProviderT<ReactPackageProvider,
                            implementation::ReactPackageProvider> {};

} // namespace winrt::RNSkia::factory_implementation
