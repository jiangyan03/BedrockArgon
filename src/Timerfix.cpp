#include "Timerfix.h"

#include <HookAPI.h>
#include <LoggerAPI.h>
#include <MC/Timer.hpp>

namespace TimerFix {
namespace {

using GetTimeCallback = std::function<__int64(void)>;
using AdvanceTimeFn = void(__fastcall *)(Timer *, float);

static AdvanceTimeFn origAdvanceTime = nullptr;
static std::unordered_map<Timer *, double> gLastTimeSeconds;

constexpr uintptr_t OFF_TIME_SCALE = 0x00;
constexpr uintptr_t OFF_TICKS = 0x04;
constexpr uintptr_t OFF_ALPHA = 0x08;
constexpr uintptr_t OFF_TICKS_PER_SECOND = 0x0C;
constexpr uintptr_t OFF_PASSED_TIME = 0x10;
constexpr uintptr_t OFF_FRAME_STEP_ALIGNMENT_REMAINDER = 0x14;
constexpr uintptr_t OFF_LAST_TIME_SECONDS = 0x18;
constexpr uintptr_t OFF_LAST_TIMESTEP = 0x1C;
constexpr uintptr_t OFF_LAST_MS = 0x20;
constexpr uintptr_t OFF_LAST_MS_SYS_TIME = 0x28;
constexpr uintptr_t OFF_ADJUST_TIME = 0x30;
constexpr uintptr_t OFF_STEPPING_TICK = 0x34;
constexpr uintptr_t OFF_GET_TIME_MS_CALLBACK = 0x38;

template <typename T> inline T &field(Timer *timer, uintptr_t offset) {
  return dAccess<T>(timer, offset);
}

inline float clampFloat(float value, float low, float high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

static void __fastcall hookedAdvanceTime(Timer *timer,
                                         float preferredFrameStep) {
  __try {
    int &steppingTick = field<int>(timer, OFF_STEPPING_TICK);
    if (steppingTick < 0) {
      GetTimeCallback &getTimeMsCallback =
          field<GetTimeCallback>(timer, OFF_GET_TIME_MS_CALLBACK);
      if (!getTimeMsCallback) {
        origAdvanceTime(timer, preferredFrameStep);
        return;
      }

      const __int64 nowMs = getTimeMsCallback();
      __int64 passedMs = nowMs - field<__int64>(timer, OFF_LAST_MS);
      float adjustTime = field<float>(timer, OFF_ADJUST_TIME);

      if (passedMs > 1000) {
        __int64 passedMsSysTime =
            nowMs - field<__int64>(timer, OFF_LAST_MS_SYS_TIME);
        if (passedMsSysTime == 0) {
          passedMs = 1;
          passedMsSysTime = 1;
        }

        field<__int64>(timer, OFF_LAST_MS) = nowMs;
        field<__int64>(timer, OFF_LAST_MS_SYS_TIME) = nowMs;
        adjustTime = (((static_cast<float>(passedMs) /
                        static_cast<float>(passedMsSysTime)) -
                       adjustTime) *
                      0.2f) +
                     adjustTime;
        field<float>(timer, OFF_ADJUST_TIME) = adjustTime;
      }

      if (passedMs < 0) {
        field<__int64>(timer, OFF_LAST_MS) = nowMs;
        field<__int64>(timer, OFF_LAST_MS_SYS_TIME) = nowMs;
      }

      const double nowSeconds = static_cast<double>(nowMs) * 0.001;
      auto [it, inserted] = gLastTimeSeconds.emplace(
          timer,
          static_cast<double>(field<float>(timer, OFF_LAST_TIME_SECONDS)));
      if (inserted) {
        it->second = nowSeconds;
      }

      double passedSeconds =
          (nowSeconds - it->second) * static_cast<double>(adjustTime);
      it->second = nowSeconds;
      field<float>(timer, OFF_LAST_TIME_SECONDS) =
          static_cast<float>(nowSeconds);

      if (preferredFrameStep > 0.0f) {
        float &frameStepAlignmentRemainder =
            field<float>(timer, OFF_FRAME_STEP_ALIGNMENT_REMAINDER);
        const float oldFrameStepAlignmentRemainder =
            frameStepAlignmentRemainder;
        float newFrameStepAlignmentRemainder = preferredFrameStep * 4.0f;
        const float candidate =
            (preferredFrameStep - static_cast<float>(passedSeconds)) +
            oldFrameStepAlignmentRemainder;

        if (candidate <= newFrameStepAlignmentRemainder) {
          newFrameStepAlignmentRemainder = candidate > 0.0f ? candidate : 0.0f;
        }

        frameStepAlignmentRemainder = newFrameStepAlignmentRemainder;
        passedSeconds -= static_cast<double>(oldFrameStepAlignmentRemainder -
                                             newFrameStepAlignmentRemainder);
      }

      const float timestep =
          clampFloat(static_cast<float>(passedSeconds), 0.0f, 0.1f);
      field<float>(timer, OFF_LAST_TIMESTEP) = timestep;

      const float totalTicks =
          ((timestep * field<float>(timer, OFF_TICKS_PER_SECOND)) *
           field<float>(timer, OFF_TIME_SCALE)) +
          field<float>(timer, OFF_PASSED_TIME);
      int ticks = static_cast<int>(totalTicks);
      if (ticks > 10) {
        ticks = 10;
      }

      field<int>(timer, OFF_TICKS) = ticks;

      const float alpha =
          totalTicks - static_cast<float>(static_cast<int>(totalTicks));
      field<float>(timer, OFF_PASSED_TIME) = alpha;
      field<float>(timer, OFF_ALPHA) = alpha;
      return;
    }

    if (steppingTick > 0) {
      field<int>(timer, OFF_TICKS) = 1;
      --steppingTick;
    } else {
      *reinterpret_cast<unsigned long long *>(reinterpret_cast<char *>(timer) +
                                              OFF_TICKS) = 0ull;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    origAdvanceTime(timer, preferredFrameStep);
    logger.error("TimerFix failed inside Timer::advanceTime hook, falling back "
                 "to original logic.");
  }
}

} // namespace

bool installHook() {
  void *addr = dlsym_real("?advanceTime@Timer@@QEAAXM@Z");
  if (!addr) {
    logger.error("Failed to find Timer::advanceTime for TimerFix.");
    return false;
  }

  if (HookFunction(addr, reinterpret_cast<void **>(&origAdvanceTime),
                   reinterpret_cast<void *>(&hookedAdvanceTime)) != 0) {
    logger.error("Failed to hook Timer::advanceTime for TimerFix.");
    return false;
  }

  logger.info("TimerFix installed on Timer::advanceTime.");
  return true;
}

} // namespace TimerFix
