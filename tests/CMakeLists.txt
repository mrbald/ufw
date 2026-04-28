find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS unit_test_framework)
find_package(yaml-cpp REQUIRED)

add_library(ufw_app_tests OBJECT
        ufw_topics_tests.cpp
        ufw_app_tests.cpp)

target_link_libraries(ufw_app_tests
    PUBLIC
        ufw_topics
        Boost::unit_test_framework
        yaml-cpp::yaml-cpp)

target_compile_definitions(ufw_app_tests PUBLIC BOOST_TEST_DYN_LINK)

add_executable(ufw_tests
        $<TARGET_OBJECTS:ufw_app_tests>
        main.cpp)

target_link_libraries(ufw_tests
    PRIVATE
        ufw_topics
        Boost::unit_test_framework
        yaml-cpp::yaml-cpp)

target_compile_definitions(ufw_tests PRIVATE BOOST_TEST_DYN_LINK)

add_custom_target(unit-test ufw_tests DEPENDS ufw_tests USES_TERMINAL)

add_test(NAME ctest_ufw_tests COMMAND ufw_tests)
