#pragma once

#include <cstdint>

namespace oyi::platform {

/// The three time axes of ADR-0014, measured here and budgeted elsewhere:
///   simulation time   = integer ticks of a fixed step (FixedStepper)
///   presentation time = a monotonic clock in seconds (Now / SecondsBetween)
///   persistent time   = UTC milliseconds (UtcMilliseconds)

/// A point on the monotonic clock; only differences mean anything.
struct TimePoint {
    int64_t mNanoseconds = 0;

    [[nodiscard]] constexpr bool operator==(const TimePoint&) const noexcept = default;
};

[[nodiscard]] TimePoint Now() noexcept;
[[nodiscard]] double SecondsBetween(TimePoint from, TimePoint to) noexcept;

/// Milliseconds since the Unix epoch on the wall clock, for lazy settlement of offline progress.
[[nodiscard]] int64_t UtcMilliseconds() noexcept;

/// Turns elapsed presentation time into fixed simulation steps (ADR-0014): at most
/// mMaxStepsPerFrame steps per frame, the rest is dropped so a slow frame cannot spiral.
class FixedStepper {
public:
    struct Config {
        double mStepSeconds        = 1.0 / 30.0;
        uint32_t mMaxStepsPerFrame = 4;
    };

    FixedStepper() noexcept : FixedStepper(Config{}) {}
    explicit FixedStepper(Config config) noexcept : mConfig(config) {}

    /// Adds `elapsedSeconds` (negative counts as zero) and returns how many steps to simulate now.
    [[nodiscard]] uint32_t Advance(double elapsedSeconds) noexcept;

    /// Fraction of the next step already elapsed, in [0, 1): the interpolation factor between the
    /// previous and the current simulation state.
    [[nodiscard]] double GetAlpha() const noexcept { return mAccumulator / mConfig.mStepSeconds; }
    [[nodiscard]] uint64_t GetTick() const noexcept { return mTick; }
    [[nodiscard]] uint64_t GetDroppedSteps() const noexcept { return mDropped; }
    [[nodiscard]] const Config& GetConfig() const noexcept { return mConfig; }

    void Reset() noexcept {
        mAccumulator = 0.0;
        mTick        = 0;
        mDropped     = 0;
    }

private:
    Config mConfig;
    double mAccumulator = 0.0;
    uint64_t mTick      = 0;
    uint64_t mDropped   = 0;
};

} // namespace oyi::platform
