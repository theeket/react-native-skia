#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <dwmapi.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "RNSkPresentThread.h"
#include "RNSkPresentThreadTimer.h"
#include "rnsurface/FrameClock.h"
#include "rnsurface/FrameScheduler.h"

#pragma comment(lib, "dwmapi.lib")

namespace RNSkia {

/**
 * The animation frames of Windows (see
 * rnsurface::FrameScheduler::AnimationFrame), paced from Desktop Window Manager
 * composition timing (DwmGetCompositionTimingInfo) on the clock of
 * rnsurface::frameClockNow(): what CADisplayLink is on Apple platforms (see
 * apple/SkiaView.mm) and AChoreographer is on Android (see
 * android/cpp/rnskia-android/RNSkAnimationFrame.h).
 *
 * DwmGetCompositionTimingInfo reports the QueryPerformanceCounter (QPC)
 * timestamp of the latest vertical blank (qpcVBlank) and the display's refresh
 * period in QPC ticks (qpcRefreshPeriod). From those we project the next
 * VBlank on the frame scheduler's clock and arm PresentThreadTimer for that
 * moment. When the callback fires on the present thread, it hands the scheduler
 * the frame's start, its refresh interval, and its composition deadline: as on
 * Apple platforms (see animationFrame() in apple/SkiaView.mm), a frame
 * presented makes the display frame that ends at `end` as long as the
 * compositor finds it ready a moment before (`end - interval / 4`), leaving a
 * quarter of a frame for DWM composition; a frame ready later than that waits
 * for the next animation frame rather than land in a slot that already got one.
 *
 * When DWM timing is unavailable (e.g. a headless or remote session), the
 * interval falls back to the primary display's refresh rate from
 * EnumDisplaySettingsW, and ultimately to 60 Hz (kDefaultInterval).
 */
class AnimationFrameSource {
public:
  using AnimationFrame = rnsurface::FrameScheduler::AnimationFrame;
  using Callback = void (*)(const AnimationFrame &frame, void *data);

  /**
   * Has `callback(frame, data)` called at the next frame of the display, on
   * the present thread. Present thread.
   */
  static bool post(Callback callback, void *data) {
    const auto now = rnsurface::frameClockNow();
    const int64_t interval = queryDisplayIntervalNanos();
    int64_t nextFrameStart = now;

    // Ask DWM for its composition timing baseline: qpcVBlank is the QPC tick
    // of the most recent (or upcoming) VBlank, and qpcRefreshPeriod is the
    // length of one refresh cycle. Project forward by whole multiples of
    // `interval` until we reach the first VBlank strictly after `now`.
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) &&
        timing.qpcRefreshPeriod > 0 && timing.qpcVBlank > 0) {
      const int64_t vblankNanos = qpcToSteadyNanos(timing.qpcVBlank, now);
      if (vblankNanos > 0) {
        if (vblankNanos > now) {
          nextFrameStart = vblankNanos;
        } else {
          const int64_t elapsed = now - vblankNanos;
          const int64_t periods = (elapsed / interval) + 1;
          nextFrameStart = vblankNanos + periods * interval;
        }
      }
    } else {
      nextFrameStart = now + interval;
    }

    const int64_t delayNanos = std::max<int64_t>(0, nextFrameStart - now);
    PresentThreadTimer::getInstance().setTimeout(
        std::chrono::nanoseconds(delayNanos), [callback, data, interval]() {
          const auto start = rnsurface::frameClockNow();
          const auto end = start + interval;
          // Leave a quarter of a frame before `end` for the compositor to pick
          // up the presented swapchain buffer, matching Apple's CADisplayLink
          // deadline in apple/SkiaView.mm.
          const AnimationFrame frame{start, interval, end - interval / 4};
          callback(frame, data);
        });
    return true;
  }

  /**
   * An animation frame for a fallback path without display timing: a frame that
   * starts and ends now.
   */
  static AnimationFrame unpaced() {
    const auto now = rnsurface::frameClockNow();
    return {now, kDefaultInterval, now};
  }

private:
  // The hardware QPC frequency (ticks per second); invariant for the life of
  // the OS boot, so queried once.
  static int64_t qpcFrequency() {
    static const int64_t freq = []() {
      LARGE_INTEGER f{};
      QueryPerformanceFrequency(&f);
      return f.QuadPart > 0 ? f.QuadPart : 10000000LL;
    }();
    return freq;
  }

  // Converts a duration in QPC ticks into nanoseconds without 64-bit integer
  // overflow (splitting whole seconds and remainder ticks).
  static int64_t qpcTicksToNanos(uint64_t ticks) {
    const int64_t freq = qpcFrequency();
    const uint64_t seconds = ticks / static_cast<uint64_t>(freq);
    const uint64_t remainder = ticks % static_cast<uint64_t>(freq);
    return static_cast<int64_t>(seconds * 1000000000ULL +
                                (remainder * 1000000000ULL) /
                                    static_cast<uint64_t>(freq));
  }

  // Translates an absolute QPC tick timestamp (from DWM_TIMING_INFO::qpcVBlank)
  // onto rnsurface::frameClockNow()'s nanosecond clock by measuring the signed
  // QPC delta against the current QueryPerformanceCounter value.
  static int64_t qpcToSteadyNanos(uint64_t targetQpc, int64_t steadyNow) {
    LARGE_INTEGER currentQpc{};
    if (!QueryPerformanceCounter(&currentQpc) || currentQpc.QuadPart <= 0) {
      return 0;
    }
    const int64_t deltaTicks =
        static_cast<int64_t>(targetQpc) - currentQpc.QuadPart;
    const int64_t freq = qpcFrequency();
    const int64_t deltaNanos = (deltaTicks * 1000000000LL) / freq;
    return steadyNow + deltaNanos;
  }

  // Reads the display's refresh period in nanoseconds: first from DWM's
  // composition timing info (which reflects high-refresh-rate monitors such as
  // 120 Hz, 144 Hz, or 240 Hz), falling back to EnumDisplaySettingsW, and
  // clamped to sane display rates [20 Hz, 250 Hz].
  static int64_t queryDisplayIntervalNanos() {
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) &&
        timing.qpcRefreshPeriod > 0) {
      const int64_t period = qpcTicksToNanos(timing.qpcRefreshPeriod);
      if (period >= kShortestInterval && period <= kLongestInterval) {
        return period;
      }
    }

    DEVMODEW devMode{};
    devMode.dmSize = sizeof(devMode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &devMode) != 0 &&
        devMode.dmDisplayFrequency > 1) {
      const int64_t period =
          1000000000LL / static_cast<int64_t>(devMode.dmDisplayFrequency);
      if (period >= kShortestInterval && period <= kLongestInterval) {
        return period;
      }
    }

    return kDefaultInterval;
  }

  // 60 Hz until the display says otherwise; the rates a display runs at
  // are between 250 Hz (4 ms) and 20 Hz (50 ms), matching Android's bounds in
  // RNSkAnimationFrame.h.
  static constexpr int64_t kDefaultInterval = 16666667;
  static constexpr int64_t kShortestInterval = 4000000;
  static constexpr int64_t kLongestInterval = 50000000;
};

} // namespace RNSkia
