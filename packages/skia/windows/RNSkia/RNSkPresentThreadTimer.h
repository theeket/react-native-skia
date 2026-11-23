#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

#include "RNSkPresentThread.h"
#include "rnsurface/ThreadChecker.h"

namespace RNSkia {

/**
 * Runs tasks on the present thread once their delay has passed: a Windows
 * threadpool timer (PTP_TIMER) armed for the earliest due task, which posts a
 * drain of all due tasks to the present thread (see rnsurface::RenderThread):
 * the setTimeout() of the present thread (see RNSkView::FrameHost::setTimeout
 * and android/cpp/rnskia-android/RNSkPresentThreadTimer.h).
 *
 * What a view polls the D3D12 queue with while its window has a present
 * deferred (as nothing announces the GPU being done with the previous frame's
 * work and a blocking wait would hold up every other canvas on the present
 * thread, see DawnContext::isDone and RNSkPacingPolicy.h), what it waits for
 * the end of a compositor slot with, and what AnimationFrameSource arms for the
 * next projected VBlank (see RNSkAnimationFrame.h). One for the process, as the
 * present thread is, created on that thread at its first timeout.
 */
class PresentThreadTimer {
public:
  static PresentThreadTimer &getInstance() {
    // Never destroyed, like the present thread it posts to.
    static auto *instance = new PresentThreadTimer();
    return *instance;
  }

  /**
   * Runs the task once `delay` has passed, after the tasks due before it and
   * never on the spot. A timeout set cannot be cleared. Present thread.
   */
  void setTimeout(std::chrono::nanoseconds delay, std::function<void()> task) {
    RNSURFACE_ASSERT_THREAD(presentThread().checker(),
                            "PresentThreadTimer::setTimeout");
    _tasks.emplace(Clock::now() + delay, std::move(task));
    arm();
  }

private:
  using Clock = std::chrono::steady_clock;

  PresentThreadTimer() {
    // A single threadpool timer re-armed for whichever task in _tasks is due
    // first. When it expires on a Windows threadpool worker, onTimerFired()
    // posts runDue() to the present thread so that _tasks is only ever mutated
    // on the present thread, without a lock of its own.
    _tpTimer = CreateThreadpoolTimer(
        [](PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) {
          static_cast<PresentThreadTimer *>(context)->onTimerFired();
        },
        this, nullptr);
  }

  void onTimerFired() {
    presentThread().post([this]() { runDue(); });
  }

  // Present thread: runs every task whose deadline has arrived, in order, and
  // re-arms the timer for the next remaining task, if any.
  void runDue() {
    const auto now = Clock::now();
    while (!_tasks.empty() && _tasks.begin()->first <= now) {
      auto task = std::move(_tasks.begin()->second);
      _tasks.erase(_tasks.begin());
      task();
    }
    arm();
  }

  // Arms SetThreadpoolTimer for the earliest deadline in _tasks, or disarms it
  // when no tasks remain. SetThreadpoolTimer takes a FILETIME in 100-nanosecond
  // ticks: a negative value is a relative delay from now, and a zero
  // msWindowLength requests no coalescing window so the timer wakes promptly.
  // A task already due waits 100 ns (1 tick) so it never runs on the spot.
  void arm() {
    if (_tpTimer == nullptr) {
      return;
    }
    if (_tasks.empty()) {
      SetThreadpoolTimer(_tpTimer, nullptr, 0, 0);
      return;
    }
    const auto wait = std::max<std::chrono::nanoseconds>(
        _tasks.begin()->first - Clock::now(), std::chrono::nanoseconds(100));
    const int64_t ticks100ns = -std::max<int64_t>(1, wait.count() / 100);
    ULARGE_INTEGER due{};
    due.QuadPart = static_cast<ULONGLONG>(ticks100ns);
    FILETIME ft{};
    ft.dwLowDateTime = due.LowPart;
    ft.dwHighDateTime = due.HighPart;
    SetThreadpoolTimer(_tpTimer, &ft, 0, 0);
  }

  std::multimap<Clock::time_point, std::function<void()>> _tasks;
  PTP_TIMER _tpTimer = nullptr;
};

} // namespace RNSkia
