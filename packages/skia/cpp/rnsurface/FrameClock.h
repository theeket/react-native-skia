#pragma once

#if defined(__APPLE__)
#include <time.h>
#elif defined(_WIN32)
#include <chrono>
#else
#include <time.h>
#endif

#include "rnsurface/FrameScheduler.h"

namespace rnsurface {

/**
 * The time, on the clock the animation frames of the platform are on: a
 * CADisplayLink's timestamps are mach absolute time, an AChoreographer's
 * frame times CLOCK_MONOTONIC, and Windows QPC (std::chrono::steady_clock).
 * The counterpart of the web's performance.now(), which is on the clock of the
 * timestamps requestAnimationFrame() hands out, but in nanoseconds and as an
 * integer, where performance.now() and React Native's own are milliseconds.
 * What a view measures its stages with and reads before each entry of its
 * FrameScheduler.
 */
inline FrameScheduler::Nanos frameClockNow() {
#if defined(__APPLE__)
  return static_cast<FrameScheduler::Nanos>(
      clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
#elif defined(_WIN32)
  return static_cast<FrameScheduler::Nanos>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
#else
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<FrameScheduler::Nanos>(now.tv_sec) * 1000000000LL +
         static_cast<FrameScheduler::Nanos>(now.tv_nsec);
#endif
}

} // namespace rnsurface
