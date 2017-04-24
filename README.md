[![build](https://api.travis-ci.org/mrbald/ufw.svg?branch=master)](https://travis-ci.org/mrbald/ufw)

# uFW - Micro Framework
A minimalistic framework for rapid server side applications prototyping and experimental work.
Additionally the framework is an example of tested and benchmarked CMake-based C++ software package.

## Building Dependencies

### Boost

### YamlCPP

    $ git clone https://github.com/jbeder/yaml-cpp.git
    $ mkdir yaml-cpp-build && cd yaml-cpp-build
    $ cmake -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release ../yaml-cpp
    $ make -j$(nproc) && sudo make install

### CapNProto

    $ git clone https://github.com/sandstorm-io/capnproto.git
    $ mkdir capnproto-build && cd capnproto-build
    $ cmake -DCMAKE_BUILD_TYPE=Release ../capnproto/c++
    $ make -j$(nproc) && sudo make install

## Building uFW

### Compiling

    $ git clone https://github.com/mrbald/ufw.git
    $ mkdir ufw-build && cd ufw-build
    $ cmake -DCMAKE_BUILD_TYPE=Release ../ufw
    $ make -j$(nproc)

### Running tests
To run all tests run

    $ make unit-test

To run individual tests with verbose output run

    $ make BOOST_TEST_LOG_LEVEL=all BOOST_TEST_RUN_FILTERS=ufw_app/* unit-test

### Running benchmarks

    $ make benchmark

### Installing

    $ sudo make install
