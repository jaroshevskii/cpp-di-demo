#include "DiceApp.h"
#include "cppdi/Dependencies.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <vector>

using namespace cppdi;
namespace app = cppdi::app;

TEST_CASE("DiceRoller produces values in 1..6") {
  auto ctx = AppContext::test(42u);
  app::DiceRoller roller{ctx};

  for (int i = 0; i < 5'000; ++i) {
    REQUIRE(roller.roll() >= 1);
    REQUIRE(roller.roll() <= 6);
  }
}

TEST_CASE("DiceRoller is deterministic for the same seed") {
  auto makeRoller = [](std::uint64_t seed) { return app::DiceRoller{AppContext::test(seed)}; };

  auto a = makeRoller(42u);
  auto b = makeRoller(42u);

  for (int i = 0; i < 20; ++i) {
    REQUIRE(a.roll() == b.roll());
  }
}

TEST_CASE("DiceRoller diverges for different seeds") {
  auto a = app::DiceRoller{AppContext::test(1u)};
  auto b = app::DiceRoller{AppContext::test(2u)};

  bool diverged = false;
  for (int i = 0; i < 100; ++i) {
    if (a.roll() != b.roll()) {
      diverged = true;
      break;
    }
  }
  REQUIRE(diverged);
}

TEST_CASE("DiceRoller.log logs every roll through the injected ILogger") {
  auto ctx = AppContext::test(42u);
  app::DiceRoller roller{ctx};

  roller.roll();
  roller.roll();

  auto &logs = dynamic_cast<TestLogger &>(*ctx.dependencies.get<ILogger>());
  REQUIRE(logs.size() == 2u);
  REQUIRE_THAT(logs.copyLogs().at(0), Catch::Matchers::ContainsSubstring("Rolled"));
}

TEST_CASE("AsyncDiceRoller rolls valid values") {
  auto ctx = AppContext::test(42u);
  app::AsyncDiceRoller roller{ctx};

  const int value = roller.rollAsync().get();
  REQUIRE(value >= 1);
  REQUIRE(value <= 6);
}

TEST_CASE("AsyncDiceRoller is repeatable for the same seed") {
  auto makeRoller = [](std::uint64_t seed) { return app::AsyncDiceRoller{AppContext::test(seed)}; };

  auto a = makeRoller(42u);
  auto b = makeRoller(42u);

  // Per-task slot seeds make parallel runs reproducible no matter how the
  // OS schedules the worker threads.
  const auto valuesA = a.rollParallel(100);
  const auto valuesB = b.rollParallel(100);
  REQUIRE(valuesA == valuesB);
}

TEST_CASE("AsyncDiceRoller hands back the requested number of results") {
  auto ctx = AppContext::test(42u);
  app::AsyncDiceRoller roller{ctx};

  const auto values = roller.rollParallel(64);
  REQUIRE(values.size() == 64u);
}

TEST_CASE("AsyncDiceRoller is thread-safe under parallel stress") {
  // 8 independent AsyncDiceRollers, each shared by its own rollParallel.
  auto makeRoller = [](std::uint64_t seed) { return app::AsyncDiceRoller{AppContext::test(seed)}; };

  std::vector<app::AsyncDiceRoller> rollers;
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    rollers.emplace_back(makeRoller(seed));
  }

  // All 800 rolls (100 per roller) must stay in the valid range.
  for (auto &roller : rollers) {
    for (const int value : roller.rollParallel(100)) {
      REQUIRE(value >= 1);
      REQUIRE(value <= 6);
    }
  }
}

TEST_CASE("RandomStringGenerator honors the requested length") {
  auto ctx = AppContext::test(42u);
  app::RandomStringGenerator generator{ctx};

  REQUIRE(generator.generate(0u).empty());
  REQUIRE(generator.generate(1u).size() == 1u);
  REQUIRE(generator.generate(100u).size() == 100u);
}

TEST_CASE("RandomStringGenerator produces only lowercase letters") {
  auto ctx = AppContext::test(42u);
  app::RandomStringGenerator generator{ctx};

  const auto text = generator.generate(10'000u);
  for (const char c : text) {
    REQUIRE(c >= 'a');
    REQUIRE(c <= 'z');
  }
}

TEST_CASE("RandomStringGenerator is deterministic for the same seed") {
  auto makeGenerator = [](std::uint64_t seed) {
    return app::RandomStringGenerator{AppContext::test(seed)};
  };

  auto a = makeGenerator(42u);
  auto b = makeGenerator(42u);

  REQUIRE(a.generate(50u) == b.generate(50u));
}