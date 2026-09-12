#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <utility>

#include <catch2/catch_all.hpp>
#include <fmt/core.h>

#include <util/shared_mapping.h>

namespace {

struct payload {
    int value;
    bool child_saw_it;
};

} // namespace

// two independent handles onto the same name back onto the same physical
// pages: a write through one must be visible through the other.
TEST_CASE("shared_mapping: create_exclusive and open_existing share memory",
          "[shared_mapping]") {
    const auto name = fmt::format("/uenv-test-shm-roundtrip-{}", getpid());

    auto created = util::shared_mapping<payload>::create_exclusive(name);
    REQUIRE(created);
    REQUIRE(created->has_value());
    auto a = std::move(**created);

    auto opened = util::shared_mapping<payload>::open_existing(name);
    REQUIRE(opened);
    auto b = std::move(*opened);

    a->value = 42;
    REQUIRE(b->value == 42);
    b->value = 7;
    REQUIRE(a->value == 7);

    // unlink() acts on the *name*, not on b's mapping, so only one of the two
    // handles needs to call it -- see the dedicated test below for what
    // happens if the other one calls it too.
    REQUIRE(a.unlink());
}

// unlink() removes the one name both handles share; it is not tied to
// either mapping. A second unlink() through a different handle onto the
// same (now-removed) name must fail rather than succeed silently or unlink
// something else -- there is nothing left to remove.
TEST_CASE("shared_mapping: unlinking twice via different handles fails the "
          "second time",
          "[shared_mapping]") {
    const auto name = fmt::format("/uenv-test-shm-double-unlink-{}", getpid());

    auto created = util::shared_mapping<payload>::create_exclusive(name);
    REQUIRE(created);
    REQUIRE(created->has_value());
    auto a = std::move(**created);

    auto opened = util::shared_mapping<payload>::open_existing(name);
    REQUIRE(opened);
    auto b = std::move(*opened);

    REQUIRE(a.unlink());
    REQUIRE(!b.unlink());
}

// create_exclusive() is how proc_barrier elects a leader: whoever loses the
// EEXIST race must see that as a successful (empty) result, not an error.
TEST_CASE("shared_mapping: create_exclusive on an existing name returns "
          "an empty optional",
          "[shared_mapping]") {
    const auto name = fmt::format("/uenv-test-shm-dup-{}", getpid());

    auto first = util::shared_mapping<payload>::create_exclusive(name);
    REQUIRE(first);
    REQUIRE(first->has_value());

    auto second = util::shared_mapping<payload>::create_exclusive(name);
    REQUIRE(second);               // not an error ...
    REQUIRE(!second->has_value()); // ... just empty: someone else won.

    REQUIRE((*first)->unlink());
}

TEST_CASE("shared_mapping: open_existing on an unknown name fails",
          "[shared_mapping]") {
    const auto name = fmt::format("/uenv-test-shm-missing-{}", getpid());

    auto opened = util::shared_mapping<payload>::open_existing(name);
    REQUIRE(!opened);
    REQUIRE(opened.error().find(name) != std::string::npos);
}

// unlink() removes the name from the system, but must not invalidate
// mappings peers already hold -- exactly like unlink(2) on a regular file
// that is still open elsewhere.
TEST_CASE("shared_mapping: unlink removes the name but not live mappings",
          "[shared_mapping]") {
    const auto name = fmt::format("/uenv-test-shm-unlink-{}", getpid());

    auto created = util::shared_mapping<payload>::create_exclusive(name);
    REQUIRE(created);
    REQUIRE(created->has_value());
    auto a = std::move(**created);

    auto opened = util::shared_mapping<payload>::open_existing(name);
    REQUIRE(opened);
    auto b = std::move(*opened);

    REQUIRE(a.unlink());

    // the name is gone ...
    auto reopened = util::shared_mapping<payload>::open_existing(name);
    REQUIRE(!reopened);

    // ... but the mappings opened before the unlink are still live and still
    // share memory.
    a->value = 5;
    REQUIRE(b->value == 5);
}

// the destructor only has to release this process's view (munmap); the name
// itself stays until something calls unlink() explicitly.
TEST_CASE("shared_mapping: destructor does not unlink the name",
          "[shared_mapping]") {
    const auto name = fmt::format("/uenv-test-shm-dtor-{}", getpid());

    {
        auto created = util::shared_mapping<payload>::create_exclusive(name);
        REQUIRE(created);
        REQUIRE(created->has_value());
        // goes out of scope without calling unlink()
    }

    int fd = shm_open(name.c_str(), O_RDWR, 0);
    REQUIRE(fd >= 0);
    close(fd);
    REQUIRE(shm_unlink(name.c_str()) == 0); // manual cleanup
}

TEST_CASE("shared_mapping: move construction and assignment",
          "[shared_mapping]") {
    const auto name1 = fmt::format("/uenv-test-shm-move1-{}", getpid());
    const auto name2 = fmt::format("/uenv-test-shm-move2-{}", getpid());

    auto created1 = util::shared_mapping<payload>::create_exclusive(name1);
    REQUIRE(created1);
    REQUIRE(created1->has_value());
    auto a = std::move(**created1);
    a->value = 11;

    util::shared_mapping<payload> moved{std::move(a)};
    REQUIRE(moved->value == 11);
    REQUIRE(moved.name() == name1);

    // move-assignment over a live handle must release what it's overwriting
    // (a leak here shows up under ASan, not as a REQUIRE failure).
    auto created2 = util::shared_mapping<payload>::create_exclusive(name2);
    REQUIRE(created2);
    REQUIRE(created2->has_value());
    auto b = std::move(**created2);
    b = std::move(moved);
    REQUIRE(b->value == 11);
    REQUIRE(b.name() == name1);

    REQUIRE(b.unlink());
    REQUIRE(shm_unlink(name2.c_str()) == 0); // name2's object outlives b
}

// exercises a separate process, not just a second handle, observes state
// written before it forked, and the parent observes what the child wrote back.
TEST_CASE("shared_mapping: a forked child observes and mutates shared state",
          "[shared_mapping]") {
    const auto name = fmt::format("/uenv-test-shm-fork-{}", getpid());

    auto created = util::shared_mapping<payload>::create_exclusive(name);
    REQUIRE(created);
    REQUIRE(created->has_value());
    auto parent = std::move(**created);
    parent->value = 123;
    parent->child_saw_it = false;

    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        auto opened = util::shared_mapping<payload>::open_existing(name);
        if (!opened) {
            _exit(1);
        }
        auto& child = *opened;
        if (child->value != 123) {
            _exit(2);
        }
        child->child_saw_it = true;
        _exit(0);
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
    REQUIRE(parent->child_saw_it);

    REQUIRE(parent.unlink());
}
