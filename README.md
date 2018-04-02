[![build](https://api.travis-ci.org/mrbald/ufw.svg?branch=master)](https://travis-ci.org/mrbald/ufw)

# &mu;FW - Micro Framework
A minimalist framework for rapid server side applications prototyping and experimental work.
Additionally the framework is an example of tested and benchmarked CMake-based C++ software package.

## Introduction

### Design Goals

* Modularity
* Configuration reflecting the application structure
* Consistent lifecycle management
* Singleton-free (clean traceable dependencies)
* Modules rewriting without code change

### Configuration - YAML
An example of a FIX (financial information exchange format) endpoint component configuration:
```
---
application:

  entities:
    # ====== general purpose entities ======
    - name: LOGGER
      config: |
        [Core]
        DisableLogging=false
        LogSeverity=error

        [Sinks.Console]
        Destination=Console
        Format="%TimeStamp(format=\"%H:%M:%S.%f\")% | %Severity(format=\"%6s\")% | %ThreadPID% | %Entity% - %Tag%%Message%"
        Asynchronous=true
        AutoFlush=true

    # ====== loaders ======
    - name: FIX_SESSION
      config:
        config_file: quickfix.properties

    # ====== managed FIX sessions ======
    - name: ECN
      loader_ref: FIX_SESSION
      config:
        handler_ref: FIX_ROUTER
        begin_string: FIX.4.4
        sender_comp_id: CID_ROUTER
        target_comp_id: CID_ECN
        forward_exception_text: true
...

```

### Terminology

* _entity_ - an identifiable building block
* _application_ - container of _entities_
* _loader_ - an _entity_ capable of loading other _entities_
* _lifecycle_participant_ - an _entity_ with application managed lifecycle

### Lifecycle Phases

* `init()` - *lifecycle_participants* may/should discover and cache strongly typed references to each other and fail fast if anything is missing or is of a wrong type
* `start()` - *lifecycle_participants* may/should establish required connections, spawn threads, etc.
* `up() - *lifecycle_participants* may start messaging others
* `stop()` - opposite of `start()`
* `fini()` - opposite of `init()`

### Loaders

* _loaders_ are _entities_
* _loaders_ can load other _loaders_
* *default_loader* - a special "seed" loader used by application to load entities by name (including other loaders)
* whether or not an _entity_ is loaded with _loader_ is specified in the config (flexibility!)
* _entities_ can be added to application programmatically without _loaders_ (e.g. *default_loader*)

### Concurrency

* Initialisation is single threaded and done in the application main thread
* A single thread "application context" (main thread :) ) is available out of the box
* All other concurrency is incremental to the `ufw.application`

## Building Dependencies
The author's main development platforms are x86_64 [Arch Linux](https://www.archlinux.org/) and [Mac OS X + Homebrew](https://brew.sh/).
Both have quite up to date versions of all _&mu;FW_ dependencies.
For those working in a less bleeding-edge enviroronments - below are th einstructions for building the dependencies from scratch.

### Boost
Use instructions from the [Boost Home Page](https://boost.org)
The _&mu;FW_ is using these modules:
* system
* program_options
* log
* boost_unit_test_framework

### YamlCPP

```
$ git clone https://github.com/jbeder/yaml-cpp.git
$ mkdir yaml-cpp-build && cd yaml-cpp-build
$ cmake -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release ../yaml-cpp
$ make -j$(nproc) && sudo make install
```

### CapNProto (optional - examples)

```
$ git clone https://github.com/sandstorm-io/capnproto.git
$ mkdir capnproto-build && cd capnproto-build
$ cmake -DCMAKE_BUILD_TYPE=Release ../capnproto/c++
$ make -j$(nproc) && sudo make install
```

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

## References
[CMake/How To Find Libraries](https://cmake.org/Wiki/CMake:How_To_Find_Libraries)
