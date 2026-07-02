# Shared warning flags as an interface target. Not -Werror in the scaffold;
# CI can promote to -Werror once the codebase fills in.
add_library(basis_warnings INTERFACE)
target_compile_options(basis_warnings INTERFACE
  $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall;-Wextra;-Wpedantic;-Wshadow>
)
