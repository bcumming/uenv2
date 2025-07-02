#include <string>
#include <vector>

namespace util {

// split a string on a character delimiter
//
// if drop_empty==false (default)
//
// ""       -> [""]
// ","      -> ["", ""]
// ",,"     -> ["", "", ""]
// ",a"     -> ["", "a"]
// "a,"     -> ["a", ""]
// "a"      -> ["a"]
// "a,b"    -> ["a", "b"]
// "a,b,c"  -> ["a", "b", "c"]
// "a,b,,c" -> ["a", "b", "", "c"]
//
// if drop_empty==true
//
// ""       -> []
// ","      -> []
// ",,"     -> []
// ",a"     -> ["a"]
// "a,"     -> ["a"]
// "a"      -> ["a"]
// "a,b"    -> ["a", "b"]
// "a,b,c"  -> ["a", "b", "c"]
// "a,b,,c" -> ["a", "b", "c"]
std::vector<std::string> split(std::string_view s, const char delim,
                               const bool drop_empty = false);

// strip whitespace from beginning and end of a string
std::string strip(std::string_view input);

std::string join(std::string_view joiner, const std::vector<std::string>& list);

bool is_sha(const std::string& str);

} // namespace util
