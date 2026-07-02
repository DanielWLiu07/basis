# Sanitizer flags as an interface target, applied to all basis targets.
# ASan/UBSan and TSan are mutually exclusive; CI runs them as separate jobs.
add_library(basis_sanitize INTERFACE)

if(BASIS_SANITIZE AND BASIS_SANITIZE_THREAD)
  message(FATAL_ERROR "BASIS_SANITIZE and BASIS_SANITIZE_THREAD are mutually exclusive")
endif()

if(BASIS_SANITIZE)
  target_compile_options(basis_sanitize INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(basis_sanitize INTERFACE -fsanitize=address,undefined)
endif()

if(BASIS_SANITIZE_THREAD)
  target_compile_options(basis_sanitize INTERFACE
    -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(basis_sanitize INTERFACE -fsanitize=thread)
endif()
