FIND_PACKAGE(Threads REQUIRED)

SET(Boost_USE_MULTITHREADED ON)
FIND_PACKAGE(Boost REQUIRED COMPONENTS system program_options log REQUIRED)

FIND_PACKAGE(yaml-cpp REQUIRED)


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
    boost_log_setup
    ${Boost_PROGRAM_OPTIONS_LIBRARY}
    ${Boost_SYSTEM_LIBRARY}
    ${Boost_LOG_LIBRARY}
    ${CMAKE_THREAD_LIBS_INIT}
    )

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
