#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace basis::core {

// Transparent hash so unordered maps keyed by std::string can be probed
// with a std::string_view without materializing a temporary key. The hot
// path looks up venue market ids and event ids on every delta; those
// lookups must not allocate.
struct StringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view s) const noexcept {
    return std::hash<std::string_view>{}(s);
  }
};

template <typename Value>
using StringMap =
    std::unordered_map<std::string, Value, StringHash, std::equal_to<>>;

using StringSet =
    std::unordered_set<std::string, StringHash, std::equal_to<>>;

}  // namespace basis::core
