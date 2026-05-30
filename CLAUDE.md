# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

µFW (uFW) is a minimalist C++ framework for prototyping Unix-like server-side applications. It provides a small dependency-injection container with YAML-driven configuration, ordered lifecycle phases, and shared-library plugin loading. The runtime is a single binary (`ufw_launcher`) that bootstraps an `application` from a YAML file.

## Build

The repo targets CMake (>=3.27), C++23, and Conan 2 for dependency management.

- `conanfile.txt` declares `boost/1.86.0`, `fmt/11.2.0`, `quill/11.1.0`, `yaml-cpp/0.8.0`, `benchmark/1.8.4`, with `boost/*:shared=True`. Generators: `CMakeDeps` + `CMakeToolchain`. Layout: `cmake_layout`.
- Top-level `CMakeLists.txt` exposes four options:
  - `UFW_ENABLE_SANITIZERS` (default `ON` in Debug, `OFF` otherwise) — wires asan + ubsan via the `ufw::sanitizers` interface lib, linked PRIVATE on every first-party target. Uniformity matters because plugins are `dlopen`'d and ASan presence must match across the launcher and the plugin.
  - `UFW_ENABLE_CLANG_TIDY` (default `ON`) — sets `CXX_CLANG_TIDY` per-target on `ufw_app` and `ufw_topics`; silently skipped if `clang-tidy` is not in `PATH`.
  - `UFW_USE_CCACHE` (default `OFF`) — opts in to a `ccache` compiler launcher.
  - `UFW_BUILD_PYTHON` (default `OFF`) — builds the nanobind Python control-plane module (`ufw/py/`). Requires `nanobind` (`pip install nanobind`) and Python dev headers; **build it with `-DUFW_ENABLE_SANITIZERS=OFF`** since a stock CPython can't import an ASan module without preloading the runtime.
- Warnings (`-pedantic -Wall -Wextra -Werror`) live on the `ufw::warnings` INTERFACE lib, linked PRIVATE only on first-party targets so Conan-imported headers don't trip `-Werror`.
- A curated `.clang-tidy` baseline is shipped at the repo root; the `HeaderFilterRegex` restricts analysis to first-party headers.

Typical build:

```sh
conan install . -s build_type=Debug --build=missing
cmake --preset conan-debug
cmake --build --preset conan-debug -j
```

(For Release: swap `Debug`/`conan-debug` → `Release`/`conan-release`.)

Custom targets:
- `cmake --build --preset conan-debug --target unit-test` — Boost.Test suite (`tests/`).
- `cmake --build --preset conan-debug --target benchmark` — Google Benchmark suite (`benchmarks/`).
- `cmake --build --preset conan-debug --target tidy` (or `tidy-fix`) — runs `tools/lint.py`, the clang-tidy harness. Unlike the per-target `CXX_CLANG_TIDY` integration, it also lints every first-party header **as a standalone TU**, so findings in template-only headers (e.g. `entity_ref<T>`, never instantiated by a `.cpp`) are caught. `--json` emits a machine-readable summary; `--export-fixes <dir>` dumps clang-tidy's raw YAML. Needs `clang-tidy` on `PATH` (brew llvm is keg-only).
- `cmake --install build/Debug --prefix <path>` — installs libs/headers/binaries plus `UfwConfig.cmake` / `UfwTargets.cmake` for downstream consumers (`find_package(Ufw)` → `ufw::ufw_app`, `ufw::ufw_topics`, etc.).

## Running the launcher

```
ufw_launcher -c <config>.yaml     # default: config.yaml
```

`examples/app.yaml` is a working sample (note: the example uses `libexample.dylib` for macOS; switch to `libexample.so` on Linux).

## Python control plane (nanobind)

