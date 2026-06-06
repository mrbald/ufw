find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS headers)
find_package(yaml-cpp REQUIRED)
find_package(quill REQUIRED)
find_package(fmt REQUIRED)

# Stamp the ABI version (PROJECT_VERSION) into a generated header, used by both
# the launcher (the LIBRARY loader's check) and plugins (the UFW_PLUGIN() stamp).
configure_file(version.hpp.in "${PROJECT_BINARY_DIR}/ufw/app/version.hpp" @ONLY)

add_library(ufw_app SHARED
    actor.hpp
    application.hpp
    asio_backstop.hpp
    configuration.hpp
    entity.hpp
    exception_types.hpp
    library.hpp
    library_repository.hpp
    lifecycle_participant.hpp
    loader.hpp
    logger.hpp
    plugin.hpp
    plugin_repository.hpp
    application.cpp
    logger.cpp)

set_target_properties(ufw_app PROPERTIES
    PUBLIC_HEADER "actor.hpp;application.hpp;asio_backstop.hpp;configuration.hpp;entity.hpp;exception_types.hpp;library.hpp;library_repository.hpp;lifecycle_participant.hpp;loader.hpp;logger.hpp;plugin.hpp;plugin_repository.hpp;${PROJECT_BINARY_DIR}/ufw/app/version.hpp")

target_compile_features(ufw_app PUBLIC cxx_std_23)

target_link_libraries(ufw_app
    PUBLIC
        ufw_core   # the dispatch fabric (exec/) + sys/ primitives in public headers
        yaml-cpp::yaml-cpp
        Boost::headers
        quill::quill
        fmt::fmt
    PRIVATE
        ufw::warnings
        ufw::sanitizers
        Threads::Threads)

if(UFW_CLANG_TIDY_COMMAND)
    set_target_properties(ufw_app PROPERTIES CXX_CLANG_TIDY "${UFW_CLANG_TIDY_COMMAND}")
endif()

install(TARGETS ufw_app EXPORT UfwTargets
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}" COMPONENT shlib
    PUBLIC_HEADER DESTINATION "${INSTALL_INCLUDE_DIR}/ufw/app"
    COMPONENT dev)


add_library(example SHARED
    example.cpp)

target_compile_features(example PRIVATE cxx_std_23)

target_link_libraries(example
    PRIVATE
        ufw_app
        ufw::warnings
        ufw::sanitizers
        Threads::Threads)

install(TARGETS example EXPORT UfwTargets
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}" COMPONENT shlib)


add_executable(ufw_launcher
    main.cpp)

target_compile_features(ufw_launcher PRIVATE cxx_std_23)

target_link_libraries(ufw_launcher
    PRIVATE
        ufw_app
        ufw::warnings
        ufw::sanitizers
        ${CMAKE_DL_LIBS})

install(TARGETS ufw_launcher EXPORT UfwTargets
    RUNTIME DESTINATION "${INSTALL_BIN_DIR}" COMPONENT bin)
