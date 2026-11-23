#include "pch.h"

#include "RNSkWindowsPlatformContext.h"

#include <dwrite.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <fstream>
#include <set>
#include <thread>
#include <utility>

#include "RNDawnContext.h"
#include "utils/RNSkLog.h"

#pragma warning(push)
#pragma warning(disable : 4244 4267)

#include "include/core/SkData.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/ports/SkTypeface_win.h"

#pragma warning(pop)

#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace RNSkia {

namespace {

// Converts a UTF-8 std::string into a UTF-16 std::wstring for Win32 and WinRT
// APIs (Uri, StorageFile, std::ifstream, MultiByteToWideChar).
std::wstring utf8ToWide(const std::string &str) {
  if (str.empty()) {
    return {};
  }
  const int size =
      MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                          nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                      result.data(), size);
  return result;
}

// Converts a UTF-16 wchar_t buffer into a UTF-8 std::string. When `length` is
// -1, the input is null-terminated and WideCharToMultiByte's returned count
// includes the trailing null terminator, which we exclude from the std::string.
std::string wideToUtf8(const wchar_t *wstr, int length = -1) {
  if (wstr == nullptr || length == 0) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, wstr, length, nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  const int charCount = (length == -1) ? (size - 1) : size;
  if (charCount <= 0) {
    return {};
  }
  std::string result(static_cast<size_t>(charCount), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wstr, length, result.data(), size, nullptr,
                      nullptr);
  return result;
}

// Reads a local file from disk into an SkMemoryStream; returns nullptr when the
// file does not exist or cannot be read so the caller can try fallback asset
// directories or report a missing resource.
std::unique_ptr<SkStreamAsset> loadLocalFileStream(const std::wstring &path) {
  std::ifstream file(path.c_str(), std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return nullptr;
  }
  const auto end = file.tellg();
  if (end < 0) {
    return nullptr;
  }
  const size_t size = static_cast<size_t>(end);
  file.seekg(0, std::ios::beg);
  auto skData = SkData::MakeUninitialized(size);
  if (size > 0 &&
      !file.read(static_cast<char *>(skData->writable_data()),
                 static_cast<std::streamsize>(size))) {
    return nullptr;
  }
  return SkMemoryStream::Make(std::move(skData));
}

// The directory containing the running application's executable, where bundled
// assets (or the Assets\ and Bundle\ subdirectories) live in unpackaged Win32
// React Native Windows builds.
std::wstring executableDirectory() {
  wchar_t buffer[MAX_PATH] = {0};
  const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) {
    return {};
  }
  std::wstring fullPath(buffer, len);
  const auto slash = fullPath.find_last_of(L"\\/");
  return slash != std::wstring::npos ? fullPath.substr(0, slash)
                                     : std::wstring{};
}

} // namespace

RNSkWindowsPlatformContext::RNSkWindowsPlatformContext(
    winrt::Microsoft::ReactNative::IReactContext reactContext,
    std::shared_ptr<facebook::react::CallInvoker> jsCallInvoker)
    : RNSkPlatformContext(std::move(jsCallInvoker), mainScreenScaleFactor()),
      _reactContext(std::move(reactContext)) {}

float RNSkWindowsPlatformContext::mainScreenScaleFactor() {
  // GetDpiForSystem returns the primary monitor's DPI (96 DPI = 100% scale,
  // 144 DPI = 150% scale, 192 DPI = 200% scale).
  const UINT dpi = GetDpiForSystem();
  return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

std::string RNSkWindowsPlatformContext::getCacheDirectory() {
  // Packaged MSIX apps have an isolated per-package LocalCacheFolder managed
  // by the OS.
  try {
    auto localFolder =
        winrt::Windows::Storage::ApplicationData::Current().LocalCacheFolder();
    if (localFolder) {
      auto path = winrt::to_string(localFolder.Path());
      if (!path.empty()) {
        return path + "\\rnskia";
      }
    }
  } catch (...) {
    // Unpackaged Win32 apps ( the default in modern React Native for Windows /
    // WinAppSDK) have no package identity, so ApplicationData::Current() throws
    // APPMODEL_ERROR_NO_PACKAGE. Fall back to %LOCALAPPDATA%\<AppName>\rnskia,
    // scoping by executable name just as non-sandboxed macOS apps scope
    // NSCachesDirectory by bundle identifier in apple/RNSkiaModule.mm.
  }

  PWSTR localAppData = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                     &localAppData)) &&
      localAppData != nullptr) {
    std::string base = wideToUtf8(localAppData);
    CoTaskMemFree(localAppData);
    if (!base.empty()) {
      wchar_t exePath[MAX_PATH] = {0};
      std::string appName = "react-native-skia";
      if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::wstring full(exePath);
        const auto slash = full.find_last_of(L"\\/");
        std::wstring name =
            slash != std::wstring::npos ? full.substr(slash + 1) : full;
        const auto dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) {
          name = name.substr(0, dot);
        }
        if (!name.empty()) {
          appName = wideToUtf8(name.c_str(), static_cast<int>(name.size()));
        }
      }
      return base + "\\" + appName + "\\rnskia";
    }
  }
  return {};
}