`ufw/py/module.cpp` is an optional nanobind module (`import ufw`) that lets Python *assemble and drive* an application instead of the YAML launcher — "control plane" in the control/data-plane sense (Python orchestrates; the data plane stays C++). It binds `ufw.application` with `add_loader(name, ufw.Loader.LIBRARY|PLUGIN)`, `load(spec_dict)`, `run()` (GIL released while the io_context runs), and `shutdown()`. The spec dict is converted to a `YAML::Node` and decoded through the *same* `convert<application_config>` path the launcher uses, so plugins and the ABI gate behave identically; a C++ `fatal_error` surfaces as a Python `RuntimeError`. `examples/app.py` mirrors `examples/app.yaml`.

Build + run (separate asan-off dir; dev deps in a local `.venv`):

```sh
python3 -m venv .venv && .venv/bin/pip install nanobind
cmake -S . -B build/Py \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/Debug/generators/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Debug -DUFW_ENABLE_SANITIZERS=OFF -DUFW_BUILD_PYTHON=ON \
  -DPython_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build/Py --target ufw_py example -j
PYTHONPATH=build/Py/ufw/py DYLD_LIBRARY_PATH=build/Py/ufw/app .venv/bin/python examples/app.py
```

## Architecture

The framework is small enough that the whole runtime model lives in `ufw/app/`.

### Core abstractions (read together to understand control flow)
- `entity` (`entity.hpp`) — identifiable building block. Has `entity_id` (string) and `resolved_entity_id` (index). Every framework object inherits this.
- `application` (`application.hpp` / `application.cpp`) — the container. Owns `boost::asio::io_context`, the entity vector, the `entity_id → index` map, and the lifecycle-participants list. After config load, `structure_locked_` becomes true and no further entity registration is allowed.
- `lifecycle_participant` (`lifecycle_participant.hpp`) — opt-in interface with phases `init → start → up → stop → fini`. The application iterates participants in declaration order for `init/start/up`, reverses for `stop/fini`.
- `loader` (`loader.hpp`) and `default_loader` (anonymous in `application.cpp`) — entities that produce other entities. The `default_loader` (id `""`) holds a registry of `loader_func_t` factories indexed by entity ID.
- `library_repository` / `plugin_repository` (`library_repository.hpp`, `plugin_repository.hpp`) — built-in loaders. `LIBRARY` does `dlopen`; `PLUGIN` resolves a `library_ref` and calls a named `extern "C"` constructor symbol returning `entity*`. Both are added by `main.cpp` before `app.load()`.

### Bootstrap sequence (`ufw/app/main.cpp` → `application.cpp`)
1. `initialize_logger()` starts the Quill backend thread.
2. Construct `application` — its constructor registers itself, the `default_loader` with id `""`, and a built-in `LOGGER` loader function that decodes the YAML config into `logger_config` and forwards to `configure_logger`.
3. `main` programmatically registers `LIBRARY` and `PLUGIN` loaders.
4. `application::load(argc, argv)` parses `--config`, reads the YAML file, decodes `application:` into `application_config` (`configuration.hpp` defines yaml-cpp `convert<>` specializations via the `CFG_ENCODE`/`CFG_DECODE` macros), and for each `entity_config` calls `application::add(name, loader_ref, config)`. If `loader_ref` resolves to a registered entity, that entity's `loader::load` is called; otherwise the `default_loader`'s loader-function table is consulted.
5. After all entities are constructed, the application discovers `lifecycle_participant`s via `for_each<>`, locks the structure, then `run()` drives `init` → schedules `up` posts on the io_context → `start` → `context_.run()`. On SIGINT, `shutdown()` clears `work_` and stops the context, then `stop`/`fini` run in reverse order.

