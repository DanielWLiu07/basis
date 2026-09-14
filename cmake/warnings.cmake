# Shared warning flags as an interface target.
#
# The list is everything the tree actually compiles clean under, not a
# wishlist: each flag below was enabled against the whole repo and only
# kept if it produced zero warnings, or if the handful it produced were
# fixed rather than suppressed.
#
# -Wconversion and -Wsign-conversion are the two that earn their keep here.
# They are usually dismissed as noisy, and they found the real thing in
# this pass: rng::uniform_int returns int64 and its result was being
# assigned into a vector<int>. Harmless today because the draw indexes a
# quantity that fits an int, and a silent truncation the day it does not.
#
# Deliberately NOT enabled:
#
#   -Wfloat-equal   flags three exact comparisons that are correct. Mid
#                   prices are computed from integer cents, so they are
#                   exactly representable, and the question being asked is
#                   "did this value change at all" - an epsilon there would
#                   swallow genuine small moves. A warning that would make
#                   correct code worse is not worth the noise.
#
# -Werror is on in CI and off locally: a compiler upgrade that adds a new
# warning should fail the build that gates merges, not the build someone
# is using to debug at the time.
add_library(basis_warnings INTERFACE)
target_compile_options(basis_warnings INTERFACE
  $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:
    -Wall;-Wextra;-Wpedantic;-Wshadow;
    -Wconversion;-Wsign-conversion;
    -Wnon-virtual-dtor;-Woverloaded-virtual;
    -Wold-style-cast;-Wcast-align;
    -Wnull-dereference;-Wdouble-promotion;-Wimplicit-fallthrough
  >
)

option(BASIS_WERROR "Treat warnings as errors (CI sets this)" OFF)
if(BASIS_WERROR)
  target_compile_options(basis_warnings INTERFACE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Werror>
  )
endif()
