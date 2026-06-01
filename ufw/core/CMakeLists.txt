# ufw_core — standalone low-level primitives (memory, rings, sync, actor).
# Deliberately depends on NOTHING from ufw_app: it throws std exceptions, not
# ufw::fatal_error. Fine-grained libs get split out only on demonstrated need.
find_package(Threads REQUIRED)

add_library(ufw_core SHARED
    mem/memory_region.cpp
    mem/memory_region.hpp)

target_compile_features(ufw_core PUBLIC cxx_std_23)

set_target_properties(ufw_core PROPERTIES
    PUBLIC_HEADER "mem/memory_region.hpp")

target_link_libraries(ufw_core
    PRIVATE
        ufw::warnings
        ufw::sanitizers
        Threads::Threads)

if(UFW_CLANG_TIDY_COMMAND)
    set_target_properties(ufw_core PROPERTIES CXX_CLANG_TIDY "${UFW_CLANG_TIDY_COMMAND}")
endif()

install(TARGETS ufw_core EXPORT UfwTargets
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    PUBLIC_HEADER DESTINATION "${INSTALL_INCLUDE_DIR}/ufw/core/mem"
    COMPONENT shlib)
