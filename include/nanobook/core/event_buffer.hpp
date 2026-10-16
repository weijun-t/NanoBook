/*
 * event_buffer.hpp: a fixed-size list the engine writes its events into.
 *
 * What it does: The caller owns one EventBuffer and passes it to every handle() call. The book
 *   clears it, then appends that input's events.
 *
 * Why it exists: A std::vector that grows would allocate memory while trading (rule 1). This
 *   buffer allocates once, in its constructor, and never again.
 *
 * How it works: A vector sized once to `capacity`, plus a count of events written. push() writes at
 *   the count and adds 1, and clear() sets the count back to 0. view() returns a std::span (a
 *   pointer and a length), so reading the events copies nothing.
 *
 * Data structures: std::vector<Event> (never resized) and a std::size_t count.
 *
 * Trade-offs: The caller must size it for the worst case, BookConfig::max_events_per_input()
 *   (max_orders + 2). Overflowing is a bug, caught by assert in Debug builds only.
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <span>
#include <vector>

#include "nanobook/core/messages.hpp"

namespace nanobook {

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
