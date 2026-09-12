#pragma once

#include <cstring>
#include <fcntl.h>
#include <semaphore.h>
#include <time.h>

#include <chrono>
#include <string>
#include <utility>

#include <fmt/core.h>

#include <util/expected.h>

namespace util {

// A POSIX named semaphore.
//
// Closing and unlinking are separate operations: every peer closes its own
// handle when it is destroyed, but only the last peer out should unlink the
// name, so unlink() is explicit rather than automatic.
class named_semaphore {
  public:
    named_semaphore() = default;

    named_semaphore(const named_semaphore&) = delete;
    named_semaphore& operator=(const named_semaphore&) = delete;

    named_semaphore(named_semaphore&& other) noexcept
        : name_(std::move(other.name_)), sem_(other.sem_) {
        other.sem_ = SEM_FAILED;
    }

    named_semaphore& operator=(named_semaphore&& other) noexcept {
        if (this != &other) {
            release();
            name_ = std::move(other.name_);
            sem_ = other.sem_;
            other.sem_ = SEM_FAILED;
        }
        return *this;
    }

    ~named_semaphore() {
        release();
    }

    // open (creating if required) the named semaphore, initialising it to
    // `value` if this call is the one that creates it.
    static util::expected<named_semaphore, std::string> open(std::string name,
                                                             unsigned value) {
        sem_t* sem = sem_open(name.c_str(), O_CREAT, 0600, value);
        if (sem == SEM_FAILED) {
            return util::unexpected(fmt::format(
                "unable to open semaphore {}: {}", name, strerror(errno)));
        }
        return named_semaphore{std::move(name), sem};
    }

    const std::string& name() const {
        return name_;
    }

    // wait for up to `timeout`.
    util::expected<void, std::string> wait(std::chrono::milliseconds timeout) {
        using namespace std::chrono;

        timespec deadline{};

        // sem_timedwait() requires an absolute deadline rather than a
        // relative timeout.
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
            return util::unexpected(
                fmt::format("clock_gettime failed: {}", strerror(errno)));
        }
        const auto total =
            seconds{deadline.tv_sec} + nanoseconds{deadline.tv_nsec} + timeout;
        const auto secs = duration_cast<seconds>(total);
        deadline.tv_sec = secs.count();
        deadline.tv_nsec = (total - secs).count();

        if (sem_timedwait(sem_, &deadline) != 0) {
            return util::unexpected(fmt::format(
                "failure waiting for barrier lock: {}", strerror(errno)));
        }
        return {};
    }

    util::expected<void, std::string> post() {
        if (sem_post(sem_) != 0) {
            return util::unexpected(
                fmt::format("sem_post failed: {}", strerror(errno)));
        }
        return {};
    }

    // remove the name from the system; existing handles stay usable.
    util::expected<void, std::string> unlink() {
        if (sem_unlink(name_.c_str()) != 0) {
            return util::unexpected(fmt::format(
                "unable to unlink semaphore {}: {}", name_, strerror(errno)));
        }
        return {};
    }

  private:
    named_semaphore(std::string name, sem_t* sem)
        : name_(std::move(name)), sem_(sem) {
    }

    void release() {
        if (sem_ != SEM_FAILED) {
            sem_close(sem_);
            sem_ = SEM_FAILED;
        }
    }

    std::string name_;
    sem_t* sem_ = SEM_FAILED;
};

} // namespace util
