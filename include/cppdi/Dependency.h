#pragma once

#include "cppdi/Dependencies.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace cppdi {

namespace detail {

inline std::mutex &globalValuesMutex() {
  static std::mutex mutex;
  return mutex;
}

/// The process-wide default dependency values. Customizable once at startup
/// with `prepareDependencies` / `setDefaultDependencies`.
inline std::shared_ptr<Dependencies> &globalValues() {
  static std::shared_ptr<Dependencies> values = std::make_shared<Dependencies>();
  return values;
}

/// Per-thread overlay installed by `withDependencies` / `bindDependencies`.
///
/// Thread-local, so scoped overrides never leak into other threads — this is
/// the C++ analogue of Swift's per-task `DependencyValues`.
inline std::shared_ptr<Dependencies> &threadOverlay() {
  static thread_local std::shared_ptr<Dependencies> values;
  return values;
}

/// The values the calling thread should resolve from: its overlay if one is
/// installed, otherwise the process-wide defaults.
inline std::shared_ptr<Dependencies> resolveCurrentValues() {
  auto &overlay = threadOverlay();
  if (overlay) {
    return overlay;
  }
  std::lock_guard<std::mutex> lock(globalValuesMutex());
  return globalValues();
}

} // namespace detail

/// RAII scope that installs dependency values for the calling thread and
/// restores the previous ones when destroyed.
///
/// Returned by `bindDependencies` (and used internally by
/// `withDependencies`). Keep the scope alive for as long as the values must
/// apply.
class DependencyValuesScope {
public:
  explicit DependencyValuesScope(std::shared_ptr<Dependencies> values)
      : previous_{detail::threadOverlay()}, hadPrevious_{static_cast<bool>(previous_)} {
    detail::threadOverlay() = std::move(values);
  }

  ~DependencyValuesScope() {
    if (hadPrevious_) {
      detail::threadOverlay() = std::move(previous_);
    } else {
      detail::threadOverlay().reset();
    }
  }

  DependencyValuesScope(const DependencyValuesScope &) = delete;
  DependencyValuesScope &operator=(const DependencyValuesScope &) = delete;

private:
  std::shared_ptr<Dependencies> previous_;
  bool hadPrevious_;
};

/// Install `values` as the current dependency values for the calling thread.
///
/// The counterpart of Swift's app-level wiring (`prepareDependencies`) when
/// you already have a ready-made context:
///
/// ```cpp
/// auto ctx = AppContext::test(42u);
/// auto scope = bindDependencies(ctx.dependencies);
/// // components built with their default constructor resolve through `ctx`
/// ```
[[nodiscard]] inline DependencyValuesScope bindDependencies(Dependencies values) {
  return DependencyValuesScope{std::make_shared<Dependencies>(std::move(values))};
}

/// Run `operation` with dependency values produced by applying `mutate` to an
/// isolated copy of the current values.
///
/// The counterpart of Swift's `withDependencies(_:operation:)`:
///
/// ```cpp
/// cppdi::withDependencies(
///     [](cppdi::Dependencies &dependencies) {
///       dependencies.provide<ILogger, NullLogger>();
///     },
///     [] {
///       // code here sees the NullLogger; the caller's values are untouched
///     });
/// ```
///
/// The overlay is thread-local and restored — even if `operation` throws.
/// Returns whatever `operation` returns.
template <typename Mutator, typename Operation>
decltype(auto) withDependencies(Mutator &&mutate, Operation &&operation) {
  auto base = detail::resolveCurrentValues();
  auto overlay = std::make_shared<Dependencies>(base->detach());
  std::invoke(std::forward<Mutator>(mutate), *overlay);
  DependencyValuesScope scope{overlay};
  return std::invoke(std::forward<Operation>(operation));
}

/// The dependency values currently in effect on the calling thread.
inline Dependencies currentDependencyValues() {
  return *detail::resolveCurrentValues();
}

/// The process-wide default dependency values, as an isolated copy (mutating
/// it does not affect the running defaults).
inline Dependencies defaultDependencies() {
  std::lock_guard<std::mutex> lock(detail::globalValuesMutex());
  return detail::globalValues()->detach();
}

/// Replace the process-wide default dependency values (thread-safe).
///
/// New scopes and unbound threads resolve through these defaults.
inline void setDefaultDependencies(Dependencies values) {
  std::lock_guard<std::mutex> lock(detail::globalValuesMutex());
  detail::globalValues() = std::make_shared<Dependencies>(std::move(values));
}

/// Configure the process-wide default dependency values at startup, and return
/// the resulting set — the counterpart of Swift's `prepareDependencies`.
///
/// Perfect for values that need a runtime secret or configuration you only
/// have at launch:
///
/// ```cpp
/// int main() {
///   prepareDependencies([](cppdi::Dependencies &dependencies) {
///     dependencies.provide<IClient>(makeClient(config.token));
///   });
///   // ...every Dependency<IClient> across every thread now resolves to it
/// }
/// ```
template <typename Mutator> Dependencies prepareDependencies(Mutator &&mutate) {
  auto base = detail::resolveCurrentValues();
  auto next = std::make_shared<Dependencies>(base->detach());
  std::invoke(std::forward<Mutator>(mutate), *next);
  {
    std::lock_guard<std::mutex> lock(detail::globalValuesMutex());
    detail::globalValues() = std::move(next);
  }
  return currentDependencyValues();
}

/// Lazy accessor that resolves an interface through the *current* dependency
/// values — the C++ counterpart of Swift's `@Dependency` property wrapper.
///
/// ```cpp
/// cppdi::Dependency<IRandomGenerator> rng;
/// int value = rng->nextInt(1, 6); // resolves through the current scope
/// ```
///
/// Default-constructed, it reads the current scope (see `withDependencies`,
/// `bindDependencies`, `prepareDependencies`). It can also target a specific
/// container for explicit-style code.
template <typename Interface> class Dependency {
public:
  Dependency() = default;

  /// Always resolve through `values`, regardless of the current thread scope.
  explicit Dependency(Dependencies values) : values_{std::move(values)} {}

  /// Resolve the interface now.
  std::shared_ptr<Interface> get() const {
    if (values_) {
      return values_->get<Interface>();
    }
    return detail::resolveCurrentValues()->get<Interface>();
  }

  [[nodiscard]] Interface &operator*() const { return *get(); }
  [[nodiscard]] Interface *operator->() const { return get().get(); }

private:
  std::optional<Dependencies> values_;
};

} // namespace cppdi