FIND_PACKAGE(Threads REQUIRED)

SET(Boost_USE_MULTITHREADED ON)
FIND_PACKAGE(Boost REQUIRED COMPONENTS system program_options log log_setup REQUIRED)

FIND_PACKAGE(yaml-cpp REQUIRED)
LINK_DIRECTORIES(${YAML_CPP_LIBRARY_DIR})

ADD_LIBRARY(ufw_app SHARED
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

SET_TARGET_PROPERTIES(ufw_app PROPERTIES
    PUBLIC_HEADER "application.hpp;configuration.hpp;entity.hpp;exception_types.hpp;library.hpp;library_repository.hpp;lifecycle_participant.hpp;loader.hpp;logger.hpp;plugin_repository.hpp")
    
TARGET_COMPILE_DEFINITIONS(ufw_app PUBLIC "-DBOOST_LOG_DYN_LINK")

TARGET_LINK_LIBRARIES(ufw_app
    yaml-cpp
    Boost::log_setup
    Boost::program_options
    Boost::system
    Boost::log
    ${CMAKE_THREAD_LIBS_INIT}
    )

INCLUDE_DIRECTORIES(${Boost_INCLUDE_DIR})

INSTALL(TARGETS ufw_app EXPORT UfwTargets
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}" COMPONENT shlib
    PUBLIC_HEADER DESTINATION "${INSTALL_INCLUDE_DIR}/ufw/app"
    COMPONENT dev)


ADD_LIBRARY(example SHARED
    example.cpp)

TARGET_LINK_LIBRARIES(example
    ufw_app
    ${CMAKE_THREAD_LIBS_INIT}
    )

INSTALL(TARGETS example EXPORT UfwTargets
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}" COMPONENT shlib)


ADD_EXECUTABLE(ufw_launcher
    main.cpp)

TARGET_LINK_LIBRARIES(ufw_launcher
    ufw_app
    ${CMAKE_DL_LIBS})

INSTALL(TARGETS ufw_launcher EXPORT UfwTargets
    RUNTIME DESTINATION "${INSTALL_BIN_DIR}" COMPONENT bin)
