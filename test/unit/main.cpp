#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>

#include <uenv/log.h>

static struct unit_init_log {
    unit_init_log() {
        uenv::init_log(spdlog::level::trace, spdlog::level::off);
    }
} uil{};
