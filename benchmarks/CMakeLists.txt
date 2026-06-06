find_package(benchmark QUIET)

if(benchmark_FOUND)
    message(STATUS "benchmarks enabled (using google.benchmark ${benchmark_VERSION})")

    add_executable(ufw_benchmarks
            ufw-sandbox-benchmarks.cpp
            ufw-mem-benchmarks.cpp
            ufw-ring-benchmarks.cpp
            ufw-dispatch-benchmarks.cpp
            main.cpp)

    target_compile_features(ufw_benchmarks PRIVATE cxx_std_23)

    target_link_libraries(ufw_benchmarks
        PRIVATE
            benchmark::benchmark
            ufw_core
            ufw::warnings
            ufw::sanitizers)

    add_custom_target(benchmark ufw_benchmarks DEPENDS ufw_benchmarks USES_TERMINAL)
else()
    message(STATUS "benchmarks disabled (google.benchmark not found)")
endif()
