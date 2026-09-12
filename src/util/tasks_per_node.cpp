#include <charconv>
#include <string_view>

#include <fmt/format.h>

#include <util/lex.h>
#include <util/tasks_per_node.h>

namespace util {

namespace {

// one entry in a SLURM_STEP_TASKS_PER_NODE-style string: `nodes` nodes each
// running `ranks` ranks.
struct section {
    unsigned nodes;
    unsigned ranks;
};

util::expected<unsigned, parse_error> parse_uint(lex::lexer& L) {
    const auto t = L.peek();
    if (t != lex::tok::integer) {
        return util::unexpected(parse_error{
            L.string(),
            fmt::format("expected an integer, found '{}'", t.spelling), t});
    }

    unsigned value;
    const auto result = std::from_chars(
        t.spelling.data(), t.spelling.data() + t.spelling.size(), value);
    if (result.ec != std::errc{}) {
        return util::unexpected(parse_error{
            L.string(),
            fmt::format("'{}' is not a valid unsigned integer", t.spelling),
            t});
    }

    L.next();
    return value;
}

// parse one entry at the current lexer position: RANKS | RANKS '(' 'x'
// NODES ')'. leaves the lexer positioned just after the entry (on a comma
// or end-of-input).
util::expected<section, parse_error> parse_entry(lex::lexer& L) {
    section result{.nodes = 1u, .ranks = 0u};

    auto ranks = parse_uint(L);
    if (!ranks) {
        return util::unexpected(ranks.error());
    }
    result.ranks = *ranks;

    if (L.current_kind() != lex::tok::lparen) {
        return result;
    }
    L.next(); // eat '('

    const auto xt = L.peek();
    if (xt != lex::tok::symbol || xt.spelling != "x") {
        return util::unexpected(parse_error{
            L.string(),
            fmt::format("expected 'x' after '(', found '{}'", xt.spelling),
            xt});
    }
    L.next(); // eat 'x'

    auto nodes = parse_uint(L);
    if (!nodes) {
        return util::unexpected(nodes.error());
    }
    result.nodes = *nodes;

    if (L.current_kind() != lex::tok::rparen) {
        const auto t = L.peek();
        return util::unexpected(
            parse_error{L.string(),
                        fmt::format("expected ')' to close section, found '{}'",
                                    t.spelling),
                        t});
    }
    L.next(); // eat ')'

    return result;
}

} // namespace

util::expected<unsigned, parse_error>
local_rank_count(std::string_view tasks_per_node, std::string_view node_id) {
    auto target = parse_unsigned(node_id);
    if (!target) {
        return util::unexpected(target.error());
    }

    lex::lexer L(tasks_per_node);
    unsigned pos = 0u; // nodes accounted for before the current entry

    while (true) {
        auto entry = parse_entry(L);
        if (!entry) {
            return util::unexpected(entry.error());
        }

        if (*target < pos + entry->nodes) {
            return entry->ranks;
        }
        pos += entry->nodes;

        if (L.current_kind() != lex::tok::comma) {
            break;
        }
        L.next(); // eat ','
    }

    const auto t = L.peek();
    return util::unexpected(
        parse_error{L.string(),
                    fmt::format("node {} is out of range for '{}'", *target,
                                tasks_per_node),
                    t});
}

} // namespace util
