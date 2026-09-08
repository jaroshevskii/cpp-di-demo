#pragma once

#include "cppdi/Dependencies.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cppdi::app {

/// Reference application: a tiny dice CLI.
///
/// Every class here receives an `AppContext` and pulls exactly the
/// dependencies it needs out of it. It never constructs its own generators
/// or loggers, which is what makes the whole stack trivially replaceable in
/// tests (see `AppContext::test`).

/// Rolls a fair d6 and logs the outcome.
class DiceRoller {
public:
  explicit DiceRoller(AppContext context)
      : rng{context.dependencies.get<IRandomGenerator>()},
        logger{context.dependencies.get<ILogger>()} {}

  int roll() {
    const int value = rng->nextInt(1, 6);
    logger->info("Rolled: " + std::to_string(value));
    return value;
  }

  std::vector<int> rollMany(std::size_t count) {
    std::vector<int> values;
    values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      values.push_back(roll());
    }
    return values;
  }

private:
  std::shared_ptr<IRandomGenerator> rng;
  std::shared_ptr<ILogger> logger;
};

/// Same dice, but rolled on a background thread.
///
/// Deterministic parallelism: every task gets a *private* RNG seeded from
/// `baseSeed + slot` (see `AppContext::forkForAsync`), so the outcome of
/// `rollParallel(N)` is fully reproducible regardless of thread scheduling.
///
/// Race-freedom: the worker task captures a copy of a `DiceRoller` by value
/// — never `this` — so the `AsyncDiceRoller` can be destroyed before the
/// future completes without dangling.
class AsyncDiceRoller {
public:
  explicit AsyncDiceRoller(AppContext injectedContext) : context{std::move(injectedContext)} {}

  AsyncDiceRoller(const AsyncDiceRoller &) = delete;
  AsyncDiceRoller &operator=(const AsyncDiceRoller &) = delete;

  AsyncDiceRoller(AsyncDiceRoller &&other) noexcept
      : context{std::move(other.context)}, nextSlot{other.nextSlot.load()} {}

  AsyncDiceRoller &operator=(AsyncDiceRoller &&other) noexcept {
    if (this != &other) {
      context = std::move(other.context);
      nextSlot.store(other.nextSlot.load());
    }
    return *this;
  }

  std::future<int> rollAsync() {
    const std::uint64_t slot = nextSlot.fetch_add(1, std::memory_order_relaxed);
    DiceRoller roller{context.forkForAsync(slot)};
    return std::async(std::launch::async, [roller]() mutable { return roller.roll(); });
  }

  std::vector<int> rollParallel(std::size_t count) {
    std::vector<std::future<int>> futures;
    futures.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      futures.push_back(rollAsync());
    }

    std::vector<int> values;
    values.reserve(count);
    for (auto &future : futures) {
      values.push_back(future.get());
    }
    return values;
  }

private:
  AppContext context;
  std::atomic<std::uint64_t> nextSlot{0};
};

/// Generates a random lowercase string of the requested length.
class RandomStringGenerator {
public:
  explicit RandomStringGenerator(AppContext context)
      : rng{context.dependencies.get<IRandomGenerator>()} {}

  std::string generate(std::size_t length) {
    constexpr std::string_view kAlphabet = "abcdefghijklmnopqrstuvwxyz";

    std::string out;
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
      const int idx = rng->nextInt(0, static_cast<int>(kAlphabet.size()) - 1);
      out.push_back(kAlphabet[static_cast<std::size_t>(idx)]);
    }
    return out;
  }

private:
  std::shared_ptr<IRandomGenerator> rng;
};

} // namespace cppdi::app