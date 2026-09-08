#include "cppdi/Random.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace cppdi;

TEST_CASE("DeterministicGenerator produces values in range") {
  DeterministicGenerator rng{42u};
  for (int i = 0; i < 10'000; ++i) {
    REQUIRE(rng.nextInt(1, 6) >= 1);
    REQUIRE(rng.nextInt(1, 6) <= 6);
  }
}

TEST_CASE("DeterministicGenerator is reproducible for the same seed") {
  DeterministicGenerator a{42u};
  DeterministicGenerator b{42u};
  for (int i = 0; i < 100; ++i) {
    REQUIRE(a.nextInt(1, 6) == b.nextInt(1, 6));
  }
}

TEST_CASE("DeterministicGenerator diverges across seeds") {
  DeterministicGenerator a{1u};
  DeterministicGenerator b{2u};

  bool diverged = false;
  for (int i = 0; i < 1000; ++i) {
    if (a.nextInt(1, 1'000'000) != b.nextInt(1, 1'000'000)) {
      diverged = true;
      break;
    }
  }
  REQUIRE(diverged);
}

TEST_CASE("DeterministicGenerator validates its range") {
  DeterministicGenerator rng{42u};
  REQUIRE_THROWS_AS(rng.nextInt(10, 1), std::invalid_argument);
}

TEST_CASE("ThreadLocalRandomGenerator produces values in range") {
  ThreadLocalRandomGenerator rng;
  for (int i = 0; i < 10'000; ++i) {
    REQUIRE(rng.nextInt(1, 6) >= 1);
    REQUIRE(rng.nextInt(1, 6) <= 6);
  }
}

TEST_CASE("ThreadLocalRandomGenerator validates its range") {
  ThreadLocalRandomGenerator rng;
  REQUIRE_THROWS_AS(rng.nextInt(5, 1), std::invalid_argument);
}

TEST_CASE("DeterministicGenerator is safe under concurrent use") {
  // A single instance shared by 8 threads: the internal mutex must keep
  // results inside [1, 6] with no torn/duplicated state. Assert only from
  // the main thread, so workers just report a flag.
  DeterministicGenerator rng{42u};

  std::atomic<bool> allValid{true};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&rng, &allValid] {
      for (int i = 0; i < 5000; ++i) {
        const int value = rng.nextInt(1, 6);
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