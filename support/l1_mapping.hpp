/*
 * l1_mapping.hpp: puts an L1Table in named shared memory, for one writer and any number of readers.
 *
 * What it does: L1Mapping::create(name) makes a fresh table under /dev/shm/<name>, writable, for
 *   nanobook_pipeline; L1Mapping::open(name) maps an existing one read-only, for nanobook_view and
 *   the tests. The mapping is undone when the L1Mapping is destroyed. remove(name) deletes the
 *   name (a process that still maps the table keeps it until it lets go).
 *
 * Why it exists: Viewer design §3.2. The viewer is another process, so the table must live in
 *   memory both can map: POSIX shared memory, a file in RAM that is never written to disk.
 *
 * How it works:
 *   - create(): remove any table left by an earlier run, shm_open(O_CREAT | O_EXCL) a new one,
 *     ftruncate it to sizeof(L1Table), mmap it MAP_SHARED, then construct the table in it
 *     (placement new). Constructing writes every byte, so every page is touched now and the
 *     publisher never page-faults on it mid-run. Last, the magic number, with release: a reader
 *     that sees it sees a constructed table. Cold path: system calls and allocation are fine.
 *   - open(): shm_open(O_RDONLY), check the size is sizeof(L1Table) (another layout would be
 *     read as garbage), mmap PROT_READ, check the magic and version. A reader can't write: a
 *     store would crash, not corrupt the writer's table.
 *   - The file descriptor is closed right after mmap: the mapping keeps the memory alive.
 *
 * Trade-offs: Linux only. The table is not removed when the run ends, so a viewer started later
 *   still shows the last run's final state; the next create() replaces it, and remove() (or
 *   `rm /dev/shm/<name>`) frees its 4.6 MiB. A reader treats the mapped bytes as an L1Table that
 *   another process constructed: what every shared-memory design does, but C++ has no word for an
 *   object made by another process (std::start_lifetime_as, C++23, isn't in GCC 14 yet, and
 *   atomics aren't implicit-lifetime types). A reader that opens the name in the moment between
 *   ftruncate and the placement new reads zeros (magic not set yet), and is refused.
 */

#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "l1_table.hpp"

namespace nanobook::view {

class L1Mapping {
public:
    // A new, empty table (state Starting), replacing any earlier one of the same name.
    [[nodiscard]] static std::expected<L1Mapping, std::string> create(std::string_view name) {
        const std::string path = shm_path(name);
        remove(name);
        const int fd = shm_open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
        if (fd < 0) {
            return failed("shm_open", path);
        }
        if (ftruncate(fd, static_cast<off_t>(sizeof(L1Table))) != 0) {
            auto error = failed("ftruncate", path);
            close(fd);
            return error;
        }
        void *memory = mmap(nullptr, sizeof(L1Table), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (memory == MAP_FAILED) {
            auto error = failed("mmap", path); // before close(), which could change errno
            close(fd);
            return error;
        }
        close(fd);                            // the mapping keeps the memory
        auto *table = new (memory) L1Table{}; // writes every byte: every page touched up front
        table->header.version.store(kLayoutVersion, std::memory_order_relaxed);
        table->header.run_id.store(
            static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()),
            std::memory_order_relaxed);
        table->header.magic.store(kMagic, std::memory_order_release); // the table is ready
        return L1Mapping{table, true};
    }

    // An existing table, read-only.
    [[nodiscard]] static std::expected<L1Mapping, std::string> open(std::string_view name) {
        const std::string path = shm_path(name);
        const int fd = shm_open(path.c_str(), O_RDONLY, 0);
        if (fd < 0) {
            return failed("shm_open", path);
        }
        struct stat info{};
        if (fstat(fd, &info) != 0) {
            auto error = failed("fstat", path);
            close(fd);
            return error;
        }
        if (static_cast<std::size_t>(info.st_size) != sizeof(L1Table)) {
            close(fd);
            return std::unexpected(path + ": not an L1 table of this layout (size " +
                                   std::to_string(info.st_size) + ")");
        }
        void *memory = mmap(nullptr, sizeof(L1Table), PROT_READ, MAP_SHARED, fd, 0);
        if (memory == MAP_FAILED) {
            auto error = failed("mmap", path);
            close(fd);
            return error;
        }
        close(fd);
        L1Mapping mapping{static_cast<L1Table *>(memory), false};
        const Header &h = mapping.table().header;
        if (h.magic.load(std::memory_order_acquire) != kMagic ||
            h.version.load(std::memory_order_relaxed) != kLayoutVersion) {
            return std::unexpected(path + ": not ready, or another layout version");
        }
        return mapping;
    }

    // Deletes the name; true if there was one.
    static bool remove(std::string_view name) {
        return shm_unlink(shm_path(name).c_str()) == 0;
    }

    L1Mapping(const L1Mapping &) = delete;
    L1Mapping &operator=(const L1Mapping &) = delete;
    L1Mapping(L1Mapping &&other) noexcept
        : table_(std::exchange(other.table_, nullptr)), writable_(other.writable_) {
    }
    L1Mapping &operator=(L1Mapping &&other) noexcept {
        if (this != &other) {
            unmap();
            table_ = std::exchange(other.table_, nullptr);
            writable_ = other.writable_;
        }
        return *this;
    }
    ~L1Mapping() {
        unmap();
    }

    [[nodiscard]] const L1Table &table() const noexcept {
        return *table_;
    }
    // Only a mapping from create() may be written; nullptr for one from open().
    [[nodiscard]] L1Table *writable() noexcept {
        return writable_ ? table_ : nullptr;
    }

private:
    L1Mapping(L1Table *table, bool writable) : table_(table), writable_(writable) {
    }

    void unmap() noexcept {
        if (table_ != nullptr) {
            munmap(table_, sizeof(L1Table));
            table_ = nullptr;
        }
    }

    // shm_open wants "/name"; a name given without the slash gets one.
    static std::string shm_path(std::string_view name) {
        return name.starts_with('/') ? std::string{name} : "/" + std::string{name};
    }
    static std::unexpected<std::string> failed(const char *call, const std::string &path) {
        return std::unexpected(path + ": " + call + " failed: " + std::strerror(errno));
    }

    L1Table *table_{nullptr};
    bool writable_{false};
};

} // namespace nanobook::view
