find_package(Threads REQUIRED)

add_library(ufw_topics SHARED
    topics.cpp
    topics.hpp)

set_target_properties(ufw_topics PROPERTIES
    PUBLIC_HEADER topics.hpp)

target_compile_features(ufw_topics PUBLIC cxx_std_23)

target_link_libraries(ufw_topics
    PUBLIC
        ufw_app
    PRIVATE
        ufw::warnings
        ufw::sanitizers
        Threads::Threads)

if(UFW_CLANG_TIDY_COMMAND)
    set_target_properties(ufw_topics PROPERTIES CXX_CLANG_TIDY "${UFW_CLANG_TIDY_COMMAND}")
endif()

install(TARGETS ufw_topics EXPORT UfwTargets
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    PUBLIC_HEADER DESTINATION "${INSTALL_INCLUDE_DIR}/ufw/topics"
    COMPONENT shlib)
