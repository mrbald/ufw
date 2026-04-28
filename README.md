&mu;FW - Micro Framework
========================
[![ci](https://github.com/mrbald/ufw/actions/workflows/ci.yml/badge.svg)](https://github.com/mrbald/ufw/actions/workflows/ci.yml)
[![Join the chat at https://gitter.im/mrbald-ufw/Lobby](https://badges.gitter.im/mrbald-ufw/Lobby.svg)](https://gitter.im/mrbald-ufw/Lobby?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)
![Licence - ](https://img.shields.io/github/license/mrbald/ufw.svg)

&mu;FW is a minimalist framework for rapid server side applications prototyping and experimental work on [Unix-like][1] operating systems, primarily [Linux][2] and [macOS][3].
Those familiar with [Spring][4] or [Guice][5] may experience a strong deja-vu &mdash; dependency injection support was one of the &rarr;

Design Objectives
-----------------

* Minimum number of lines of code &mdash; clean concepts, compact implementations
* Modularity in spirit of [inversion of control][6]
* [Late binding][7] support via [shared library][11]-based [plugins][12]
* Consistency in module configuration, lifecycle, concurrency, and logging
* Singleton-free design with traceable dependencies
* Structured configuration reflecting both the application topology and the concurrency model
* Zero steady state runtime overhead
* Hacking-friendly design

Following core C++ design principles, the rule _"you don't pay for what you don't use"_ is adhered to where possible.

Introduction
------------

![uFW Topology](uFW_Topology.png)

### Terminology

The terminology used in the framework maps directly to the main building blocks, which are

* _entity_ - an identifiable building block of the application (a module)
* _application_ - container of _entities_
* _loader_ - an _entity_ capable of loading other _entities_
* _lifecycle_participant_ - an _entity_ with application managed lifecycle
* _execution context_ - set of rules for code execution (e.g. a specific thread, a thread pool, a strand on a thread pool, ...)
* _launcher_ - a binary (ufw_launcher, the entry point into an application)

### Configuration

Application configuration language is hierarchical [YAML][8].
This format gives a good representation of both application bootstrap process and the runtime structure.

### Lifecycle Phases

Application modules are created in the order they are defined in the configuration file and are destroyed in the reverse order.
Modules can opt to participate in the structural lifecycle by extending the virtual `ufw::lifecycle_participant` base.
The lifecycle phases `lifecycle_participant`-s are transitioned through are below.
The order of transition among individual participants matches their declaration order in the application configuration file.

* `init()` - *lifecycle_participants* may/should discover and cache strongly typed references to each other and fail fast if anything is missing or is of a wrong type
* `start()` - *lifecycle_participants* may/should establish required connections, spawn threads, etc.
* `up()` - *lifecycle_participants* may start messaging others
* `stop()` - opposite of `start()`
* `fini()` - opposite of `init()`

### Loaders

A subset of entities capable of loading other entities is called _loaders_.
A _loader_ _entity_ extends the virtual `ufw::loader` base.
_loaders_ are _entities_.
_loaders_ can load other _loaders_.
A special "seed" _loader_  &mdash; the `default_loader`, is used by the _application_ to load _entities_ by name (including other loaders).
Whether or not an _entity_ is loaded with a _loader_ is specified in the config (flexibility!).
_entities_ can be registered in the application programmatically without _loaders_.
The application registers the `default_loader` in directly in the constructor.
The default launcher registers `LIBRARY` (loads shared libaries) and `PLUGIN` (loads entities from shared libraries) loaders before initiating the application bootstrap.

### Concurrency

Application initialisation is done single-threaded in the application main thread.
Once the application is up the main thread becomes the host of the _default execution context_.
The _default execution context_ an instance of the `boost::asio::io_context` accessible from _entities_ via `this.context()`.
All other concurrency models are incremental to the `ufw.application`.

### Logging

Logging is part of the framework, backed by [Quill][13] (asynchronous, formatting on a backend thread, [fmt][14]-style format strings).
Each entity owns a named logger; the logger name is rendered in the log line via Quill's `%(logger)` pattern token.
Macros `LOG_DBG / LOG_INF / LOG_WRN / LOG_ERR` expand to `QUILL_LOG_*` and resolve `get_logger()` via unqualified lookup, so they pick up the entity's own logger inside member functions and a process-wide root logger elsewhere.

The logger is configured the same way as any other entity.
The _config_ block is decoded into a typed `logger_config { severity, pattern, timestamp_pattern }`.
See the configuration file fragment below as an example.

Trying It
---------

&mu;FW comes with an _example_ module packaged into a plugin shared library (`libexample.so` on [Linux][2]).

The below configuration fragment has a single instance of the _example_ module.

```yaml
---
application:

  entities:
    # ====== logger ======
    - name: LOGGER
      config:
        severity: info
        pattern: "%(time) | %(log_level:<7) | %(thread_id) | %(logger) - %(message)"
        timestamp_pattern: "%H:%M:%S.%Qms"

    # ====== a dynamic library ======
    - name: example_lib
      loader_ref: LIBRARY
      config:
        filename: libexample.so   # libexample.dylib on macOS

    # ====== an entity -- plugin from a dynamic library ======
    - name: example_plugin
      loader_ref: PLUGIN
      config:
        library_ref: example_lib
        constructor: example_ctor
...
```

To run it, store the above fragment into a YAML file (say config.yaml) and run the &mu;FW launcher as `ufw_launcher -c config.yaml`.

The console log should look similar to the below screenshot.

![screenshot](screenshot.png)

Building
--------

Dependencies are managed via [Conan 2][15]. The toolchain requirements are CMake &ge; 3.27 and a C++23-capable compiler (gcc 13, clang 18, or Apple clang 15+).

### One-time setup

```sh
$ pip install 'conan==2.7.*'
$ conan profile detect --force
# Bump cppstd in the detected profile to gnu23 (the project is C++23):
$ sed -i.bak 's/^compiler.cppstd=.*/compiler.cppstd=gnu23/' ~/.conan2/profiles/default
```

### Compiling

```sh
$ git clone https://github.com/mrbald/ufw.git && cd ufw
$ conan install . -s build_type=Debug --build=missing
$ cmake --preset conan-debug
$ cmake --build --preset conan-debug -j
```

For a Release build, swap `Debug` for `Release` and `conan-debug` for `conan-release`.

### Build options

| Option                  | Default              | Effect                                                      |
|-------------------------|----------------------|-------------------------------------------------------------|
| `UFW_ENABLE_SANITIZERS` | `ON` in Debug, `OFF` | AddressSanitizer + UndefinedBehaviorSanitizer on all targets |
| `UFW_ENABLE_CLANG_TIDY` | `ON`                 | Run clang-tidy on `ufw_app` / `ufw_topics` (silently skipped if `clang-tidy` is not in `PATH`) |
| `UFW_USE_CCACHE`        | `OFF`                | Use `ccache` as the compiler launcher when available         |

### Running tests

```sh
$ cmake --build --preset conan-debug --target unit-test
```

### Running benchmarks

```sh
$ cmake --build --preset conan-debug --target benchmark
```

### Installing

```sh
$ cmake --install build/Debug --prefix /path/to/prefix
```

The install ships `UfwConfig.cmake` / `UfwConfigVersion.cmake` / `UfwTargets.cmake`, so downstream consumers can `find_package(Ufw)` and link the namespaced targets `ufw::ufw_app`, `ufw::ufw_topics`, etc.

### Sanitizers and the plugin model

The framework loads plugins via `dlopen`, so AddressSanitizer must be present (or absent) consistently across the launcher and every plugin shared library. The `UFW_ENABLE_SANITIZERS` option enforces this uniformly across all first-party targets. On macOS, Apple clang embeds the toolchain rpath that points at `libclang_rt.asan_osx_dynamic.dylib`, so no `DYLD_LIBRARY_PATH` workarounds are needed. On Linux, the asan runtime is resolved at link time via `-fsanitize=address` on the launcher.

Using
-----

TODO

Hacking
-------

TODO

References
----------
[CMake/How To Find Libraries](https://cmake.org/Wiki/CMake:How_To_Find_Libraries)

[Markdown Cheatsheet](https://github.com/adam-p/markdown-here/wiki/Markdown-Cheatsheet)

[Draw.io](https://www.draw.io)


[1]: https://en.wikipedia.org/wiki/Unix-like
[2]: https://en.wikipedia.org/wiki/Linux
[3]: https://en.wikipedia.org/wiki/MacOS
[4]: http://spring.io/
[5]: https://github.com/google/guice
[6]: https://en.wikipedia.org/wiki/Inversion_of_control
[7]: https://en.wikipedia.org/wiki/Late_binding
[8]: http://yaml.org/
[9]: https://www.archlinux.org/
[10]: https://brew.sh/
[11]: https://en.wikipedia.org/wiki/Library_(computing)
[12]: https://en.wikipedia.org/wiki/Plug-in_(computing)
[13]: https://github.com/odygrd/quill
[14]: https://fmt.dev/
[15]: https://docs.conan.io/2/
