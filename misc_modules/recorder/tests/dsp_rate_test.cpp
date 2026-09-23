// From the repository root:
// c++ -std=c++17 -O2 -Icore/src $(pkg-config --cflags volk) \
//   misc_modules/recorder/tests/dsp_rate_test.cpp -o /tmp/dsp_rate_test \
//   $(pkg-config --libs volk)
// Also run with -O1 -g -fsanitize=address,undefined instead of -O2.
#include <volk/volk.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <unordered_map>

// Track the DSP's allocations, including releases on configuration/destruction.
// The real VOLK allocator remains in use so sanitizers can check these buffers.
static std::unordered_map<void*, size_t> allocations;
static size_t liveBytes = 0;
static void* trackedAlloc(size_t bytes, size_t alignment) {
    void* ptr = volk_malloc(bytes, alignment);
    assert(ptr);
    assert(allocations.emplace(ptr, bytes).second);
    liveBytes += bytes;
    return ptr;
}
static void trackedFree(void* ptr) {
    if (!ptr) { return; }
    auto it = allocations.find(ptr);
    assert(it != allocations.end());
    liveBytes -= it->second;
    allocations.erase(it);
    volk_free(ptr);
}
#define volk_malloc trackedAlloc
#define volk_free trackedFree
#include <dsp/channel/rx_vfo.h>
#undef volk_malloc
#undef volk_free
#include <vector>

class InspectVFO : public dsp::channel::RxVFO {
public:
    void checkStreams(bool processOnly) {
        for (auto* stream : {&xlator.out, &resamp.out, &filter.out}) {
            assert((stream->writeBuf == nullptr) == processOnly);
            assert((stream->readBuf == nullptr) == processOnly);
        }
        assert(out.writeBuf);
        assert((out.readBuf == nullptr) == processOnly);
    }
};

static void checkRate(double inputRate, double expectedRate, int seconds, int chunk) {
    dsp::channel::RxVFO vfo;
    vfo.init(nullptr, inputRate, 1000, 300, 0);
    vfo.releaseUnusedProcessBuffers();
    assert(std::abs(vfo.getActualOutSamplerate() - expectedRate) < 1e-9);
    const long long total = std::llround(inputRate * seconds);
    std::vector<dsp::complex_t> input(chunk, {0, 0});
    long long produced = 0;
    for (long long pos = 0; pos < total; pos += chunk) {
        int count = int(std::min<long long>(chunk, total - pos));
        produced += vfo.process(count, input.data(), vfo.out.writeBuf);
    }
    // Cascaded decimators/polyphase scheduling can round the final block up.
    const double expected = total * expectedRate / inputRate;
    assert(std::abs(produced - expected) < 2);
    std::printf("PASS rate input=%.3f actual=%.9f samples=%lld expected=%.6f\n",
        inputRate, vfo.getActualOutSamplerate(), produced, expected);
}

static void checkReconfiguration() {
    InspectVFO vfo;
    vfo.init(nullptr, 2400000, 1000, 300, 0);
    vfo.releaseUnusedProcessBuffers();
    vfo.setInSamplerate(48000);
    assert(vfo.getActualOutSamplerate() == 1000);
    vfo.setOutSamplerate(2000, 500);
    assert(vfo.getActualOutSamplerate() == 2000);
    vfo.setBandwidth(300);
    vfo.setOffset(6000);
    vfo.reset();
    vfo.checkStreams(true);
    // Exercise the retained *full input-size* scratch allocation after reset.
    std::vector<dsp::complex_t> input(STREAM_BUFFER_SIZE, {0, 0});
    int count = vfo.process(input.size(), input.data(), vfo.out.writeBuf);
    assert(std::abs(count - STREAM_BUFFER_SIZE / 24.0) < 2);
    for (int i = 0; i < count; i++) {
        assert(vfo.out.writeBuf[i].re == 0 && vfo.out.writeBuf[i].im == 0);
    }
    bool rejected = false;
    try { vfo.start(); }
    catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    assert(vfo.run() == -1);

    // Check the RationalResampler setter independently as well.
    dsp::multirate::RationalResampler<dsp::complex_t> resampler(nullptr, 48000, 1000);
    resampler.setRates(2400000, 1000);
    assert(std::abs(resampler.getActualOutSamplerate() - 2400000.0 / 2048 * 250 / 293) < 1e-9);
}

