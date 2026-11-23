#pragma once

// Define NOMINMAX and WIN32_LEAN_AND_MEAN before any Windows headers so that
// the legacy min/max macros from <windows.h> do not collide with std::min /
// std::max in Skia, Dawn, and React Native headers, and rarely used Win32 APIs
// are left out of every translation unit.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// <unknwn.h> must come before C++/WinRT headers when classic COM interfaces
// (IUnknown, IDXGISwapChain, IDWriteFactory, ISwapChainPanelNative) are used
// alongside winrt::com_ptr and Microsoft::WRL::ComPtr.
#include <unknwn.h>
#include <windows.h>

// C++/WinRT projections for React Native for Windows (Fabric / Composition and
// ContentIslandComponentView) and the WinUI 3 / Windows App SDK hosting layer
// (XamlIsland, SwapChainPanel, ContentIsland, DispatcherQueue).
#include <winrt/Microsoft.ReactNative.Composition.h>
#include <winrt/Microsoft.ReactNative.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Web.Http.h>

// Microsoft.ReactNative.Cxx helpers for JSI execution (ExecuteJsi) and
// attribute-based TurboModule registration (REACT_MODULE, REACT_INIT,
// REACT_SYNC_METHOD).
#include <JSI/JsiApiContext.h>
#include <NativeModules.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
