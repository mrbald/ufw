Benchmarks
==========

Google Benchmark micro-benchmarks for the `ufw_core` primitives. Each domain's
benchmarks live in `ufw-<domain>-benchmarks.cpp` and register names prefixed by
`<domain>_` (e.g. `mem_*`, later `ring_*`).

Recording numbers with the code
-------------------------------

Performance numbers are committed **in the source file**, in a delimited block
at the bottom, so the number delta shows up in `git diff` right next to the code
delta. Regenerate after a change with `tools/bench.py`:

```sh
# numbers from a Debug/ASan build are noise — use an optimized build:
cmake --preset conan-release -DUFW_ENABLE_CLANG_TIDY=OFF
cmake --build --preset conan-release --target ufw_benchmarks -j
python3 tools/bench.py mem            # rewrites the result block in ufw-mem-benchmarks.cpp
```

Commit the updated source alongside the change. The recorded block notes the
platform (results are machine-specific — Apple Silicon vs x86 differ; treat the
committed numbers as the author's reference, and rely on relative deltas).
