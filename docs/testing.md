# Testing

`cppdi` is designed so the *entire* program becomes deterministic and
observable in tests — with the same production code path. This page shows the
patterns, why they hold, and how to keep them from becoming flaky.

Run everything:

```bash
cmake -B build && cmake --build build --parallel
ctest --test-dir build --output-on-failure     # 31 cases
./build/tests                                  # same suite, richer output
```

---

## 1. Two wiring functions — everything else is equal

The only difference between production and a test is **which implementations
get registered**:

```cpp
// production
auto ctx = cppdi::AppContext::live();          // ThreadLocalRandomGenerator, ConsoleLogger

// test
auto ctx = cppdi::AppContext::test(42u);       // DeterministicGenerator(42), TestLogger
```

Your code (`DiceRoller`, ...) does not branch on this. It just calls
`get<IRandomGenerator>()` and `get<ILogger>()`. That single fact is why
everything below is possible.

## 2. Determinism: same seed, same output

```cpp
TEST_CASE("DiceRoller is deterministic for the same seed") {
    auto makeRoller = [](std::uint64_t seed) {
        return app::DiceRoller{AppContext::test(seed)};
    };
    auto a = makeRoller(42u);
    auto b = makeRoller(42u);
    for (int i = 0; i < 20; ++i) {
        REQUIRE(a.roll() == b.roll());
    }
}
```

**Why it works:** both contexts register `DeterministicGenerator(42)`. The
C++ standard guarantees `std::mt19937` with the same seed and the same
`std::uniform_int_distribution` calls yields the same sequence — across
compilers, platforms, and runs.

**Golden rule:** never seed from `std::random_device` or the clock in a test.

## 3. Override exactly one dependency

Say a test wants the real RNG but *no logging*:

```cpp
auto ctx = AppContext::test(42u);
ctx.dependencies.provide<ILogger, NullLogger>();     // only the logger changes

app::DiceRoller roller{ctx};                         // RNG untouched
```

Or capture logs while keeping a specific seed:

```cpp
auto ctx = AppContext::test(7u);
auto &logs = dynamic_cast<TestLogger &>(*ctx.dependencies.get<ILogger>());
roller.roll();
REQUIRE(logs.size() == 1u);
```

## 4. Observable side effects

Because `TestLogger` *records* rather than writes, assertions can inspect the
actual behavior:

```cpp
REQUIRE_THAT(logs.copyLogs().at(0), Catch::Matchers::ContainsSubstring("Rolled"));
```

`TestLogger::copyLogs()` is a thread-safe snapshot; never read its internals
directly from another thread.

## 5. Concurrency tests must assert from the main thread

Catch2 tracks assertion state per-thread only in a limited way. Do **not**
call `REQUIRE` from worker threads. The suite's convention:

```cpp
std::atomic<bool> allValid{true};
std::vector<std::thread> threads;
for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&rng, &allValid] {
        for (int i = 0; i < 5000; ++i) {
            const int value = rng.nextInt(1, 6);
            if (value < 1 || value > 6) allValid.store(false);   // report, don't assert
        }
    });
}
for (auto &t : threads) t.join();
REQUIRE(allValid.load());                                        // assert from main thread
```

This keeps the test deterministic and Catch2 happy.

## 6. Deterministic parallelism — and the trap to avoid

The suite contains a test that is very easy to get *wrong*:

```cpp
TEST_CASE("AsyncDiceRoller is repeatable for the same seed") {
    auto a = AsyncDiceRoller{AppContext::test(42u)};
    auto b = AsyncDiceRoller{AppContext::test(42u)};
    REQUIRE(a.rollParallel(100) == b.rollParallel(100));
}
```

**The trap:** if `rollParallel` used one shared `DeterministicGenerator` for
all 100 tasks, the sequence would be deterministic *in total*, but which task
pulls which draw is decided by the OS thread scheduler — nondeterministic
interleaving ⇒ different result vectors run-to-run. The test would flake.

**The fix:** `AsyncDiceRoller` gives task *k* a **private** engine seeded
`baseSeed + k` (see `AppContext::forkForAsync`). Task k always draws exactly
its first value from a fixed seed, so the vector `[task0, task1, …]` is a
pure function of `baseSeed`. Scheduling order is irrelevant.

## 7. Property-ish tests with tiny seeds

Change the seed in one line to shake out corner cases:

```cpp
AppContext::test(1u);   // ~6 sequences to eyeball
AppContext::test(42u);  // default
AppContext::test(0u);
```

## 8. Running under sanitizers

CI runs the full suite under **AddressSanitizer + UndefinedBehaviorSanitizer**
on Linux (`ubuntu-latest`).

> Note: AppleClang on macOS has known ASan/UBSan runtime quirks (link/start-up
> hangs). Sanitizer verification is therefore authoritative on Linux; on macOS
> prefer Debug + `-Wall -Wextra` builds.

Local equivalent (Linux or WSL):

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
```

## 9. Keeping tests deterministic — checklist

- [ ] Never `random_device` / `time()` / thread id in seeds.
- [ ] Pass a seed explicitly; the default `42u` is a fallback, not an excuse.
- [ ] Assertions from the main thread only.
- [ ] Prefer `forkForAsync(slot)` over a shared generator for parallel work.
- [ ] After any RNG API change, re-run `ctest` a few times:
      `for i in $(seq 1 10); do ./build/tests; done`