find_package(Threads REQUIRED)

add_library(ufw_topics SHARED
    topics.cpp
    topics.hpp)

set_target_properties(ufw_topics PROPERTIES
    PUBLIC_HEADER topics.hpp)

target_link_libraries(ufw_topics
    PUBLIC
        ufw_app
    PRIVATE
        Threads::Threads)

install(TARGETS ufw_topics EXPORT UfwTargets
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    PUBLIC_HEADER DESTINATION "${INSTALL_INCLUDE_DIR}/ufw/topics"
    COMPONENT shlib)
