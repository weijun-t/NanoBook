/*
 * mapped_file.hpp: a file mapped read-only into memory, unmapped when the object goes away.
 *
 * What it does: MappedFile maps a whole file (an ITCH day) with mmap and gives it as a
 *   std::span<const std::byte>. An empty span means the file couldn't be opened or mapped.
 *
 * Why it exists: The replay and pipeline tools both read an 11 GB day this way, and the timing
 *   code (itch_timing.hpp) relies on it being a read-only FILE mapping: releasing its pages with
 *   MADV_DONTNEED is then safe, because they come back from the file when touched again.
 *
 * How it works: open, fstat for the size, mmap(PROT_READ, MAP_PRIVATE), close the descriptor
 *   (the mapping keeps the file open), madvise(MADV_SEQUENTIAL) so the kernel reads ahead.
 *   Nothing is read until a page is touched.
 *
 * Trade-offs: Linux only. Tool code: not for the engine.
 */

#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <span>
#include <string>

namespace nanobook::support {

// The file, mapped read-only. Unmapped when it goes out of scope.
class MappedFile {
public:
    explicit MappedFile(const std::string &path) {
        const int fd = open(path.c_str(), O_RDONLY);
        struct stat st{};
        if (fd < 0 || fstat(fd, &st) != 0 || st.st_size == 0) {
            if (fd >= 0) {
                close(fd);
            }
            return;
        }
        size_ = static_cast<std::size_t>(st.st_size);
        void *map = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd); // the mapping keeps the file open
        if (map == MAP_FAILED) {
            size_ = 0;
            return;
        }
        madvise(map, size_, MADV_SEQUENTIAL); // read ahead, drop pages behind
        data_ = static_cast<const std::byte *>(map);
    }
    ~MappedFile() {
        if (data_ != nullptr) {
            munmap(const_cast<std::byte *>(data_), size_);
        }
    }
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;
    MappedFile(MappedFile &&) = delete;
    MappedFile &operator=(MappedFile &&) = delete;

    [[nodiscard]] std::span<const std::byte> bytes() const {
        return {data_, size_};
    }

private:
    const std::byte *data_{nullptr};
    std::size_t size_{0};
};

} // namespace nanobook::support
