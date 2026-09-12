#include <catch2/catch_all.hpp>

#include <util/tasks_per_node.h>

TEST_CASE("local_rank_count", "[tasks_per_node]") {
    // "5,4(x3)": node 0 has 5 ranks, nodes 1-3 have 4 ranks each
    {
        auto r = util::local_rank_count("5,4(x3)", "0");
        REQUIRE(r);
        REQUIRE(*r == 5u);
    }
    {
        auto r = util::local_rank_count("5,4(x3)", "1");
        REQUIRE(r);
        REQUIRE(*r == 4u);
    }
    {
        auto r = util::local_rank_count("5,4(x3)", "2");
        REQUIRE(r);
        REQUIRE(*r == 4u);
    }
    {
        auto r = util::local_rank_count("5,4(x3)", "3");
        REQUIRE(r);
        REQUIRE(*r == 4u);
    }
    // uniform partition
    {
        auto r = util::local_rank_count("4(x4)", "0");
        REQUIRE(r);
        REQUIRE(*r == 4u);
    }
    {
        auto r = util::local_rank_count("4(x4)", "3");
        REQUIRE(r);
        REQUIRE(*r == 4u);
    }
    // single-node partition
    {
        auto r = util::local_rank_count("8", "0");
        REQUIRE(r);
        REQUIRE(*r == 8u);
    }
}

TEST_CASE("local_rank_count out of range", "[tasks_per_node]") {
    REQUIRE_FALSE(util::local_rank_count("5,4(x3)", "4"));
    REQUIRE_FALSE(util::local_rank_count("4(x4)", "4"));
    REQUIRE_FALSE(util::local_rank_count("8", "1"));
}

TEST_CASE("local_rank_count parse errors", "[tasks_per_node]") {
    // malformed tasks_per_node
    REQUIRE_FALSE(util::local_rank_count("", "0"));
    REQUIRE_FALSE(util::local_rank_count("a", "0"));
    REQUIRE_FALSE(util::local_rank_count("4(3)", "0")); // missing 'x'
    REQUIRE_FALSE(util::local_rank_count("4(x3", "0")); // missing ')'
    REQUIRE_FALSE(util::local_rank_count("4(x)", "0")); // missing NODES
    REQUIRE_FALSE(util::local_rank_count(",", "0"));

    // malformed node_id
    REQUIRE_FALSE(util::local_rank_count("5,4(x3)", ""));
    REQUIRE_FALSE(util::local_rank_count("5,4(x3)", "a"));
    REQUIRE_FALSE(util::local_rank_count("5,4(x3)", "-1"));

    // a trailing comma is not tolerated -- SLURM never emits one, and
    // being strict here keeps the entry parser simple
    REQUIRE_FALSE(util::local_rank_count("4,2,", "2"));
}
