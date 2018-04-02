FIND_PACKAGE(Threads REQUIRED)

SET(Boost_USE_MULTITHREADED ON)
FIND_PACKAGE(Boost REQUIRED COMPONENTS system program_options log REQUIRED)

FIND_PACKAGE(yaml-cpp REQUIRED)

ADD_LIBRARY(ufw_app SHARED
    exception_types.hpp
    configuration.hpp
    application.hpp
    application.cpp
    logger.hpp
    logger.cpp)

SET_TARGET_PROPERTIES(ufw_app PROPERTIES
    PUBLIC_HEADER application.hpp
    PUBLIC_HEADER logger.hpp
    PUBLIC_HEADER configuration.hpp
    PUBLIC_HEADER exception_types.hpp)
    
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

ADD_EXECUTABLE(ufw_lancher
    main.cpp)

TARGET_LINK_LIBRARIES(ufw_lancher
    ufw_app)

INSTALL(TARGETS ufw_lancher EXPORT UfwTargets
    RUNTIME DESTINATION "${INSTALL_LIB_DIR}" COMPONENT bin)
