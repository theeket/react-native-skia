#include "pch.h"

#include "ReactPackageProvider.h"
#if __has_include("ReactPackageProvider.g.cpp")
#include "ReactPackageProvider.g.cpp"
#endif

#include "RNSkiaModule.h"
#include "SkiaView.h"

namespace winrt::RNSkia::implementation {

void ReactPackageProvider::CreatePackage(
    winrt::Microsoft::ReactNative::IReactPackageBuilder const
        &packageBuilder) noexcept {
  // Registers every REACT_MODULE in this translation unit / library
  // (RNSkiaModule, see RNSkiaModule.h), with `useDefaultNamespace = true` so it
  // is exposed to TurboModuleRegistry under its module name ("RNSkiaModule").
  winrt::Microsoft::ReactNative::AddAttributedModules(packageBuilder, true);
  // Fabric view components are not discovered through attributes on Windows:
  // register the "SkiaView" component explicitly on IReactPackageBuilderFabric
  // (see SkiaView::RegisterComponent).
  ::RNSkia::SkiaView::RegisterComponent(packageBuilder);
}

} // namespace winrt::RNSkia::implementation
