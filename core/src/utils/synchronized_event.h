#pragma once
#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>
#include "event.h"

// Event-compatible synchronous dispatch with draining unsubscription.
//
// Callbacks for one subscription are serialized (same-thread reentry is supported).
// Different subscriptions may execute concurrently. Never hold a lock needed
// by a callback while externally unbinding it. Cross-unbinding or recursive
// dispatch involving other slots on different threads can likewise deadlock;
// marshal such operations to an owner thread. Self-unbind is allowed: it prevents
// subsequent entry, but the current
// callback (and any outer recursive invocation) must still return normally.
//
// Keep handler/context alive until unbind returns, or until all current callback
// frames return when self-unbinding. Do not mutate a bound EventHandler; function
// and context are captured at bind time. Duplicate binds are idempotent. A bind
// racing with unbind may observe the existing subscription and do nothing; wait
// for unbind to finish before rebinding. The event itself must outlive all calls.
template <class T>
class SynchronizedEvent {
public:
    void bindHandler(EventHandler<T>* handler) {
        std::lock_guard<std::mutex> lock(registryMutex);
        if (findSlot(handler) != slots.end()) { return; }
        slots.push_back(std::make_shared<Slot>(handler));
    }

    void unbindHandler(EventHandler<T>* handler) {
        std::shared_ptr<Slot> slot;
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            auto it = findSlot(handler);
            if (it == slots.end()) { return; }
            slot = *it;
        }
        // Never wait for a callback while holding the registry mutex.
        std::lock_guard<std::recursive_mutex> lock(slot->mutex);
        slot->active = false;
        retireIfInactive(slot);
    }

    void emit(T value) {
        std::vector<std::shared_ptr<Slot>> snapshot;
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            snapshot = slots;
        }
        for (const auto& slot : snapshot) {
            std::lock_guard<std::recursive_mutex> lock(slot->mutex);
            if (!slot->active) { continue; }
            ++slot->depth;
            CallbackGuard guard{*this, slot};
            slot->callback(value, slot->context);
        }
    }

private:
    struct Slot {
        explicit Slot(EventHandler<T>* handler)
            : identity(handler), callback(handler->handler), context(handler->ctx) {}
        EventHandler<T>* identity;
        void (*callback)(T, void*);
        void* context;
        std::recursive_mutex mutex;
        bool active = true;
        unsigned depth = 0;
    };

    struct CallbackGuard {
        SynchronizedEvent& owner;
        std::shared_ptr<Slot> slot;
        ~CallbackGuard() {
            --slot->depth;
            owner.retireIfInactive(slot);
        }
    };

    auto findSlot(EventHandler<T>* handler) {
        return std::find_if(slots.begin(), slots.end(),
            [handler](const auto& slot) { return slot->identity == handler; });
    }

    // Called with the slot mutex held. Retain a self-unbound slot until its
    // outermost callback returns, so an external unbind still drains that frame.
    void retireIfInactive(const std::shared_ptr<Slot>& slot) {
        if (slot->active || slot->depth) { return; }
        std::lock_guard<std::mutex> lock(registryMutex);
        slots.erase(std::remove(slots.begin(), slots.end(), slot), slots.end());
    }

    std::mutex registryMutex;
    std::vector<std::shared_ptr<Slot>> slots;
};
