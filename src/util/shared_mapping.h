#pragma once

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <fmt/core.h>

#include <util/expected.h>

namespace util {

template <class T>
concept trivial = std::is_trivial_v<T>;

// A POSIX shared memory object holding a single T, mapped into this process.
//
// Closing and unlinking are separate operations: every peer closes its own
// mapping when the object is destroyed, but only the last peer out should
// unlink the name, so unlink() is explicit rather than automatic.
template <trivial T> class shared_mapping {
  public:
    shared_mapping() = default;

    shared_mapping(const shared_mapping&) = delete;
    shared_mapping& operator=(const shared_mapping&) = delete;

    shared_mapping(shared_mapping&& other) noexcept
        : name_(std::move(other.name_)), data_(other.data_) {
        other.data_ = nullptr;
    }

    shared_mapping& operator=(shared_mapping&& other) noexcept {
        if (this != &other) {
            release();
            name_ = std::move(other.name_);
            data_ = other.data_;
            other.data_ = nullptr;
        }
        return *this;
    }

    ~shared_mapping() {
        release();
    }

    // create the shared memory object.
    // Returns nullopt, if another peer created it first; that race is how a
    // leader can be elected among peers racing to create the same name.
    static util::expected<std::optional<shared_mapping>, std::string>
    create_exclusive(std::string name) {
        int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) {
            if (errno == EEXIST) {
                return std::optional<shared_mapping>{};
            }
            return util::unexpected(fmt::format("unable to create shm {}: {}",
                                                name, strerror(errno)));
        }
        if (ftruncate(fd, sizeof(T)) != 0) {
            std::string err{strerror(errno)};
            close(fd);
            return util::unexpected(
                fmt::format("unable to size shm {}: {}", name, err));
        }
        auto mapped = map(std::move(name), fd);
        if (!mapped) {
            return util::unexpected(mapped.error());
        }
        return std::optional<shared_mapping>{std::move(*mapped)};
    }

    // open a shared memory object created by another peer.
    //
    // The size is checked before mapping: mmap() happily maps sizeof(T) bytes
    // of a segment shorter than that, and every access past the object's end
    // then raises SIGBUS. A short segment under this name is never something
    // this peer created, so refuse it rather than crash on first touch.
    static util::expected<shared_mapping, std::string>
    open_existing(std::string name) {
        int fd = shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0) {
            return util::unexpected(fmt::format("unable to open shm {}: {}",
                                                name, strerror(errno)));
        }
        struct stat info;
        if (fstat(fd, &info) != 0) {
            std::string err{strerror(errno)};
            close(fd);
            return util::unexpected(
                fmt::format("unable to stat shm {}: {}", name, err));
        }
        if (info.st_size < static_cast<off_t>(sizeof(T))) {
            close(fd);
            return util::unexpected(fmt::format(
                "shm {} is {} bytes, too small to hold the {} bytes expected: "
                "an unrelated or truncated object is using this name",
                name, static_cast<long long>(info.st_size), sizeof(T)));
        }
        return map(std::move(name), fd);
    }

    const std::string& name() const {
        return name_;
    }

    T* operator->() const {
        return data_;
    }

    util::expected<void, std::string> unlink() {
        if (shm_unlink(name_.c_str()) != 0) {
            return util::unexpected(fmt::format("unable to unlink shm {}: {}",
                                                name_, strerror(errno)));
        }
        return {};
    }

  private:
    shared_mapping(std::string name, T* data)
        : name_(std::move(name)), data_(data) {
    }

    // map the whole object and close fd, which the mapping does not need to
    // stay alive. Consumes fd on every path.
    static util::expected<shared_mapping, std::string> map(std::string name,
                                                           int fd) {
        void* p =
            mmap(nullptr, sizeof(T), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        std::string err{strerror(errno)};
        close(fd);
        if (p == MAP_FAILED) {
            return util::unexpected(
                fmt::format("unable to map shm {}: {}", name, err));
        }
        return shared_mapping{std::move(name), static_cast<T*>(p)};
    }

    void release() {
        if (data_ != nullptr) {
            munmap(data_, sizeof(T));
            data_ = nullptr;
        }
    }

    std::string name_;
    T* data_ = nullptr;
};

} // namespace util
