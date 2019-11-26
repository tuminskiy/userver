#include <benchmark/benchmark.h>

#include <thread>

#include <engine/async.hpp>
#include <engine/run_in_coro.hpp>
#include <utils/async.hpp>

// Note: We intentionally do not run this benchmark from RunInCoro to avoid
// any side-effects (RunInCoro spawns additional std::threads and uses some
// synchronization primitives).
void async_comparisons_std_thread(benchmark::State& state) {
  std::size_t constructed_joined_count = 0;
  for (auto _ : state) {
    std::thread([] {}).join();
    ++constructed_joined_count;
  }

  state.SetItemsProcessed(constructed_joined_count);
}
BENCHMARK(async_comparisons_std_thread);

void async_comparisons_coro(benchmark::State& state) {
  RunInCoro(
      [&]() {
        std::size_t constructed_joined_count = 0;
        for (auto _ : state) {
          engine::impl::Async([] {}).Wait();
          ++constructed_joined_count;
        }

        state.SetItemsProcessed(constructed_joined_count);
      },
      state.range(0));
}
BENCHMARK(async_comparisons_coro)->RangeMultiplier(2)->Range(1, 32);

void async_comparisons_coro_spanned(benchmark::State& state) {
  RunInCoro(
      [&]() {
        std::size_t constructed_joined_count = 0;
        for (auto _ : state) {
          utils::Async("", [] {}).Wait();
          ++constructed_joined_count;
        }

        state.SetItemsProcessed(constructed_joined_count);
      },
      state.range(0));
}
BENCHMARK(async_comparisons_coro_spanned)->RangeMultiplier(2)->Range(1, 32);