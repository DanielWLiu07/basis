#pragma once

#include <string_view>

namespace basis::log {

enum class Level { Debug, Info, Warn, Error };

void set_level(Level level);
void log(Level level, std::string_view msg);

inline void debug(std::string_view m) { log(Level::Debug, m); }
inline void info(std::string_view m)  { log(Level::Info, m); }
inline void warn(std::string_view m)  { log(Level::Warn, m); }
inline void error(std::string_view m) { log(Level::Error, m); }

}  // namespace basis::log
