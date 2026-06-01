find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS unit_test_framework)
find_package(yaml-cpp REQUIRED)

add_library(ufw_app_tests OBJECT
        ufw_topics_tests.cpp
        ufw_app_tests.cpp
        ufw_core_tests.cpp
        ufw_core_ring_tests.cpp)

target_compile_features(ufw_app_tests PRIVATE cxx_std_23)
target_compile_definitions(ufw_app_tests PUBLIC BOOST_TEST_DYN_LINK)

# Fork-based fault/death tests are unstable under a sanitizer's fault handler.
if(UFW_ENABLE_SANITIZERS)
    target_compile_definitions(ufw_app_tests PRIVATE UFW_TESTS_SANITIZED=1)
endif()

target_link_libraries(ufw_app_tests
    PUBLIC
        ufw_topics
        ufw_core
        Boost::unit_test_framework
        yaml-cpp::yaml-cpp
    PRIVATE
        ufw::warnings
        ufw::sanitizers)  # instrument the test code itself (so __has_feature(address_sanitizer) works)

add_executable(ufw_tests
        $<TARGET_OBJECTS:ufw_app_tests>
        main.cpp)

target_compile_features(ufw_tests PRIVATE cxx_std_23)
target_compile_definitions(ufw_tests PRIVATE BOOST_TEST_DYN_LINK)

target_link_libraries(ufw_tests
    PRIVATE
        ufw_topics
        ufw_core
        Boost::unit_test_framework
        yaml-cpp::yaml-cpp
        ufw::warnings
        ufw::sanitizers)

add_custom_target(unit-test ufw_tests DEPENDS ufw_tests USES_TERMINAL)

add_test(NAME ctest_ufw_tests COMMAND ufw_tests)
