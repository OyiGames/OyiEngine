#include <oyi_platform/Clock.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace oyi::platform {

TimePoint Now() noexcept {
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return TimePoint{.mNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(since).count()};
}

double SecondsBetween(TimePoint from, TimePoint to) noexcept {
    return static_cast<double>(to.mNanoseconds - from.mNanoseconds) * 1e-9;
}

int64_t UtcMilliseconds() noexcept {
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
}

uint32_t FixedStepper::Advance(double elapsedSeconds) noexcept {
    if (elapsedSeconds > 0.0) {
        mAccumulator += elapsedSeconds;
    }
    auto steps = static_cast<uint64_t>(std::floor(mAccumulator / mConfig.mStepSeconds));
    if (steps > mConfig.mMaxStepsPerFrame) {
        mDropped += steps - mConfig.mMaxStepsPerFrame;
        steps = mConfig.mMaxStepsPerFrame;
        // The surplus is gone for good; keep only the fraction of a step for interpolation.
        mAccumulator = std::fmod(mAccumulator, mConfig.mStepSeconds);
    } else {
        mAccumulator -= static_cast<double>(steps) * mConfig.mStepSeconds;
    }
    mAccumulator = std::max(mAccumulator, 0.0);
    mTick += steps;
    return static_cast<uint32_t>(steps);
}

} // namespace oyi::platform
