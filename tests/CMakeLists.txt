FIND_PACKAGE(Threads REQUIRED)

SET(Boost_USE_MULTITHREADED ON)  
FIND_PACKAGE(Boost REQUIRED COMPONENTS unit_test_framework REQUIRED)
ADD_DEFINITIONS("-DBOOST_TEST_DYN_LINK")

ADD_LIBRARY(ufw_app_tests OBJECT
        ufw_app_tests.cpp)

ADD_EXECUTABLE(ufw_tests
        $<TARGET_OBJECTS:ufw_app_tests>
        main.cpp)

TARGET_LINK_LIBRARIES(ufw_tests
        boost_unit_test_framework)

ADD_CUSTOM_TARGET(unit-test ufw_tests DEPENDS ufw_tests USES_TERMINAL)
