// Fuzz surface: the contracts.toml subset parser. Config files are less
// hostile than wire bytes, but the loader promises to fail loudly with a
// line number on any malformed input, and that promise should hold for
// every input, not just the ones a human would write.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "normalize/contract_registry.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  std::string error;
  const auto registry =
      basis::normalize::TomlContractRegistry::parse(text, &error);
  (void)registry;
  return 0;
}
