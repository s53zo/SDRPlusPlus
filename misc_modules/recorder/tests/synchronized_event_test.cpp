#include <utils/synchronized_event.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

// The callback gate fixes the interleaving; timeout checks only bound test waits.
struct Gate {
    std::promise<void> entered, release;
    std::shared_future<void> released = release.get_future().share();
    void wait() { entered.set_value(); released.wait(); }
};

void concurrentUnbind(bool selfUnbind) {
    SynchronizedEvent<int> event;
    struct Context {
        SynchronizedEvent<int>* event;
        EventHandler<int>* handler;
        Gate gate;
        std::atomic<int> calls{0};
        bool selfUnbind;
    } ctx;
    ctx.event = &event;
    ctx.selfUnbind = selfUnbind;
    EventHandler<int> handler([](int, void* data) {
        auto& c = *static_cast<Context*>(data);
        ++c.calls;
        if (c.selfUnbind) { c.event->unbindHandler(c.handler); }
        c.gate.wait();
    }, &ctx);
    ctx.handler = &handler;
    event.bindHandler(&handler);
    std::thread emitter([&] { event.emit(1); });
    assert(ctx.gate.entered.get_future().wait_for(2s) == std::future_status::ready);
    std::promise<void> unbindStarted;
    auto unbinder = std::async(std::launch::async, [&] {
        unbindStarted.set_value();
        event.unbindHandler(&handler);
    });
    unbindStarted.get_future().wait();
    assert(unbinder.wait_for(100ms) == std::future_status::timeout);

    // Registry operations remain available while the callback/unbind is blocked.
    int unrelatedCalls = 0;
    EventHandler<int> unrelated([](int, void* p) { ++*static_cast<int*>(p); }, &unrelatedCalls);
    auto registryCheck = std::async(std::launch::async, [&] {
        event.bindHandler(&unrelated);
        event.unbindHandler(&unrelated);
    });
    assert(registryCheck.wait_for(2s) == std::future_status::ready);
    registryCheck.get();

    ctx.gate.release.set_value();
    emitter.join();
    assert(unbinder.wait_for(2s) == std::future_status::ready);
    unbinder.get();
    event.emit(2);
    assert(ctx.calls == 1);
    event.unbindHandler(&handler); // Repeated unbind is harmless.
}

void staleSnapshot() {
    SynchronizedEvent<int> event;
    Gate gate;
    EventHandler<int> first([](int, void* p) { static_cast<Gate*>(p)->wait(); }, &gate);
    int calls = 0;
    EventHandler<int> second([](int, void* p) { ++*static_cast<int*>(p); }, &calls);
    event.bindHandler(&first);
    event.bindHandler(&second);
    std::thread emitter([&] { event.emit(0); });
    assert(gate.entered.get_future().wait_for(2s) == std::future_status::ready);
    event.unbindHandler(&second); // Already snapshotted, but not entered.
    gate.release.set_value();
    emitter.join();
    event.unbindHandler(&first);
    event.emit(0);
    assert(calls == 0);
}

void recursiveSelfUnbind() {
    SynchronizedEvent<int> event;
    struct Context {
        SynchronizedEvent<int>* event;
        EventHandler<int>* handler;
        int calls = 0;
    } ctx{&event, nullptr};
    EventHandler<int> handler([](int value, void* p) {
        auto& c = *static_cast<Context*>(p);
        ++c.calls;
        if (!value) { c.event->emit(1); }
        else { c.event->unbindHandler(c.handler); }
        c.event->emit(2); // Must skip the self-unbound slot in both frames.
    }, &ctx);
    ctx.handler = &handler;
    event.bindHandler(&handler);
    event.bindHandler(&handler);
    event.emit(0);
    event.emit(0);
    assert(ctx.calls == 2);
    event.bindHandler(&handler); // Rebind after callback drain.
    event.emit(0);
    assert(ctx.calls == 4);
}

void throwingSelfUnbind() {
    SynchronizedEvent<int> event;
    EventHandler<int> handler;
    struct Context { SynchronizedEvent<int>* event; EventHandler<int>* handler; } ctx{&event, &handler};
    handler = {[](int, void* p) {
        auto& c = *static_cast<Context*>(p);
        c.event->unbindHandler(c.handler);
        throw std::runtime_error("callback failure");
    }, &ctx};
    event.bindHandler(&handler);
    bool caught = false;
    try { event.emit(0); }
    catch (const std::runtime_error&) { caught = true; }
    assert(caught);
    event.emit(0);
    event.unbindHandler(&handler);
}

int main() {
    concurrentUnbind(false);
    concurrentUnbind(true);
    staleSnapshot();
    recursiveSelfUnbind();
    throwingSelfUnbind();
    std::cout << "PASS synchronized event draining, snapshots, self-unbind, rebind, exceptions\n";
}
