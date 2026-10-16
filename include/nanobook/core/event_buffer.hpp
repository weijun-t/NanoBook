#pragma once

#include <cassert>
#include <cstddef>
#include <span>
#include <vector>

#include "nanobook/core/messages.hpp"

namespace nanobook {

// Fixed-capacity list of events, owned by the caller and reused for every input (design §12).
// All memory is allocated once, in the constructor. push() never grows it: overflowing is a bug,
// caught by assert in Debug builds. Size it with BookConfig::max_events_per_input().
class EventBuffer {
public:
    explicit EventBuffer(std::size_t capacity) : events_(capacity) {
    }

    void clear() {
        size_ = 0;
    }

    void push(const Event &event) {
        assert(size_ < events_.size() && "EventBuffer overflow: capacity too small");
        events_[size_++] = event;
    }

    [[nodiscard]] std::span<const Event> view() const {
        return {events_.data(), size_};
    }
    [[nodiscard]] std::size_t size() const {
        return size_;
    }
    [[nodiscard]] std::size_t capacity() const {
        return events_.size();
    }

private:
    std::vector<Event> events_; // sized once; never resized
    std::size_t size_{0};       // number of events written for the current input
};

} // namespace nanobook
