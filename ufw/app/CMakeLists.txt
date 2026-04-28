find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS headers system program_options log log_setup)
find_package(yaml-cpp REQUIRED)
find_package(quill REQUIRED)
find_package(fmt REQUIRED)

add_library(ufw_app SHARED
    application.hpp
    configuration.hpp
    entity.hpp
    exception_types.hpp
    library.hpp
    library_repository.hpp
    lifecycle_participant.hpp
    loader.hpp
    logger.hpp
    plugin_repository.hpp
    application.cpp
    logger.cpp)

set_target_properties(ufw_app PROPERTIES
    PUBLIC_HEADER "application.hpp;configuration.hpp;entity.hpp;exception_types.hpp;library.hpp;library_repository.hpp;lifecycle_participant.hpp;loader.hpp;logger.hpp;plugin_repository.hpp")

target_compile_features(ufw_app PUBLIC cxx_std_23)
target_compile_definitions(ufw_app PUBLIC BOOST_LOG_DYN_LINK)

target_link_libraries(ufw_app
    PUBLIC
        yaml-cpp::yaml-cpp
        Boost::headers
        Boost::program_options
        Boost::system
        Boost::log
        Boost::log_setup
    PRIVATE
        ufw::warnings
        ufw::sanitizers
        quill::quill
        fmt::fmt
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
