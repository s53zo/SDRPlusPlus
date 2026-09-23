#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace recorder {
// Include a 50 Hz guard around the frontend DC-removal notch and FIR transitions.
inline bool overlapsDCNotch(double offset, double bandwidth) {
    return std::abs(offset) <= 0.55 * bandwidth + 50.0;
}

// Alternate integer sample counts to track 100 ms boundaries at the actual rate.
struct PowerClock {
    double rate;
    uint64_t windows = 0, end = 0;
    explicit PowerClock(double actualRate) : rate(actualRate) {}
    uint64_t nextWindowLength() {
        uint64_t next = std::llround(++windows * rate / 10.0);
        uint64_t length = next - end;
        end = next;
        return length;
    }
};

// Powers are relative to IQ amplitude 1.0, not calibrated dBm.
struct PowerWindow {
    std::array<double, 3> sums{};
    double peak = 0;
    uint64_t count = 0;
    uint64_t length;

    explicit PowerWindow(uint64_t samples) : length(samples) {}
    bool add(double signal, double left, double right) {
        sums[0] += signal;
        sums[1] += left;
        sums[2] += right;
        if (signal > peak) { peak = signal; }
        return ++count == length;
    }
    double mean(int channel) const { return sums[channel] / count; }
    void clear() { sums = {}; peak = 0; count = 0; }
    static double db(double power) { return 10.0 * std::log10(std::fmax(power, 1e-30)); }
};
}
