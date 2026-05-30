# nanobind Python control-plane module. Built only when UFW_BUILD_PYTHON=ON.
#
# Sanitizers: a stock CPython is not built with ASan, so a sanitized extension
# module cannot be imported without preloading the ASan runtime. Build the
# Python module with -DUFW_ENABLE_SANITIZERS=OFF.
if(UFW_ENABLE_SANITIZERS)
    message(WARNING
        "UFW_BUILD_PYTHON=ON with sanitizers enabled: the module will not be "
        "importable by a stock python without preloading the ASan runtime. "
        "Configure the Python build with -DUFW_ENABLE_SANITIZERS=OFF.")
endif()

find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)

# Locate nanobind's CMake package via the chosen interpreter (pip install nanobind).
execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT
    RESULT_VARIABLE _ufw_nb_rc)
if(NOT _ufw_nb_rc EQUAL 0)
    message(FATAL_ERROR
        "nanobind not found for ${Python_EXECUTABLE}; run: "
        "${Python_EXECUTABLE} -m pip install nanobind")
endif()
find_package(nanobind CONFIG REQUIRED)

# Produces ufw.<abi>.so so Python can `import ufw`.
nanobind_add_module(ufw_py NB_STATIC module.cpp)
set_target_properties(ufw_py PROPERTIES OUTPUT_NAME ufw)
target_compile_features(ufw_py PRIVATE cxx_std_23)
target_link_libraries(ufw_py PRIVATE ufw_app)

install(TARGETS ufw_py LIBRARY DESTINATION "${Python_SITEARCH}" COMPONENT python)
