#include <benchmark/benchmark.h>

#include <fstream>

namespace {

void ufw_startup_benchmark(benchmark::State& state) {
    std::ofstream out("/dev/null");

    while (state.KeepRunning())
    {
        out << "hello, world\n";
    }
}

BENCHMARK(ufw_startup_benchmark);

} // local namespace
