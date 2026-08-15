#pragma once

namespace basis::cli {

// Prints the full command list and returns 1, so a command can `return
// usage();` on bad input and get the exit code with the help text.
int usage();

}  // namespace basis::cli
