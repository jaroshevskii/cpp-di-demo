# Declaring dependencies, registering default values

In `swift-dependencies` the steps are: conform to `DependencyKey`
(`liveValue`, `testValue`), read with `@Dependency(\.key)`, override scopes
with `withDependencies { $0.x = … }`. This page is the C++ translation of
that same workflow, end to end.

For the philosophy and the value-stack rules, see
[architecture.md §7](architecture.md).

---

## 1. Declare the interface

```cpp
// api_client.hpp
class IApiClient {
public:
  virtual ~IApiClient() = default;

  /// The greeting the client returns for a user id. Throws on failure.
  virtual std::string greet(int userId) = 0;
};
```

## 2. Register default values (the `DependencyKey` conformance)

Specialize `DependencyTraits<T>` exactly the way you'd implement
`liveValue` and `testValue`:

```cpp
// api_client_live.cpp
template <> struct cppdi::DependencyTraits<IApiClient> {
  static std::shared_ptr<IApiClient> live() {
    return std::make_shared<LiveAPIClient>();      // real network + auth
  }
  static std::shared_ptr<IApiClient> test() {
    return std::make_shared<MockAPIClient>();      // canned, deterministic
  }
};
```

Rules, mirroring Swift:

| Swift                                             | cppdi                                            |
| ------------------------------------------------- | ------------------------------------------------ |
| `static var liveValue`                            | `static … live()`                                |
| `static var testValue`                            | `static … test()`                                |
| unprovided `testValue` defaults to `liveValue`    | primary template's `test()` calls `live()`       |
| `liveValue` used in a test context fails the test | our `test()` can throw `DependencyNotFoundError` |
| first access caches the value                     | `Dependencies` caches per container              |

If a dependency needs a runtime secret, don't encode it in `live()`; inject it
once at startup with `prepareDependencies` (step 5).

## 3. Read it anywhere (`@Dependency`)

```cpp
cppdi::Dependency<IApiClient> apiClient;   // like @Dependency(\.apiClient)
std::string greeting = apiClient->greet(42);
```

- Default-constructed: resolves through the current scope (see steps 4–5).
- `Dependency<T>{deps}`: pin it to a specific container instead.

## 4. Override for a scope (`withDependencies`)

```cpp
cppdi::withDependencies(
    [](cppdi::Dependencies &dependencies) {
      dependencies.provide<IApiClient, MockAPIClient>();
    },
    [] {
      // Everything here — however deep — resolves the mock.
    });
```

Restores the previous values even if the body throws. This is also how you set
up a whole test in the style of Swift's `.dependencies` test trait.

## 5. Override for the process (`prepareDependencies`)

For `liveValue`s that need configuration only available at launch:

```cpp
int main(int argc, char **argv) {
  cppdi::prepareDependencies([](cppdi::Dependencies &dependencies) {
    dependencies.provide<IApiClient>(makeClient(token));
  });
  run(argc, argv);
}
```

Every thread and every scope now defaults to that graph. Snapshot and restore
with `defaultDependencies()` / `setDefaultDependencies(...)` if you replace
the defaults from tests.

## 6. Bridging to the explicit style

The `DependencyTraits`/`Dependency<T>` layer and the `AppContext` layer both
degrade gracefully into each other:

```cpp
auto ctx = cppdi::AppContext::test(42u);          // seeded, deterministic
auto scope = cppdi::bindDependencies(ctx.dependencies);

app::DiceRoller a{ctx};        // explicit  — reads from `ctx`
app::DiceRoller b;             // ergonomic — reads from `ctx` via the scope
```

Both get the *same instances* from `ctx`'s graph. Components that take a
context keep working standalone; components built implicitly pick up whatever
the current values resolve to.

## 7. Deleting an override

`Dependencies::remove<T>()` forgets an explicit value so `get<T>()` falls back
to the trait default again — handy when a scoped override should "reset".

## Cheat sheet

| Goal                            | Use                                                |
| ------------------------------- | -------------------------------------------------- |
| Live default for an interface   | `DependencyTraits<T>::live()`                      |
| Deterministic test default      | `DependencyTraits<T>::test()`                      |
| Access a dependency             | `Dependency<T>` (current scope or pinned)          |
| Override for a lexical scope    | `withDependencies(mutate, operation)`               |
| Override for the whole process  | `prepareDependencies(mutate)`                       |
| Install a ready context         | `bindDependencies(values)`                          |
| Forget an explicit value        | `Dependencies::remove<T>()`                         |
| Current scope values            | `currentDependencyValues()`                         |