#pragma once

namespace nanobook {
class Core {
public:
    void initialize();
    [[nodiscard]] int value() const {
        return test_;
    }

private:
    int test_{0};
};
} // namespace nanobook