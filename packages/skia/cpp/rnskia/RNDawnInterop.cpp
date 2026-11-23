#include "RNDawnContext.h"

#if defined(_WIN32)
#define RNSKIA_EXPORT __declspec(dllexport)
#else
#define RNSKIA_EXPORT __attribute__((visibility("default")))
#endif

// Runtime discovery surface for react-native-webgpu.
//
// webgpu looks this symbol up with dlsym(RTLD_DEFAULT, ...) (or GetProcAddress
// on Windows) when it creates its GPU object and, when found, adopts the
// returned instance instead of creating its own. Sharing one wgpu::Instance
// across both packages makes pointer-based device handoff
// (RNWebGPU.importDevice(Skia.getNativeDevice())) sound: async callbacks of
// imported devices settle on the instance webgpu pumps. Plain C, default
// visibility: no C++ ABI surface, works across separately compiled pods/DLLs,
// and stays dlsym-able under -fvisibility=hidden. Null when the device has no
// GPU Skia can use (see DawnContext::getGpuInfo): webgpu then creates its own
// instance, which fails on the same device in its own terms.
extern "C" RNSKIA_EXPORT WGPUInstance rnskia_getWGPUInstance() {
  auto *context = RNSkia::DawnContext::tryGetInstance();
  return context != nullptr ? context->getWGPUInstance().Get() : nullptr;
}
