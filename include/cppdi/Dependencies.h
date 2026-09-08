#pragma once

#include "cppdi/DependencyTraits.h"
#include "cppdi/Logger.h"
#include "cppdi/Random.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>

namespace cppdi {

/// A tiny, type-erased dependency container inspired by Swift's
/// `DependencyValues`.
///
/// Registrations are keyed by the *interface* type and stored as erased
/// `shared_ptr`s. Lookups are O(1) and protected by a mutex, so the same
/// container can be shared across threads.
///
/// Values are resolved in this order:
///   1. an implementation registered with `provide<T>(...)`;
///   2. a lazy default from `DependencyTraits<T>` — `live()` or `test()`
///      depending on this container's `DependencyContext` — created once on
///      first access and cached (mirrors `DependencyValues` caching);
///   3. otherwise `get<T>()` throws `DependencyNotFoundError`.
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
  explicit Dependencies(DependencyContext context = DependencyContext::Live)
      : registry{std::make_shared<Registry>(context)} {}

  // Copying the container is cheap and *shares* the underlying services and
  // memoized defaults: a copy behaves like a "clone" for this pattern.
  Dependencies(const Dependencies &) = default;
  Dependencies &operator=(const Dependencies &) = default;
  Dependencies(Dependencies &&) = default;
  Dependencies &operator=(Dependencies &&) = default;

  /// Register (or replace) the implementation for `Interface`.
  ///
  /// @tparam Interface the interface type used as the lookup key
  /// @tparam Concrete  the concrete implementation to instantiate
  /// @tparam Args      forwarded to the `Concrete` constructor
  template <typename Interface, typename Concrete, typename... Args> void provide(Args &&...args) {
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

  /// Return the value for `Interface`: a provided implementation if one
  /// exists, otherwise a lazily-created, cached `DependencyTraits<Interface>`
  /// default.
  ///
  /// @throws DependencyNotFoundError if neither a provider nor a trait
  ///         default exists.
  template <typename Interface> std::shared_ptr<Interface> get() const {
    if (auto impl = tryGet<Interface>()) {
      return impl;
    }
    return resolveDefault<Interface>();
  }

  /// Non-throwing variant of `get()`.
  ///
  /// @return a valid `shared_ptr`, or a null `shared_ptr` if no *explicit*
  ///         implementation is registered. Trait defaults are deliberately
  ///         ignored, so `contains()` keeps meaning "was this provided?".
  template <typename Interface> std::shared_ptr<Interface> tryGet() const {
    const std::type_index key{typeid(Interface)};
    std::lock_guard<std::mutex> lock(registry->mutex);
    auto it = registry->services.find(key);
    if (it == registry->services.end()) {
      return {};
    }
    return std::static_pointer_cast<Interface>(it->second);
  }

  /// Whether an implementation was explicitly registered for `Interface`.
  template <typename Interface> bool contains() const {
    return static_cast<bool>(tryGet<Interface>());
  }

  /// Forget an explicitly provided value so later `get<T>()` falls back to
  /// the `DependencyTraits<T>` default again.
  template <typename Interface> void remove() {
    const std::type_index key{typeid(Interface)};
    std::lock_guard<std::mutex> lock(registry->mutex);
    registry->services.erase(key);
  }

  /// Which `DependencyContext` decides which trait default (`live()` vs
  /// `test()`) is used for values that are not explicitly provided.
  DependencyContext context() const {
    std::lock_guard<std::mutex> lock(registry->mutex);
    return registry->context;
  }

  /// Switch the default-value flavor (`DependencyContext::Live` / `Test`).
  ///
  /// Only affects defaults that have not been created *and cached* yet; an
  /// already-resolved value keeps its instance.
  void setContext(DependencyContext value) {
    std::lock_guard<std::mutex> lock(registry->mutex);
    registry->context = value;
  }

  /// Create a copy sharing the same services and cached defaults.
  ///
  /// Equivalent to copy construction; provided as an explicit
  /// intention-revealing alias for use in async code.
  Dependencies clone() const { return *this; }

  /// Create a copy with a *fresh* registry that shares the same service
  /// instances and previously-resolved defaults.
  ///
  /// Services registered afterwards on the copy do not affect the original
  /// — the scoping primitive behind `AppContext::forkForAsync`,
  /// `withDependencies`, and `prepareDependencies`.
  Dependencies detach() const {
    Dependencies copy;
    std::lock_guard<std::mutex> lock(registry->mutex);
    copy.registry->services = registry->services;
    copy.registry->defaults = registry->defaults;
    copy.registry->context = registry->context;
    return copy;
  }

private:
  template <typename Interface> std::shared_ptr<Interface> resolveDefault() const {
    const std::type_index key{typeid(Interface)};
    std::lock_guard<std::mutex> lock(registry->mutex);
    auto it = registry->defaults.find(key);
    if (it != registry->defaults.end()) {
      return std::static_pointer_cast<Interface>(it->second);
    }
    auto value = buildTraitDefault<Interface>();
    registry->defaults.emplace(key, std::static_pointer_cast<void>(value));
    return value;
  }

  template <typename Interface> std::shared_ptr<Interface> buildTraitDefault() const {
    if (registry->context == DependencyContext::Test) {
      return DependencyTraits<Interface>::test();
    }
    return DependencyTraits<Interface>::live();
  }

  struct Registry {
    std::unordered_map<std::type_index, std::shared_ptr<void>> services;
    std::unordered_map<std::type_index, std::shared_ptr<void>> defaults;
    DependencyContext context;
    mutable std::mutex mutex;

    explicit Registry(DependencyContext value) : context{value} {}
  };

  std::shared_ptr<Registry> registry{std::make_shared<Registry>(DependencyContext::Live)};
};

/// A ready-to-use application context, the C++ counterpart of
/// `DependencyValues` in the Composable Architecture.
///
/// It bundles every dependency the application cares about and lets you build
/// either a production graph or a fully deterministic test graph with one
/// line. For the ergonomic ("read dependencies anywhere") style, install one
/// on the current thread with `bindDependencies` and construct components with
/// their default constructor.
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