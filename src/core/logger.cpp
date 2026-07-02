#include "core/logger.h"

#include <atomic>
#include <cstdio>

namespace basis::log {
namespace {

std::atomic<Level> g_level{Level::Info};

const char* tag(Level l) {
  switch (l) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
  }
  return "?????";
}

}  // namespace

void set_level(Level level) {
  g_level.store(level, std::memory_order_relaxed);
}

void log(Level level, std::string_view msg) {
  if (static_cast<int>(level) < static_cast<int>(g_level.load(std::memory_order_relaxed))) {
    return;
  }
  std::FILE* out = (level >= Level::Warn) ? stderr : stdout;
  std::fprintf(out, "[%s] %.*s\n", tag(level),
               static_cast<int>(msg.size()), msg.data());
}

}  // namespace basis::log
