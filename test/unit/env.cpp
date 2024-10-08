#include <catch2/catch_all.hpp>
#include <fmt/core.h>

#include <uenv/env.h>
#include <uenv/log.h>
#include <uenv/meta.h>

TEST_CASE("load_meta", "[env]") {
    REQUIRE(uenv::load_meta("../test/data/env-files/cp2k-2024.2-v1.json"));
}
