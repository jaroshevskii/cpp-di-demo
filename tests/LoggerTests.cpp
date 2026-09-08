#include "cppdi/Logger.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <thread>
#include <vector>

using namespace cppdi;

TEST_CASE("TestLogger records structurally-tagged messages") {
  TestLogger logger;
  logger.info("hello");
  logger.warn("careful");
  logger.error("boom");

  const auto logs = logger.copyLogs();
  REQUIRE(logger.size() == 3u);

  REQUIRE_THAT(logs.at(0), Catch::Matchers::StartsWith("[INFO]"));
  REQUIRE_THAT(logs.at(0), Catch::Matchers::ContainsSubstring("hello"));
  REQUIRE_THAT(logs.at(1), Catch::Matchers::StartsWith("[WARN]"));
  REQUIRE_THAT(logs.at(2), Catch::Matchers::StartsWith("[ERROR]"));
}

TEST_CASE("TestLogger clear() empties the recorded entries") {
  TestLogger logger;
  logger.info("one");
  logger.clear();
  REQUIRE(logger.size() == 0u);
}

TEST_CASE("NullLogger discards everything") {
  NullLogger logger;
  logger.info("x");
  logger.warn("x");
  logger.error("x"); // must not crash; nothing to assert
  SUCCEED();
}

TEST_CASE("TestLogger is safe under concurrent use") {
  // Every one of the 1000 writes must appear exactly once.
  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kLocalWrites = 125;

  TestLogger logger;
  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < kThreads; ++t) {
    threads.emplace_back([&logger] {
      for (std::size_t i = 0; i < kLocalWrites; ++i) {
        logger.info("line");
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  REQUIRE(logger.size() == kThreads * kLocalWrites);
}