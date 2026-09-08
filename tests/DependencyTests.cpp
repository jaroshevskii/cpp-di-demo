#include "DiceApp.h"
#include "cppdi/Dependency.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using namespace cppdi;

namespace {

/// An app-level interface demonstrating the `DependencyKey` pattern: give it
/// distinct live + test defaults, then it needs *zero* wiring anywhere.
struct IGreeter {
  virtual ~IGreeter() = default;
  virtual std::string greeting() const = 0;
};

struct LiveGreeter final : IGreeter {
  std::string greeting() const override { return "live"; }
};

struct TestGreeter final : IGreeter {
  std::string greeting() const override { return "test"; }
};

struct EmptyGreeter final : IGreeter {
  std::string greeting() const override { return ""; }
};

/// A count-tracking client used to prove defaults are built once and cached.
struct IToken {
  virtual ~IToken() = default;
  virtual int value() const = 0;
};

int &tokenBuildCount() {
  static int count = 0;
  return count;
}

struct CountingToken final : IToken {
  int value() const override { return 7; }
};

} // namespace

template <> struct DependencyTraits<IGreeter> {
  static std::shared_ptr<IGreeter> live() { return std::make_shared<LiveGreeter>(); }
  static std::shared_ptr<IGreeter> test() { return std::make_shared<TestGreeter>(); }
};

template <> struct DependencyTraits<IToken> {
  static std::shared_ptr<IToken> live() {
    ++tokenBuildCount();
    return std::make_shared<CountingToken>();
  }
  static std::shared_ptr<IToken> test() {
    ++tokenBuildCount();
    return std::make_shared<CountingToken>();
  }
};

TEST_CASE("live context falls back to DependencyTraits::live() and caches it") {
  Dependencies deps;
  auto a = deps.get<IGreeter>();
  auto b = deps.get<IGreeter>();

  REQUIRE(a->greeting() == "live");
  REQUIRE(a == b); // same instance: built once, cached like DependencyValues
}

TEST_CASE("test context falls back to DependencyTraits::test()") {
  Dependencies deps{DependencyContext::Test};
  REQUIRE(deps.get<IGreeter>()->greeting() == "test");
}

TEST_CASE("a provided value wins over the trait default") {
  Dependencies deps{DependencyContext::Test};
  deps.provide<IGreeter, EmptyGreeter>();
  REQUIRE(deps.get<IGreeter>()->greeting() == "");
  REQUIRE(deps.contains<IGreeter>()); // explicitly provided
}

TEST_CASE("remove() restores the trait default") {
  Dependencies deps;
  deps.provide<IGreeter, EmptyGreeter>();
  REQUIRE(deps.get<IGreeter>()->greeting() == "");

  deps.remove<IGreeter>();
  REQUIRE_FALSE(deps.contains<IGreeter>());
  REQUIRE(deps.get<IGreeter>()->greeting() == "live");
}

TEST_CASE("defaults are built exactly once (lazy + memoized)") {
  Dependencies deps;
  deps.get<IToken>();
  deps.get<IToken>();
  deps.clone().get<IToken>(); // shared registry, still the same instance
  REQUIRE(tokenBuildCount() == 1);
}

TEST_CASE("a live dependency used in a test context fails loudly") {
  // Like Swift's `.unimplemented` test values: IRandomGenerator has no test()
  // default, so a deterministic test cannot silently use real entropy.
  Dependencies deps{DependencyContext::Test};
  REQUIRE_THROWS_AS(deps.get<IRandomGenerator>(), DependencyNotFoundError);
}

TEST_CASE("built-in live defaults resolve without any wiring") {
  Dependencies deps;
  REQUIRE(deps.get<IRandomGenerator>() != nullptr);
  REQUIRE(deps.get<ILogger>() != nullptr);
  REQUIRE(deps.get<IRandomGenerator>() == deps.get<IRandomGenerator>());
  REQUIRE(deps.get<ILogger>() == deps.get<ILogger>());
}

TEST_CASE("Dependency<T> resolves through the current values") {
  Dependency<IGreeter> greeter;
  REQUIRE(greeter->greeting() == "live");
  REQUIRE((*greeter).greeting() == "live");

  auto held = greeter.get();
  REQUIRE(held->greeting() == "live");
}

TEST_CASE("Dependency<T> can target a specific container") {
  Dependencies deps;
  deps.provide<IGreeter, TestGreeter>();
  Dependency<IGreeter> greeter{deps};
  REQUIRE(greeter->greeting() == "test");
}

