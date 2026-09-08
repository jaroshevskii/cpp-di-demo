# cppdi

[![CI](https://github.com/jaroshevskii/cpp-di-demo/actions/workflows/ci.yml/badge.svg)](https://github.com/jaroshevskii/cpp-di-demo/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blueviolet.svg)]()

A tiny, **header-only**, thread-safe dependency injection toolkit for modern
C++, inspired by
[The Composable Architecture](https://github.com/pointfreeco/swift-composable-architecture)’s
`DependencyValues`.

It gives you the same ergonomics Swift developers enjoy with `DependencyValues`:

- **one-line production wiring** — `AppContext::live()`;
- **one-line deterministic test wiring** — `AppContext::test(seed)`;
- **override a single dependency** without touching anything else;
- **deterministic parallelism** — every async task gets a private, seeded RNG,
  so results are reproducible no matter the thread schedule.

---

## Why?

Testing anything that uses randomness, time, or I/O is painful. In Swift, TCA
solves this with a dependency system that lets you swap the real world for a
predictable one in tests. `cppdi` brings that pattern to C++20.

Instead of writing this:

```cpp
// Production: real file, real RNG, real logging
class DiceRoller {
    std::mt19937 rng; // member, hard to test
};
```

you write this:

```cpp
class DiceRoller {
    DiceRoller(AppContext ctx)
        : rng(ctx.dependencies.get<IRandomGenerator>()),
          logger(ctx.dependencies.get<ILogger>()) {}
    // code only talks to interfaces — the world is injected
};
```

And in tests the *whole world* becomes deterministic with one seed:

```cpp
auto ctx = cppdi::AppContext::test(42); // seeded RNG, capturing logger
```

## Features

- **`Dependencies`** — type-safe, thread-safe, type-erased container
  (`provide` / `get` / `tryGet` / `contains` / `clone` / `detach`).
- **`AppContext`** — ready-made bindings: `live()` vs `test(seed)`.
- **Deterministic async** — `AsyncDiceRoller` proves parallel work is fully
  reproducible via per-task seeded engines (`forkForAsync`).
- **Thread-safety by design** — three independent layers:
  - container lookups are atomic;
  - `ThreadLocalRandomGenerator` is lock-free per thread;
  - `DeterministicGenerator` is mutex-guarded shared state.
- **Header-only** — drop `include/cppdi/` into your project. Zero dependencies.
- **Modern** — C++20, RAII, `std::atomic`, `std::shared_ptr`, CTAD.
- **Style** — Apple/LLVM conventions: 2-space indent (Swift-style), PascalCase
  files, Swift naming (`nextInt`, `rollAsync`, `forkForAsync`).

## Requirements

- CMake ≥ 3.20
- A C++20 compiler (GCC 11+, Clang 14+, MSVC 2022)
- (Tests only) network access on first configure to fetch
  [Catch2 v3.5.2](https://github.com/catchorg/Catch2)

## Quick start

```bash
git clone https://github.com/jaroshevskii/cpp-di-demo.git
cd cpp-di-demo

# Build + run the demo CLI
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/dice_cli --seed 42 --rolls 3 --parallel

# Build + run the test suite
ctest --test-dir build --output-on-failure

# 100% deterministic: same seed → same output, every time
./build/dice_cli --seed 42 --rolls 5
./build/dice_cli --seed 42 --rolls 5   # identical output
```

### CMake presets (Ninja)

```bash
cmake --preset develop     # Debug + compile_commands.json
cmake --build --preset develop
ctest --preset ci          # Release, warnings-as-errors
```

## Code style

`clang-format` and `clang-tidy` configs ship with the repo
(`.clang-format`, `.clang-tidy`). Check formatting with:

```bash
clang-format --dry-run --Werror include/cppdi/*.h examples/*.h examples/*.cpp tests/*.cpp
```

## How it works — 60 second tour

```cpp
#include <cppdi/Dependencies.h>
using namespace cppdi;

// 1. Production wiring
auto liveContext = AppContext::live();

// 2. Inject into your logic
app::DiceRoller roller{liveContext};
roller.roll(); // real randomness, logs to stdout

// 3. Test wiring — one seed makes everything reproducible
auto testContext = AppContext::test(42u);
app::DiceRoller testRoller{testContext};

// 4. Override a single dependency (e.g. silence logging) in a test
testContext.dependencies.provide<ILogger, NullLogger>();

// 5. Deterministic parallel rolls (each task gets its own seeded RNG)
app::AsyncDiceRoller async{testContext};
async.rollParallel(100); // same result vector every run
```

Full walkthrough: [docs/architecture.md](docs/architecture.md),
testing guide: [docs/testing.md](docs/testing.md).

## Project layout

```
include/cppdi/          The library (header-only)
  Dependencies.h        Dependencies + AppContext + DependencyNotFoundError
  Random.h              IRandomGenerator + thread-local / deterministic impls
  Logger.h              ILogger + console / test / null impls
examples/               Reference application that uses the library
  DiceApp.h             DiceRoller, AsyncDiceRoller, RandomStringGenerator
  DiceCli.cpp           CLI entry point (--seed / --rolls / --parallel)
tests/                  Catch2 test suite (31 cases)
docs/                   architecture.md, testing.md
.github/workflows/      CI: Linux GCC/Clang, macOS, Windows, ASan+UBSan
cmake/                  CompileSettings.cmake (warning policy)
```

## Thread-safety model

| Layer                        | Strategy                                                            |
| ---------------------------- | ------------------------------------------------------------------- |
| Container lookup/registration | internal mutex, atomic swap of erased `shared_ptr`                  |
| `ThreadLocalRandomGenerator` | per-thread `thread_local` engine — zero contention                   |
| `DeterministicGenerator`     | mutex-guarded shared engine — safe across threads (tests)           |
| Async tasks                  | task captures state **by value**, never `this`                      |
| Parallel determinism         | `forkForAsync(slot)` → private engine seeded `baseSeed + slot`      |

## Testing

31 test cases / ~72k assertions, all deterministic, all run in CI on three
compilers plus a Linux ASan+UBSan job:

- deterministic reproduction (`same seed == same output`);
- range validity under 8-thread / 100-task stress;
- container thread-safety and clone/detach semantics;
- logging capture, null-logger, concurrency.

```bash
./build/tests                       # all tests
./build/tests "DiceRoller*"         # tag-filtered
./build/tests "AsyncDiceRoller*" -r compact
```

## Contributing

1. Fork & create a branch.
2. Keep formatting clean: `clang-format -i` your files.
3. Add tests for anything new; they must stay deterministic.
4. Open a PR; CI (three compilers + sanitizers) must be green.

## License

[MIT](LICENSE) © 2026 Sasha Jaroshevskii.