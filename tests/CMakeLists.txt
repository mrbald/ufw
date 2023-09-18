FIND_PACKAGE(Threads REQUIRED)

SET(Boost_USE_MULTITHREADED ON)  
FIND_PACKAGE(Boost REQUIRED COMPONENTS unit_test_framework REQUIRED)
ADD_DEFINITIONS("-DBOOST_TEST_DYN_LINK")

FIND_PACKAGE(yaml-cpp REQUIRED)
LINK_DIRECTORIES(${YAML_CPP_LIBRARY_DIR})

ADD_LIBRARY(ufw_app_tests OBJECT
        ufw_topics_tests.cpp
        ufw_app_tests.cpp)

ADD_EXECUTABLE(ufw_tests
        $<TARGET_OBJECTS:ufw_app_tests>
        main.cpp)

TARGET_LINK_LIBRARIES(ufw_tests
        ufw_topics
        Boost::unit_test_framework)

INCLUDE_DIRECTORIES(${Boost_INCLUDE_DIR})

ADD_CUSTOM_TARGET(unit-test ufw_tests DEPENDS ufw_tests USES_TERMINAL)

ADD_TEST(ctest_ufw_tests "${CMAKE_COMMAND}" --build ${CMAKE_BINARY_DIR} --target unit-test)
