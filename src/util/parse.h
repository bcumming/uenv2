#pragma once

#include <string>
#include <string_view>

#include <fmt/core.h>

#include <util/expected.h>
#include <util/lex.h>

namespace util {

/// represents an error generated when parsing a string.
///
/// stores the input string and the location (loc) in the string where the error
/// was encountered
///
/// there are two levels of error message:
/// - detail: a detailed low level description (e.g. "unexpected symbol ?") that
/// correlates to the loc in input
/// - description: a high level description, usually added at a higher level
/// (e.g. "invalid --uenv argument")
struct parse_error {
    std::string input;
    std::string description;
    std::string detail;
    unsigned loc;
    unsigned width;
    parse_error(std::string input, std::string detail, const lex::token& tok)
        : input(std::move(input)), detail(std::move(detail)), loc(tok.loc),
          width(tok.spelling.length()) {
    }
    parse_error(std::string_view input, std::string detail,
                const lex::token& tok)
        : input(input), detail(std::move(detail)), loc(tok.loc),
          width(tok.spelling.length()) {
    }
    parse_error(std::string input, std::string description, std::string detail,
                const lex::token& tok)
        : input(std::move(input)), description(std::move(description)),
          detail(std::move(detail)), loc(tok.loc),
          width(tok.spelling.length()) {
    }
    std::string message() const;
};

// some pre-processor gubbins that generates code to attempt
// parsing a value using a parse_x method. unwraps and
// forwards the error if there was an error.
// much ergonomics!
#define PARSE(L, TYPE, X)                                                      \
    {                                                                          \
        if (auto rval__ = parse_##TYPE(L))                                     \
            X = *rval__;                                                       \
        else                                                                   \
            return util::unexpected(std::move(rval__.error()));                \
    }

// consume and concatenate the spelling of every token for which test() is
// true. returns an error if no token was consumed.
template <typename Test>
util::expected<std::string, parse_error>
parse_string(lex::lexer& L, std::string_view type, Test&& test) {
    std::string result;
    while (test(L.current_kind())) {
        const auto t = L.next();
        result += t.spelling;
    }

    // if result is empty, nothing was parsed
    if (result.empty()) {
        const auto t = L.peek();
        return util::unexpected(parse_error{
            L.string(), fmt::format("unexpected '{}'", type, t.spelling), t});
    }

    return result;
}

// require the lexer to be at end-of-input, otherwise build a "trailing input"
// parse_error for `what`.
util::expected<void, parse_error> expect_end(lex::lexer& L,
                                             std::string_view what);

// parse a bare unsigned integer, e.g. from an environment variable like
// SLURM_NODEID. leading/trailing whitespace is tolerated; anything else
// around the digits is an error.
util::expected<unsigned, parse_error> parse_unsigned(std::string_view text);

} // namespace util
