#include "../src/signal_power.h"
#include <dsp/channel/rx_vfo.h>
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

void check(double bw, int chunk, double sr = 48000) {
    const double outRate = 1000, center = 6000;
    std::array<std::unique_ptr<dsp::channel::RxVFO>, 3> filters;
    for (int c = 0; c < 3; c++) {
        filters[c] = std::make_unique<dsp::channel::RxVFO>(
            nullptr, sr, outRate, bw, center + (c == 1 ? -2 : c == 2 ? 2 : 0) * bw);
    }
    std::vector<dsp::complex_t> in(chunk);
    recorder::PowerWindow window(100);
    int total = 0, windows = 0;
    double smallest = 1e9, largest = -1e9;
    for (int start = 0; start < int(2 * sr); start += chunk) {
        int count = std::min(chunk, int(2 * sr) - start);
        for (int i = 0; i < count; i++) {
            double re = 0, im = 0;
            for (int c = 0; c < 3; c++) {
                double freq = center + (c == 1 ? -2 : c == 2 ? 2 : 0) * bw;
                double phase = 2 * 3.141592653589793 * freq * (start + i) / sr;
                double amp = c ? 0.02 : 0.1;
                re += amp * std::cos(phase);
                im += amp * std::sin(phase);
            }
            in[i] = {float(re), float(im)};
        }
        int n = 0;
        for (int c = 0; c < 3; c++) {
            int produced = filters[c]->process(count, in.data(), filters[c]->out.writeBuf);
            if (c) { assert(produced == n); }
            n = produced;
        }
        for (int i = 0; i < n; i++) {
            if (++total <= 500) { continue; }
            double p[3];
            for (int c = 0; c < 3; c++) {
                auto z = filters[c]->out.writeBuf[i];
                p[c] = double(z.re)*z.re + double(z.im)*z.im;
            }
            if (window.add(p[0], p[1], p[2])) {
                // Existing resampler/filter gain is close to, but not exactly, unity.
                assert(std::abs(recorder::PowerWindow::db(window.mean(0)) + 20) < 0.3);
                smallest = std::min(smallest, recorder::PowerWindow::db(window.mean(0)));
                largest = std::max(largest, recorder::PowerWindow::db(window.mean(0)));
                for (int c = 1; c < 3; c++) {
                    assert(std::abs(recorder::PowerWindow::db(window.mean(c)) - recorder::PowerWindow::db(0.0004)) < 0.3);
                }
                windows++;
                window.clear();
            }
        }
    }
    assert(windows >= 14);
    assert(largest - smallest < 0.05);
    std::cout << "PASS bandwidth=" << bw << " chunk=" << chunk << " windows=" << windows << "\n";
}

int main() {
    for (double rate : {999.893344709898, 999.756859385}) {
        recorder::PowerClock clock(rate);
        uint64_t samples = 0;
        for (int i = 1; i <= 36000; i++) {
            samples += clock.nextWindowLength();
            assert(std::abs(samples - i * rate / 10) <= 0.500001);
        }
        assert(std::abs(samples / rate - 3600) < 0.001);
    }
    for (double bw : {300.0, 500.0}) {
        assert(recorder::overlapsDCNotch(0, bw));
        assert(recorder::overlapsDCNotch(10, bw));
        for (int c = -1; c <= 1; c++) {
            assert(!recorder::overlapsDCNotch(2500 + c * 2 * bw, bw));
        }
    }
    recorder::PowerWindow w(100);
    // Half zero, half unit power must average to -3.01 dB, not mean(log(power)).
    for (int i = 0; i < 100; i++) { assert(w.add(i < 50 ? 0 : 1, 0.1, 0.1) == (i == 99)); }
    assert(std::abs(recorder::PowerWindow::db(w.mean(0)) + 3.01029995664) < 1e-8);
    assert(w.peak == 1);
    w.clear();
    assert(w.count == 0 && w.peak == 0 && w.sums[0] == 0);
    check(300, 4096);
    check(300, 997);
    check(500, 4096);
    check(500, 997);
    check(300, 65536, 2400000);
}