TEST_CASE("withDependencies overrides values for its scope only") {
  Dependency<IGreeter> greeter;
  REQUIRE(greeter->greeting() == "live");

  withDependencies([](Dependencies &deps) { deps.provide<IGreeter, TestGreeter>(); },
                   [&] {
                     REQUIRE(greeter->greeting() == "test");

                     withDependencies(
                         [](Dependencies &deps) { deps.provide<IGreeter, EmptyGreeter>(); },
                         [&] { REQUIRE(greeter->greeting() == ""); });

                     // Back to the enclosing scope's value.
                     REQUIRE(greeter->greeting() == "test");
                   });

  // Back to the caller's value.
  REQUIRE(greeter->greeting() == "live");
}

TEST_CASE("withDependencies starts from the current values") {
  // Values installed before the scope are visible inside it.
  auto outer = bindDependencies(AppContext::test(42u).dependencies);
  Dependency<ILogger> logger;

  REQUIRE(dynamic_cast<TestLogger *>(&*logger) != nullptr);

  withDependencies([](Dependencies &deps) { deps.provide<ILogger, NullLogger>(); },
                   [&] { REQUIRE(dynamic_cast<NullLogger *>(&*logger) != nullptr); });

  REQUIRE(dynamic_cast<TestLogger *>(&*logger) != nullptr);
}

TEST_CASE("withDependencies returns the operation result and restores on throw") {
  Dependency<IGreeter> greeter;

  const int result =
      withDependencies([](Dependencies &deps) { deps.provide<IGreeter, TestGreeter>(); },
                       [&] {
                         REQUIRE(greeter->greeting() == "test");
                         return 42;
                       });
  REQUIRE(result == 42);

  REQUIRE_THROWS(withDependencies([](Dependencies &) {}, [] { throw std::runtime_error("boom"); }));
  REQUIRE(greeter->greeting() == "live");
}

TEST_CASE("withDependencies can swap logging deterministically") {
  withDependencies([](Dependencies &deps) { deps.provide<ILogger, TestLogger>(); },
                   [&] {
                     auto logger = Dependency<ILogger>().get();
                     auto &logs = dynamic_cast<TestLogger &>(*logger);
                     logger->info("deterministic");
                     REQUIRE(logs.copyLogs().size() == 1u);
                     REQUIRE_THAT(logs.copyLogs().at(0),
                                  Catch::Matchers::ContainsSubstring("deterministic"));
                   });
}

TEST_CASE("scoped values are thread-local") {
  auto ctx = AppContext::test(7u); // TestLogger + seeded RNG
  {
    auto scope = bindDependencies(ctx.dependencies);

    Dependency<ILogger> logger;
    REQUIRE(dynamic_cast<TestLogger *>(&*logger) != nullptr);

    // A worker thread has its own scope: it falls back to the process
    // defaults (live) and must NOT see this thread's overlay.
    std::atomic<bool> workerSawLive{false};
    std::thread worker{[&workerSawLive] {
      workerSawLive.store(dynamic_cast<ConsoleLogger *>(&*Dependency<ILogger>()) != nullptr);
    }};
    worker.join();
    REQUIRE(workerSawLive.load());
  }
  (void)ctx;
}

TEST_CASE("bindDependencies powers components that resolve implicitly") {
  auto ctx = AppContext::test(42u);
  auto scope = bindDependencies(ctx.dependencies);

  // Default-constructed components resolve exactly the context's values.
  app::DiceRoller roller;
  REQUIRE(roller.roll() >= 1);
  REQUIRE(roller.roll() <= 6);

  app::RandomStringGenerator strings;
  REQUIRE(strings.generate(5u).size() == 5u);
}

TEST_CASE("prepareDependencies sets process-wide defaults and can be restored") {
  const Dependencies previous = defaultDependencies();
  prepareDependencies([](Dependencies &deps) { deps.provide<IGreeter, TestGreeter>(); });

  std::string seenOnWorker;
  std::thread worker{[&seenOnWorker] { seenOnWorker = Dependency<IGreeter>{}->greeting(); }};
  worker.join();
  REQUIRE(seenOnWorker == "test");

  setDefaultDependencies(previous);

  std::string restoredOnWorker;
  std::thread worker2{
      [&restoredOnWorker] { restoredOnWorker = Dependency<IGreeter>{}->greeting(); }};
  worker2.join();
  REQUIRE(restoredOnWorker == "live");
}