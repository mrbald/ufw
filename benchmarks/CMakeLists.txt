IF(benchmark_DIR)
    FIND_PACKAGE(benchmark PATHS "${benchmark_DIR}" QUIET)
ELSE(benchmark_DIR)
    FIND_PACKAGE(benchmark QUIET)
ENDIF(benchmark_DIR)

IF(benchmark_FOUND)
    MESSAGE(STATUS "benchmarks enabled (using google.benchmark ${benchmark_VERSION} from ${benchmark_DIR})")

    ADD_EXECUTABLE(ufw_benchmarks
            ufw-sandbox-benchmarks.cpp
            main.cpp)

    TARGET_LINK_LIBRARIES(ufw_benchmarks
            benchmark::benchmark)

    ADD_CUSTOM_TARGET(benchmark ufw_benchmarks DEPENDS ufw_benchmarks USES_TERMINAL)

ELSE()
    MESSAGE(STATUS "benchmarks disabled (google.benchmark not found)")
ENDIF()