float RNSkWindowsPlatformContext::getPixelDensity() {
  return mainScreenScaleFactor();
}

void RNSkWindowsPlatformContext::performStreamOperation(
    const std::string &sourceUri,
    const std::function<void(std::unique_ptr<SkStreamAsset>)> &op) {
  // Loads the resource on a detached worker thread and calls `op` once with a
  // stream over its bytes, or nullptr when nothing could be loaded at the URI,
  // matching RNSkApplePlatformContext::performStreamOperation.
  auto loader = [sourceUri, op]() {
    // Initialize a multithreaded WinRT apartment on this std::thread so we can
    // use WinRT's HttpClient and StorageFile APIs and block on their IAsyncInfo
    // results with .get().
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
      // 1. Remote or Metro bundler URLs (http:// or https://) via WinRT
      //    Windows.Web.Http.HttpClient.
      if (sourceUri.rfind("http://", 0) == 0 ||
          sourceUri.rfind("https://", 0) == 0) {
        winrt::Windows::Foundation::Uri uri(utf8ToWide(sourceUri));
        winrt::Windows::Web::Http::HttpClient client;
        auto response = client.GetAsync(uri).get();
        if (!response.IsSuccessStatusCode()) {
          // Nothing at the URI: the caller is told rather than handed no bytes.
          op(nullptr);
          return;
        }
        auto buffer = response.Content().ReadAsBufferAsync().get();
        const uint32_t length = buffer.Length();
        auto reader =
            winrt::Windows::Storage::Streams::DataReader::FromBuffer(buffer);
        std::vector<uint8_t> bytes(length);
        if (length > 0) {
          reader.ReadBytes(bytes);
        }
        auto skData = SkData::MakeWithCopy(bytes.data(), bytes.size());
        op(SkMemoryStream::Make(std::move(skData)));
        return;
      }

      // 2. Packaged application URIs (ms-appx:/// or ms-appdata:///) via WinRT
      //    StorageFile.
      if (sourceUri.rfind("ms-appx://", 0) == 0 ||
          sourceUri.rfind("ms-appdata://", 0) == 0) {
        winrt::Windows::Foundation::Uri uri(utf8ToWide(sourceUri));
        auto file =
            winrt::Windows::Storage::StorageFile::GetFileFromApplicationUriAsync(
                uri)
                .get();
        auto buffer =
            winrt::Windows::Storage::FileIO::ReadBufferAsync(file).get();
        const uint32_t length = buffer.Length();
        auto reader =
            winrt::Windows::Storage::Streams::DataReader::FromBuffer(buffer);
        std::vector<uint8_t> bytes(length);
        if (length > 0) {
          reader.ReadBytes(bytes);
        }
        auto skData = SkData::MakeWithCopy(bytes.data(), bytes.size());
        op(SkMemoryStream::Make(std::move(skData)));
        return;
      }

      // 3. Local file:// URIs or direct filesystem paths.
      std::string localPath = sourceUri;
      if (localPath.rfind("file:///", 0) == 0) {
        localPath = localPath.substr(8);
      } else if (localPath.rfind("file://", 0) == 0) {
        localPath = localPath.substr(7);
      }

      std::wstring widePath = utf8ToWide(localPath);
      if (auto stream = loadLocalFileStream(widePath)) {
        op(std::move(stream));
        return;
      }

      // 4. Relative asset name without a scheme: look next to the executable
      //    and inside its Assets\ or Bundle\ subdirectories, where React Native
      //    for Windows places bundled image and font assets (analogous to
      //    UIImage/NSImage imageNamed: on Apple platforms).
      const std::wstring exeDir = executableDirectory();
      if (!exeDir.empty()) {
        if (auto stream = loadLocalFileStream(exeDir + L"\\" + widePath)) {
          op(std::move(stream));
          return;
        }
        if (auto stream =
                loadLocalFileStream(exeDir + L"\\Assets\\" + widePath)) {
          op(std::move(stream));
          return;
        }
        if (auto stream =
                loadLocalFileStream(exeDir + L"\\Bundle\\" + widePath)) {
          op(std::move(stream));
          return;
        }
      }

      // Nothing at the uri: the caller is told rather than handed no bytes.
      op(nullptr);
    } catch (...) {
      op(nullptr);
    }
  };

  // Fire and forget the thread - will be resolved on completion.
  std::thread(std::move(loader)).detach();
}

