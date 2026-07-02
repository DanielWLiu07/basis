# Layering is enforced by what each library links. See PLAN.md.
#   basis_core   <- nothing project-specific
#   basis_model  <- basis_core
#   basis (exe)  <- basis_model, basis_core
#
# feed/ net/ normalize/ analytics/ api/ bench/ come online per phase; their
# interface headers live in the tree now (parsed via main.cpp) but are not
# compiled until their phase wires sources + deps.

add_library(basis_core
  core/logger.cpp
)
target_include_directories(basis_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(basis_core PUBLIC basis_warnings basis_sanitize)

add_library(basis_model
  model/order_book.cpp
  model/unified_book.cpp
)
target_link_libraries(basis_model PUBLIC basis_core)

add_executable(basis main.cpp)
target_link_libraries(basis PRIVATE basis_model)
