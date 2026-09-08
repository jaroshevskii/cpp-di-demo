#pragma once

#include <cstdint>
#include <mutex>
#include <random>
#include <stdexcept>

namespace cppdi {

/// Random number generator abstraction.
///
/// Inject through `Dependencies` so tests can swap in a seeded, reproducible
/// generator while production uses real randomness.
class IRandomGenerator {
public:
    virtual ~IRandomGenerator() = default;

    /// Uniform integer in the inclusive range `[min, max]`.
    /// @throws std::invalid_argument if `min > max`.
    [[nodiscard]] virtual int nextInt(int min, int max) = 0;

    /// Uniform double in the half-open range `[0.0, 1.0)`.
    [[nodiscard]] virtual double nextDouble() = 0;

    /// Reseed the generator from `seed`.
    virtual void seed(std::uint64_t seed) = 0;
};

/// Production implementation.
///
/// Each thread owns an independent engine (`thread_local`), so concurrent
/// calls are lock-free and never contend. `seed()` only affects the calling
/// thread — acceptable outside tests, where determinism is required per
/// thread anyway.
class ThreadLocalRandomGenerator final : public IRandomGenerator {
public:
    [[nodiscard]] int nextInt(int min, int max) override {
        if (min > max) {
            throw std::invalid_argument("ThreadLocalRandomGenerator: min > max");
        }
        return std::uniform_int_distribution<int>{min, max}(engine());
    }

    [[nodiscard]] double nextDouble() override {
        return std::uniform_real_distribution<double>{0.0, 1.0}(engine());
    }

    void seed(std::uint64_t seed) override {
        // Only affects the calling thread; other threads keep independent
        // state. std::mt19937 seeds are 32-bit; the upper bits are dropped.
        engine().seed(static_cast<std::mt19937::result_type>(seed));
    }

private:
    static std::mt19937 &engine() {
        static thread_local std::mt19937 engine{std::random_device{}()};
        return engine;
    }
};

/// Test / reproducible implementation.
///
/// A single seed reproduces an identical sequence on every run. Shared state
/// is guarded by a mutex, making this implementation safe to use
/// concurrently (e.g. from several async worker threads) at the cost of
/// moderate locking.
class DeterministicGenerator final : public IRandomGenerator {
public:
    explicit DeterministicGenerator(std::uint64_t seed = 42u)
        : engine{static_cast<std::mt19937::result_type>(seed)} {}

    [[nodiscard]] int nextInt(int min, int max) override {
        if (min > max) {
            throw std::invalid_argument("DeterministicGenerator: min > max");
        }
        std::lock_guard<std::mutex> lock(mutex);
        return std::uniform_int_distribution<int>{min, max}(engine);
    }

    [[nodiscard]] double nextDouble() override {
        std::lock_guard<std::mutex> lock(mutex);
        return std::uniform_real_distribution<double>{0.0, 1.0}(engine);
    }

    void seed(std::uint64_t seed) override {
        std::lock_guard<std::mutex> lock(mutex);
        // std::mt19937 seeds are 32-bit; the upper bits are intentionally
        // dropped.
        engine.seed(static_cast<std::mt19937::result_type>(seed));
    }

private:
    std::mt19937 engine;
    mutable std::mutex mutex;
};

} // namespace cppdi