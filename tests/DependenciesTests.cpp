#include "DiceApp.h"
#include "cppdi/Dependencies.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace cppdi;

namespace {
/// An interface with *no* `DependencyTraits` specialization, so accessing it
/// without a provider genuinely has no default to fall back to.
struct INoDefault {
  virtual ~INoDefault() = default;
  virtual void run() = 0;
};
} // namespace

TEST_CASE("provide() registers and get() retrieves an implementation") {
  Dependencies deps;
  deps.provide<IRandomGenerator, DeterministicGenerator>(42u);

  auto rng = deps.get<IRandomGenerator>();
  REQUIRE(rng != nullptr);
  REQUIRE(rng->nextInt(1, 6) >= 1);
  REQUIRE(rng->nextInt(1, 6) <= 6);
}

TEST_CASE("get() throws DependencyNotFoundError when unregistered and defaultless") {
  Dependencies deps;
  REQUIRE_THROWS_AS(deps.get<INoDefault>(), DependencyNotFoundError);
}

TEST_CASE("contains() and tryGet() report registration status without throwing") {
  Dependencies deps;
  REQUIRE_FALSE(deps.contains<IRandomGenerator>());
  REQUIRE(deps.tryGet<IRandomGenerator>() == nullptr);

  deps.provide<IRandomGenerator, DeterministicGenerator>();
  REQUIRE(deps.contains<IRandomGenerator>());
  REQUIRE(deps.tryGet<IRandomGenerator>() != nullptr);
}

TEST_CASE("provide() replaces an existing registration (override)") {
  Dependencies deps;
  deps.provide<IRandomGenerator, DeterministicGenerator>(1u);
  const int first = deps.get<IRandomGenerator>()->nextInt(1, 1000);

  deps.provide<IRandomGenerator, DeterministicGenerator>(2u);
  const int second = deps.get<IRandomGenerator>()->nextInt(1, 1000);

  REQUIRE_FALSE(first == second);
}

TEST_CASE("clone() shares the same service instances") {
  Dependencies a;
  a.provide<IRandomGenerator, DeterministicGenerator>(7u);

  Dependencies b = a.clone();

  // Same underlying object: seeding through one clone is visible in the
  // other.
  b.get<IRandomGenerator>()->seed(123u);
  REQUIRE(a.get<IRandomGenerator>()->nextInt(1, 1000) ==
          DeterministicGenerator{123u}.nextInt(1, 1000));
}

TEST_CASE("Dependencies is safe for concurrent lookups") {
  Dependencies deps;
  deps.provide<IRandomGenerator, DeterministicGenerator>(42u);

  std::atomic<bool> allValid{true};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&deps, &allValid] {
      for (int i = 0; i < 1000; ++i) {
        auto rng = deps.get<IRandomGenerator>();
        const int value = rng->nextInt(1, 6);
        if (value < 1 || value > 6) {
          allValid.store(false);
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  REQUIRE(allValid.load());
}

TEST_CASE("AppContext::test() wires a deterministic, observable stack") {
  auto ctx = AppContext::test(42u);
  REQUIRE(ctx.dependencies.contains<IRandomGenerator>());
  REQUIRE(ctx.dependencies.contains<ILogger>());
}

TEST_CASE("AppContext::live() wires a production stack") {
  auto ctx = AppContext::live();
  REQUIRE(ctx.dependencies.contains<IRandomGenerator>());
  REQUIRE(ctx.dependencies.contains<ILogger>());
}

TEST_CASE("AppContext::clone() produces a working graph for app code") {
  auto ctx = AppContext::test(42u);
  auto clone = ctx.clone();

  app::DiceRoller rollerA{ctx};
  app::DiceRoller rollerB{clone};

  for (int i = 0; i < 25; ++i) {
    REQUIRE(rollerA.roll() >= 1);
    REQUIRE(rollerA.roll() <= 6);
    REQUIRE(rollerB.roll() >= 1);
    REQUIRE(rollerB.roll() <= 6);
  }
}