#pragma once

#include <string_view>

#include <util/expected.h>
#include <util/parse.h>

namespace util {

// parse a SLURM_STEP_TASKS_PER_NODE-style compact partition string (e.g.
// "5,4(x3)") one entry at a time, stopping as soon as the entry covering
// `node_id` (a SLURM_NODEID-style node index) is found, and returning the
// number of ranks (tasks) on that node.
//
// this builds no intermediate container: it streams straight through
// tasks_per_node with a lexer, discarding each entry as soon as it has been
// checked against node_id, and makes no heap allocations of its own.
//
// returns a parse_error if either string is malformed, or if node_id is out
// of range for tasks_per_node (fewer nodes described than node_id).
//
// note: a trailing comma in tasks_per_node is not tolerated -- SLURM never
// emits one.
util::expected<unsigned, parse_error>
local_rank_count(std::string_view tasks_per_node, std::string_view node_id);

} // namespace util
