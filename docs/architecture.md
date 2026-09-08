# Architecture

This document explains how `cppdi` works, why it is designed the way it is,
and how to extend it. It mirrors the mental model of
[The Composable Architecture](https://github.com/pointfreeco/swift-composable-architecture)'s
dependency system, translated into idiomatic C++20.

---

## 1. The core idea

Everything in your program that is "external" — randomness, clocks, sockets,
files, logging destination — is represented by a small **interface** with a
*virtual* interface, registered in a central **`Dependencies`** container, and
injected into the code that needs it.

```
                    ┌──────────────────────┐
 InjectGraph ─────▶ │ AppContext           │
 (live or test)     │  ┌────────────────┐  │
                    │  │ Dependencies   │  │   provide/get    ┌────────────┐
                    │  │  Interface ──▶ │  │ ───────────────▶ │ DiceRoller │
                    │  │  shutter box   │  │                  │  rng (I/F) │
                    │  └────────────────┘  │                  └────────────┘
                    │  makeRng: factory    │
                    └──────────────────────┘
```

The three moving parts are:

| Piece            | API                                          | Responsibility              |
| ---------------- | -------------------------------------------- | --------------------------- |
| Interface        | `IRandomGenerator`, `ILogger`                | the *contract* you program against |
| Container        | `Dependencies::provide / get / tryGet / …`   | owns the implementations, keys by interface |
| Context          | `AppContext::live / test`, `makeRng`         | *wiring* — production vs. test |

## 2. `Dependencies` — the type-erased container

### Registration keys are interfaces

```cpp
template <typename Interface, typename Concrete, typename... Args>
void provide(Args &&...args) {
    provide<Interface>(std::make_shared<Concrete>(std::forward<Args>(args)...));
}
```

The interface type — `typeid(Interface)` — is the map key. The value is an
erased `std::shared_ptr<void>`. `get<T>()` casts it back:

```cpp
template <typename Interface>
std::shared_ptr<Interface> get() const {
    auto impl = tryGet<Interface>();
    if (!impl) throw DependencyNotFoundError{detail::typeName(typeid(Interface))};
    return impl;
}
```

Because registration is keyed by **interface**, you can re-provide the same
interface with a different concrete type later — that is exactly how a test
"overrides one instance" without touching anything else.

### Why `shared_ptr`?

- **Cheap copies.** Copying `Dependencies` copies the container handle, not
  the services (see §4).
- **Lifetime safety.** The container and every service outlive any object that
  grabbed a `shared_ptr` reference to them — even if the container is
  destroyed.
- **Type erasure.** The erased `shared_ptr<void>` stores the holding-operation
  (the `shared_ptr`'s control block), so the correct deleter is invoked even
  though the type is erased.

### The container is thread-safe

A `std::mutex` guards the map on every `provide` / `tryGet` / `detach`.
Lookups are O(1) and can bounce off any number of threads. The **state of the
services** is a separate concern owned by each implementation (§4).

### API summary

| Method                                   | Semantics                                                     |
| ---------------------------------------- | ------------------------------------------------------------- |
| `provide<Interface, Concrete>(args…)`    | construct + register (replaces existing)                      |
| `provide<Interface>(shared_ptr<Impl>)`   | register a pre-built instance                                 |
| `get<Interface>()`                       | throw on missing, else `shared_ptr<Interface>`                |
| `tryGet<Interface>()`                    | null `shared_ptr` on missing                                  |
| `contains<Interface>()`                  | `bool`                                                        |
| `clone()`                                | copy sharing the same services                                |
| `detach()`                               | copy with a fresh registry (same service instances)           |

## 3. `AppContext` — production vs. test wiring

`AppContext` bundles a `Dependencies` plus an **RNG factory**:

```cpp
struct AppContext {
    Dependencies dependencies;
    std::function<std::shared_ptr<IRandomGenerator>(std::uint64_t slot)> makeRng;

    static AppContext live();   // thread-local randomness, console logging
    static AppContext test(std::uint64_t baseSeed = 42u);
    AppContext forkForAsync(std::uint64_t slot) const;
};
```

- `live()` binds the real world: `ThreadLocalRandomGenerator`, `ConsoleLogger`.
- `test(baseSeed)` binds the controllable world: `DeterministicGenerator`,
  `TestLogger`.
- `makeRng(slot)` lets async code mint **per-slot** private engines
  (see `forkForAsync`).

The `std::function` factory is what turns "parallel work" from a test
nightmare into a reproducible sequence: the factory is the *one place* a
context knows how to build a generator, so both production and tests fork the
same way.

## 4. Thread-safety model, in depth

There are three separate layers; confusing them is the #1 cause of bugs:

```
Layer 1: container bookkeeping   (which interface → which impl)
Layer 2: per-service state       (RNG engine, log buffer, sockets…)
Layer 3: task confinement        (what each thread "owns")
```

### Layer 1 — container

Synchronized by `Dependencies`' internal mutex on every lookup/insertion.
`detach()` copies the services map under that same lock.

### Layer 2 — service state

The container does **not** protect service state. Each service chooses its
strategy:

| Service                        | Strategy                                  | Cost           |
| ------------------------------ | ----------------------------------------- | -------------- |
| `ThreadLocalRandomGenerator`   | per-thread `thread_local` engine          | zero locking   |
| `DeterministicGenerator`       | `mutex` around the engine (shared)        | locking        |
| `TestLogger`                   | `mutex` around the entries vector         | locking        |
| `ConsoleLogger`                | `mutex` around the write                  | locking        |

Rule of thumb: **in production prefer per-thread state, in tests prefer
shared state guarded by a mutex** — determinism requires sharing; real code
prefers not to lock.

### Layer 3 — task confinement

The async pattern in `AsyncDiceRoller::rollAsync()`:

```cpp
std::future<int> rollAsync() {
    const std::uint64_t slot = nextSlot.fetch_add(1, std::memory_order_relaxed);
    DiceRoller roller{context.forkForAsync(slot)};   // private engine!
    return std::async(std::launch::async, [roller]() mutable {
        return roller.roll();
    });
}
```

Two race-freedom rules:

1. **Never capture `this`.** The worker captures `roller` (a `DiceRoller`)
   **by value**. The `AsyncDiceRoller` can be destroyed while the future is
   still running — no dangling.
2. **Each task owns its engine.** `forkForAsync(slot)` gives the task a
   private `DeterministicGenerator(seed + slot)`. Two consequences:
   - *safe*: no two threads touch the same engine → no locking, no torn state;
   - *deterministic*: task #k always draws its first value from generator
     `seed + k`, so the result vector is identical on every run regardless of
     OS scheduling. (This is what makes `AsyncDiceRoller is repeatable for the
     same seed` pass — see [testing.md](testing.md) for why a shared generator
     would fail it.)

## 5. Why `detach()` exists

`clone()` shares the registry. That's perfect for "give another worker the
whole graph". But if task code then did

```cpp
task.dependencies.provide<IRandomGenerator>(privateEngine);
```

with a **shared** registry, it would replace the generator for *every other*
context too. `detach()` creates a fresh registry that still points at the
same logger, same string tables, etc. — so installing a private engine is
scoped to the task.

`AppContext::forkForAsync` composes these:

```cpp
AppContext forkForAsync(std::uint64_t slot) const {
    AppContext task{*this};
    task.dependencies = dependencies.detach();        // fresh registry
    task.dependencies.provide<IRandomGenerator>(makeRng(slot)); // private engine
    return task;
}
```

## 6. Extending the toolkit

### Add a new service

1. Define an interface + a production impl + a test impl.
2. Add it to `AppContext::live()` / `test()`.
3. Consume it exactly like `DiceRoller` does: `ctx.dependencies.get<T>()`.

### Add an RNG-free dependency that has no per-slot variant

Keep `makeRng` slot-aware only for randomness. Other dependencies bind once in
`live()/test()` and are shared through `clone()`/`detach()` unchanged — no
fork needed.

### Support a fresh RNG type

Implement `IRandomGenerator`. Wire it in via `makeRng` (for its own impl) and
`AppContext::test`/`live`. Only two construction sites in the whole codebase.

### Make the container RTTI-free (advanced)

`typeid` keys require RTTI. For `-fno-rtti` builds, replace the key with a
`constexpr` tag or a `type_index`-like enum. The container's contract does
not change.

## 7. Authoritative answers to "where does X live?"

| Question                                   | Answer                                              |
| ------------------------------------------ | ---------------------------------------------------- |
| How is the random generator chosen?        | `AppContext::live()` / `test()` + `makeRng`          |
| Why is a test deterministic?               | `AppContext::test(42)` registers a seeded generator  |
| Why is parallel work deterministic?        | `forkForAsync(slot)` → engine seeded `seed + slot`   |
| Who owns the RNG mutex?                    | The generator impl (`DeterministicGenerator`)        |
| Why is `THIS` never captured in async?     | Task captures state by value (dangling-free)         |
| How do I override one dependency?          | `ctx.dependencies.provide<Iface, Impl>(args…)`       |