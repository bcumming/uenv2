#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <util/parse.h>
#include <util/strings.h>

namespace util {

std::string parse_error::message() const {
    return fmt::format("{}\n  {}\n  {}{}", detail, input, std::string(loc, ' '),
                       std::string(std::max(1u, width), '^'));
}

util::expected<void, parse_error> expect_end(lex::lexer& L,
                                             std::string_view what) {
    if (L != lex::tok::end) {
        const auto t = L.peek();
        return util::unexpected(parse_error{
            L.string(), fmt::format("unexpected '{}' in {}", t.spelling, what),
            t});
    }
    return {};
}

util::expected<unsigned, parse_error> parse_unsigned(std::string_view text) {
    const std::string sanitised = util::strip(text);
    lex::lexer L(sanitised);

    const auto t = L.peek();
    if (t != lex::tok::integer) {
        return util::unexpected(parse_error{
            L.string(),
            fmt::format("expected an unsigned integer, found '{}'", t.spelling),
            t});
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

    if (auto e = expect_end(L, "unsigned integer"); !e) {
        return util::unexpected(e.error());
    }

    return value;
}

} // namespace util
