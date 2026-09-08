#pragma once

#include "cppdi/Logger.h"
#include "cppdi/Random.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>

namespace cppdi {

namespace detail {
/// Best-effort readable type name. `typeid(...).name()` is implementation
/// defined, but good enough for diagnostics.
inline std::string typeName(const std::type_info &info) {
    return info.name();
}
} // namespace detail

/// Thrown by `Dependencies::get<T>()` when no implementation was registered
/// for the requested interface.
class DependencyNotFoundError : public std::out_of_range {
public:
    explicit DependencyNotFoundError(const std::string &typeName)
        : std::out_of_range("No implementation registered for: " + typeName) {}
};

/// A tiny, type-erased dependency container inspired by the Composable
/// Architecture's `DependencyValues`.
///
/// Registrations are keyed by the *interface* type and stored as erased
/// `shared_ptr`s. Lookups are O(1) and protected by a mutex, so the same
/// container can be shared across threads.
///
/// ## Thread-safety model
///
/// These layers are independent:
///   - lookup / registration on the container is internally synchronized;
///   - the *state* of the stored implementation is owned by the implementation
///     itself (e.g. `DeterministicGenerator` guards its engine with a mutex,
///     while `ThreadLocalRandomGenerator` uses a per-thread engine).
class Dependencies {
public:
    Dependencies() = default;

    // Copying the container is cheap and *shares* the underlying services:
    // a copy behaves like a "clone" for this pattern.
    Dependencies(const Dependencies &) = default;
    Dependencies &operator=(const Dependencies &) = default;
    Dependencies(Dependencies &&) = default;
    Dependencies &operator=(Dependencies &&) = default;

    /// Register (or replace) the implementation for `Interface`.
    ///
    /// @tparam Interface the interface type used as the lookup key
    /// @tparam Concrete  the concrete implementation to instantiate
    /// @tparam Args      forwarded to the `Concrete` constructor
    template <typename Interface, typename Concrete, typename... Args>
    void provide(Args &&...args) {
        provide<Interface>(std::make_shared<Concrete>(std::forward<Args>(args)...));
    }

    /// Register (or replace) an already-built implementation for `Interface`.
    ///
    /// Accepts a ready-made `shared_ptr` for cases where the caller wants to
    /// construct the implementation itself (e.g. with a per-task seed).
    template <typename Interface> void provide(std::shared_ptr<Interface> impl) {
        const std::type_index key{typeid(Interface)};
        std::lock_guard<std::mutex> lock(registry->mutex);
        registry->services[key] = std::move(impl);
    }

    /// Return the implementation registered for `Interface`.
    ///
    /// @throws DependencyNotFoundError if no implementation is registered
    template <typename Interface> std::shared_ptr<Interface> get() const {
        auto impl = tryGet<Interface>();
        if (!impl) {
            throw DependencyNotFoundError{detail::typeName(typeid(Interface))};
        }
        return impl;
    }

    /// Non-throwing variant of `get()`.
    ///
    /// @return a valid `shared_ptr`, or a null `shared_ptr` if unregistered.
    template <typename Interface> std::shared_ptr<Interface> tryGet() const {
        const std::type_index key{typeid(Interface)};
        std::lock_guard<std::mutex> lock(registry->mutex);
        auto it = registry->services.find(key);
        if (it == registry->services.end()) {
            return {};
        }
        return std::static_pointer_cast<Interface>(it->second);
    }

    /// Whether an implementation is registered for `Interface`.
    template <typename Interface> bool contains() const {
        return static_cast<bool>(tryGet<Interface>());
    }

    /// Create a copy sharing the same services.
    ///
    /// Equivalent to copy construction; provided as an explicit
    /// intention-revealing alias for use in async code.
    Dependencies clone() const { return *this; }

    /// Create a copy with a *fresh* registry that still shares the same
    /// service instances.
    ///
    /// Services registered afterwards on the copy do not affect the original
    /// — used by `AppContext::forkForAsync` so parallel tasks can install
    /// per-task engines without mutating a shared graph.
    Dependencies detach() const {
        Dependencies copy;
        std::lock_guard<std::mutex> lock(registry->mutex);
        copy.registry->services = registry->services;
        return copy;
    }

private:
    struct Registry {
        std::unordered_map<std::type_index, std::shared_ptr<void>> services;
        mutable std::mutex mutex;
    };

    std::shared_ptr<Registry> registry{std::make_shared<Registry>()};
};

/// A ready-to-use application context, the C++ counterpart of
/// `DependencyValues` in the Composable Architecture.
///
/// It bundles every dependency the application cares about and lets you build
/// either a production graph or a fully deterministic test graph with one
/// line.
///
/// @code
/// auto liveContext = cppdi::AppContext::live();    // real randomness
/// auto testContext = cppdi::AppContext::test(42u); // seeded, deterministic
/// @endcode
struct AppContext {
    Dependencies dependencies;

    /// Builds an RNG for a given work "slot". Production ignores the slot;
    /// tests derive `(baseSeed + slot)` so parallel work stays reproducible.
    std::function<std::shared_ptr<IRandomGenerator>(std::uint64_t slot)> makeRng;

    /// Production wiring: real (thread-local) randomness, console logging.
    static AppContext live() {
        AppContext ctx;
        ctx.makeRng = [](std::uint64_t) { return std::make_shared<ThreadLocalRandomGenerator>(); };
        ctx.dependencies.provide<IRandomGenerator>(ctx.makeRng(0));
        ctx.dependencies.provide<ILogger, ConsoleLogger>();
        return ctx;
    }

    /// Test wiring: seeded/deterministic randomness, capturing logger.
    ///
    /// Parallel tasks get slot-derived seeds (`baseSeed + slot`) so any
    /// sequence of asynchronous work is fully reproducible.
    static AppContext test(std::uint64_t baseSeed = 42u) {
        AppContext ctx;
        ctx.makeRng = [baseSeed](std::uint64_t slot) {
            return std::make_shared<DeterministicGenerator>(baseSeed + slot);
        };
        ctx.dependencies.provide<IRandomGenerator>(ctx.makeRng(0));
        ctx.dependencies.provide<ILogger, TestLogger>();
        return ctx;
    }

    /// Copy sharing the same services (safe to pass across threads).
    AppContext clone() const { return *this; }

    /// Fork a task-local context with an engine derived from `slot`.
    ///
    /// The detached copy keeps the original graph untouched (so the base RNG
    /// is preserved) while giving the caller a private, slot-seeded engine —
    /// the secret to deterministic parallelism.
    AppContext forkForAsync(std::uint64_t slot) const {
        AppContext task{*this};
        task.dependencies = dependencies.detach();
        task.dependencies.provide<IRandomGenerator>(makeRng(slot));
        return task;
    }
};

} // namespace cppdi