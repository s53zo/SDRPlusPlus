#include <dsp/sink/handler_sink.h>
#include <cassert>
#include <future>
#include <iostream>

// Force stopReader to win over a queued, unconsumed block.
struct ControlledStream : dsp::stream<int> {
    std::promise<void> entered, stopped;
    std::shared_future<void> resume = stopped.get_future().share();
    bool gate = true, released = false;
    int read() override {
        if (gate) {
            gate = false;
            entered.set_value();
            resume.wait();
        }
        return dsp::stream<int>::read();
    }
    void stopReader() override {
        dsp::stream<int>::stopReader();
        if (!released) { released = true; stopped.set_value(); }
    }
};

int main() {
    ControlledStream stream;
    std::promise<int> observed;
    dsp::sink::Handler<int> sink(&stream, [](int* p, int n, void* ctx) {
        assert(n == 1);
        static_cast<std::promise<int>*>(ctx)->set_value(p[0]);
    }, &observed);
    sink.start();
    stream.entered.get_future().wait();
    stream.writeBuf[0] = 12345;
    assert(stream.swap(1));
    // Producer has detached, matching the recorder shutdown sequence.
    sink.stop();
    stream.flush();
    sink.start();
    auto result = observed.get_future();
    assert(result.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    stream.writeBuf[0] = 67890;
    assert(stream.swap(1));
    assert(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(result.get() == 67890);
    sink.stop();
    std::cout << "PASS: restart consumes only new IQ\n";
}
