#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <typeinfo>

namespace cppdi {

namespace detail {
/// Best-effort readable type name. `typeid(...).name()` is implementation
/// defined but good enough for diagnostics.
inline std::string typeName(const std::type_info &info) {
  return info.name();
}
} // namespace detail

/// Which "flavor" of default values a dependency container should fall back
/// to. Mirrors Swift's `DependencyContext` (without previews, which have no
/// C++ analogue).
enum class DependencyContext {
  Live, ///< production: `live()` defaults are used
  Test, ///< tests: `test()` defaults are used
};

/// Thrown by `Dependencies::get<T>()` when neither an explicitly provided
/// value nor a `DependencyTraits<T>` default is available.
class DependencyNotFoundError : public std::out_of_range {
public:
  explicit DependencyNotFoundError(const std::string &typeName)
      : std::out_of_range("No implementation registered for: " + typeName) {}
};

/// The C++ counterpart of Swift's `DependencyKey` protocol.
///
/// Specialize this template once per interface to register *default values*:
///
/// ```cpp
/// template <> struct cppdi::DependencyTraits<IRandomGenerator> {
///   static std::shared_ptr<IRandomGenerator> live() { return /* real */; }
///   static std::shared_ptr<IRandomGenerator> test() {
///     return /* deterministic, or throw to forbid accidental live use */;
///   }
/// };
/// ```
///
/// - `live()` is used by `Dependencies{DependencyContext::Live}` (production).
/// - `test()` is used by `Dependencies{DependencyContext::Test}`; unless you
///   override it, it falls back to `live()` — exactly like Swift's default
///   `testValue == liveValue`.
///
/// A dependency with no specialization has **no default**: accessing `get<T>()`
/// without an explicit `provide` throws `DependencyNotFoundError`, so tests
/// cannot silently fall into live behavior.
///
/// Defaults are created lazily on first access and cached per container
/// (mirroring `DependencyValues` caching).
template <typename Interface> struct DependencyTraits {
  static std::shared_ptr<Interface> live() {
    throw DependencyNotFoundError{detail::typeName(typeid(Interface)) +
                                  " (no DependencyTraits<...>::live() default registered)"};
  }

  static std::shared_ptr<Interface> test() { return live(); }
};

} // namespace cppdi