void RNSkWindowsPlatformContext::raiseError(const std::exception &err) {
  // Output the fatal error to the debugger console and JS logging channel.
  RNSkLogger::logToConsole("RNSkia fatal error: %s", err.what());
}

sk_sp<SkSurface>
RNSkWindowsPlatformContext::makeOffscreenSurface(int width, int height,
                                                 bool useP3ColorSpace) {
  return DawnContext::getInstance().MakeOffscreen(width, height,
                                                  useP3ColorSpace);
}

sk_sp<SkFontMgr> RNSkWindowsPlatformContext::createFontMgr() {
  // DirectWrite is Windows' modern text and font engine; passing nullptr has
  // Skia use the shared system IDWriteFactory and system font collection
  // (see include/ports/SkTypeface_win.h).
  return SkFontMgr_New_DirectWrite();
}

std::vector<std::string> RNSkWindowsPlatformContext::getSystemFontFamilies() {
  std::set<std::string> uniqueFamilies;

  // Enumerate the system font families registered with DirectWrite so that
  // Skia.FontMgr.System().getFamilyName() and system font lookups include all
  // installed Windows fonts in the user's locale (falling back to "en-us" or
  // the first localized entry when the user locale is not present on the font).
  Microsoft::WRL::ComPtr<IDWriteFactory> factory;
  if (SUCCEEDED(DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown **>(factory.GetAddressOf())))) {
    Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
    if (SUCCEEDED(factory->GetSystemFontCollection(&collection, FALSE)) &&
        collection != nullptr) {
      const UINT32 count = collection->GetFontFamilyCount();
      for (UINT32 i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IDWriteFontFamily> family;
        if (FAILED(collection->GetFontFamily(i, &family)) ||
            family == nullptr) {
          continue;
        }
        Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(family->GetFamilyNames(&names)) || names == nullptr) {
          continue;
        }
        UINT32 index = 0;
        BOOL exists = FALSE;
        wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {0};
        if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0) {
          names->FindLocaleName(localeName, &index, &exists);
        }
        if (!exists) {
          names->FindLocaleName(L"en-us", &index, &exists);
        }
        if (!exists) {
          index = 0;
        }
        UINT32 length = 0;
        if (SUCCEEDED(names->GetStringLength(index, &length)) && length > 0) {
          std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
          if (SUCCEEDED(names->GetString(index, buffer.data(), length + 1))) {
            std::string utf8 =
                wideToUtf8(buffer.data(), static_cast<int>(length));
            if (!utf8.empty()) {
              uniqueFamilies.insert(std::move(utf8));
            }
          }
        }
      }
    }
  }

  // Ensure the default Windows UI font is always present even if DirectWrite
  // enumeration fails.
  if (uniqueFamilies.empty()) {
    uniqueFamilies.insert("Segoe UI");
  }
  return std::vector<std::string>(uniqueFamilies.begin(), uniqueFamilies.end());
}

std::string
RNSkWindowsPlatformContext::resolveFontFamily(const std::string &familyName) {
  // Handle special and CSS generic font family names like React Native Windows
  // and RNSkApplePlatformContext do:
  // - "System" / "sans-serif" resolves to Windows' system UI font ("Segoe UI").
  // - "serif" resolves to "Times New Roman".
  // - "monospace" resolves to "Consolas".
  if (familyName == "System" || familyName == "system" ||
      familyName == "sans-serif") {
    return "Segoe UI";
  }
  if (familyName == "SystemCondensed" || familyName == "system-condensed") {
    // Return system font - condensed trait is handled via font style.
    return "Segoe UI";
  }
  // CSS generic font families
  if (familyName == "serif") {
    return "Times New Roman";
  }
  if (familyName == "monospace") {
    return "Consolas";
  }
  // Return as-is if no mapping exists.
  return familyName;
}

void RNSkWindowsPlatformContext::runOnMainThread(std::function<void()> func) {
  // Post to the React Native Windows UI thread dispatcher (analogous to
  // dispatch_async(dispatch_get_main_queue(), ...) on Apple platforms).
  if (_reactContext) {
    auto dispatcher = _reactContext.UIDispatcher();
    if (dispatcher) {
      dispatcher.Post([func = std::move(func)]() { func(); });
      return;
    }
  }
  func();
}

} // namespace RNSkia