### Cross-cutting
- **Logging** (`logger.hpp`, `logger.cpp`) — Quill 11 backend with fmt-style format strings. Macros `LOG_DBG / LOG_INF / LOG_WRN / LOG_ERR` expand to `QUILL_LOG_*` and call `get_logger()` via unqualified lookup (entity member via `ENTITY_LOGGER`, or a free-function fallback that returns the root logger). Each entity owns a `quill::Logger*` named after its `entity_id`; the id renders in the log line via the `%(logger)` pattern token. `configure_logger(logger_config)` sets the root logger's severity and the pattern that newly-created entity loggers will pick up (Quill's pattern is fixed at logger creation).
- **Configuration** (`configuration.hpp`) — `config_t` is a `YAML::Node`. Use the `CFG_ENCODE`/`CFG_DECODE`/`CFG_*_IF_SET` macros when adding new typed config structs. `logger_config` is the in-tree example.
- **Entity references** — `entity_ref<T>` (in `entity.hpp`) is a lazy reference: store one during construction, call `resolve()` in `init()`. Strongly typed lookup goes through `application::get<T>(id)` which `dynamic_cast`s and throws `fatal_error` on mismatch.
- **Topics** (`ufw/topics/`) — separate library `ufw_topics` providing a topic-id scheme (payload-type id × subject id). Currently a stub — only the payload-id sequencer is implemented.

### Adding new things
- A new in-process entity type: register a typed factory via `app.register_loader<T, Cfg>("ID")` (template overload) or a raw `loader_func_t` for full control.
- A new plugin in a shared library: `#include "plugin.hpp"`, declare the constructor with `UFW_PLUGIN_ENTITY_CTOR(my_ctor) { ... }` (the canonical `entity_ctor_t` signature), and stamp the library **once** with `UFW_PLUGIN()`. Build a `SHARED` library that links `ufw_app`, then load it via `LIBRARY`/`PLUGIN` entries in YAML (see `examples/app.yaml` and `ufw/app/example.cpp`). The `LIBRARY` loader (`library_repository`) is a strict ABI gate: it refuses, with a `fatal_error`, any `dlopen`'d library that lacks the `UFW_PLUGIN()` stamp or whose stamped `UFW_VERSION` (from `version.hpp`, generated from `PROJECT_VERSION`) differs from the launcher's. A plugin is only loadable in the exact uFW version it was compiled against; in-binary entities from the static loader cross no ABI boundary and need no stamp.
- New entity ctor signature is fixed: `T(..., entity_id const&, resolved_entity_id, application&)` — anything you add must accept these as the trailing args. For plugins, `entity_ctor_t` in `plugin.hpp` is the single source of truth for this signature.

## CI

`.github/workflows/ci.yml` runs the matrix `[ubuntu-24.04 (gcc-13, clang-18), macos-14] × [Debug, Release]` with Conan caching and `ccache` via `hendrikmuhs/ccache-action`. There is no Travis config — `.travis.yml` was retired during the refresh.

## Working with me

The repo owner has deep C++ / distributed-systems experience; in this repo he is the domain authority. Work accordingly:

- **Spar, don't agree** — surface cracks with reasoning; a yes-man is useless here.
- **One hat at a time; name it** — finish a slice before starting another.
- **Right-size** — the cheapest thing that works wins; building or abstracting is a bias, not a default. Expect hard correction for over-engineering.
- **Don't roleplay expertise** — you're Claude: strong on distributed-systems patterns, modern C++, build tooling, and prior art; weaker on this framework's hard-won design calls. Attempt, expect correction, name your edges.
- **Capture corrections** — when a correction lands on something the owner knows cold, note it as a one-line candidate principle; these feed the model-agnostic tool catalog in the hats core (wired below).

<!-- hats:core -->
## Engineering core (hats)

This project uses the shared **hats engineering core**. Before substantive
work, read and follow `~/.hats/docs/USING.md`; it loads the hard rules
(`GUARDRAILS.md`), the engineering priors (`PRIORS.md`), and the validated
thinking tools. Re-read each session: the core is the source of truth and
its updates propagate here automatically. If `~/.hats` does not resolve,
the core is not linked on this machine (see the hats repo's README).
<!-- /hats:core -->