static void checkGain(double bandwidth) {
    InspectVFO normal, lean;
    normal.init(nullptr, 48000, 1000, bandwidth, 6000);
    normal.checkStreams(false);
    lean.init(nullptr, 48000, 1000, bandwidth, 6000);
    lean.releaseUnusedProcessBuffers();
    lean.checkStreams(true);
    const int chunk = 997;
    std::vector<dsp::complex_t> input(chunk);
    int samples = 0, used = 0;
    double power = 0;
    for (int pos = 0; pos < 96000; pos += chunk) {
        int count = std::min(chunk, 96000 - pos);
        for (int i = 0; i < count; i++) {
            double phase = 2 * 3.141592653589793 * 6000 * (pos + i) / 48000;
            input[i] = {float(0.1 * std::cos(phase)), float(0.1 * std::sin(phase))};
        }
        int n = lean.process(count, input.data(), lean.out.writeBuf);
        assert(normal.process(count, input.data(), normal.out.writeBuf) == n);
        for (int i = 0; i < n; i++) {
            auto a = lean.out.writeBuf[i], b = normal.out.writeBuf[i];
            assert(std::abs(a.re - b.re) < 1e-7 && std::abs(a.im - b.im) < 1e-7);
            if (++samples <= 500) { continue; }
            power += double(a.re) * a.re + double(a.im) * a.im;
            used++;
        }
    }
    assert(used >= 1499);
    double db = 10 * std::log10(power / used);
    assert(std::abs(db + 20) < 0.3);
    std::printf("PASS gain bandwidth=%.0f measured=%.6f dB; normal/process-only agree\n", bandwidth, db);
}

static void checkMemory() {
    assert(liveBytes == 0);
    size_t normalBytes;
    {
        InspectVFO normal;
        normal.init(nullptr, 2400000, 1000, 300, 6000);
        normalBytes = liveBytes;
        normal.checkStreams(false);
    }
    assert(liveBytes == 0 && allocations.empty());
    for (int iteration = 0; iteration < 3; iteration++) {
        {
            InspectVFO lean;
            lean.init(nullptr, 2400000, 1000, 300, 6000);
            auto* scratch = lean.out.writeBuf;
            lean.releaseUnusedProcessBuffers();
            lean.releaseUnusedProcessBuffers();
            assert(lean.out.writeBuf == scratch);
            lean.checkStreams(true);
            // Three embedded read/write pairs plus this VFO's unused readBuf.
            size_t saved = 7 * STREAM_BUFFER_SIZE * sizeof(dsp::complex_t);
            assert(normalBytes - liveBytes == saved);
            std::printf("PASS memory normal=%zu process-only=%zu saved=%zu bytes\n",
                normalBytes, liveBytes, saved);
        }
        assert(liveBytes == 0 && allocations.empty());
    }
}

static void checkThreaded() {
    dsp::stream<dsp::complex_t> input;
    InspectVFO normal;
    normal.init(&input, 48000, 1000, 300, 0);
    normal.checkStreams(false);
    normal.start();
    bool rejected = false;
    try { normal.releaseUnusedProcessBuffers(); }
    catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    normal.checkStreams(false);
    for (int block = 0; block < 3; block++) {
        for (int i = 0; i < 4096; i++) { input.writeBuf[i] = {0, 0}; }
        assert(input.swap(4096));
        int count = normal.out.read();
        assert(count == 85 || count == 86);
        normal.out.flush();
    }
    normal.stop();
    normal.checkStreams(false);
}

int main() {
    checkMemory();
    checkRate(2400000, 2400000.0 / 2048 * 250 / 293, 30, 65536);
    checkRate(10000000, 10000000.0 / 8192 * 1000 / 1221, 10, 65536);
    checkRate(48000, 1000, 30, 997);
    checkRate(1000.4, 1000.4, 10, 997); // NONE: rounded rates equal
    checkRate(2000.4, 1000.2, 10, 997); // DECIM_ONLY
    checkRate(1500.25, 1500.25 * 2 / 3, 10, 997); // RESAMP_ONLY
    checkReconfiguration();
    checkGain(300);
    checkGain(500);
    checkThreaded();
    assert(liveBytes == 0 && allocations.empty());
    std::puts("PASS all DSP rate, gain, stream lifetime and threaded checks");
}
