# Modern C++23 with Qt

Omabook is C++23 (`CMAKE_CXX_STANDARD 23`, no extensions) on Qt 6, built with
GCC and checked with clang-tidy. The rules below are the defaults for new code;
each is the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
rule it comes from, adapted where Qt has its own idiom. Enforced by `.clang-tidy`
and `rules/cpp/*.yml` on the lines a change adds.

## Ownership and lifetime

- **Every resource has an owner whose destructor releases it** (R.1). For a
  QObject the owner is its Qt parent; for anything else a `std::unique_ptr`, a
  container, or a value member. A C library handle gets a `std::unique_ptr`
  with its free function as the deleter (see `bookarchive.cpp`).
- **No `delete`, `malloc` or `free` in application code** (R.11, R.10).
  `deleteLater()` for a QObject that may still be in use by the event loop.
- **Raw pointers and references never own** (R.3). `T&` when there must be one,
  `T*` only for "maybe none", `std::span` and `QStringView`/`std::string_view`
  for a view of someone else's data.
- `std::shared_ptr` only when ownership is really shared (R.21), never as a
  parameter of a function that only uses the object: take `T&` (F.7, R.36).
- Rule of zero: a class that owns nothing directly declares no destructor, copy
  or move. A class that does declares all five, with `noexcept` moves (C.20, C.21, C.66).

## Errors

Omabook does not use exceptions (Qt doesn't either). Instead:

| Situation | Write |
|---|---|
| Can fail, and the caller needs to know why | `std::expected<T, QString>` (and `std::expected<void, QString>`) |
| May have no answer, no reason needed | `std::optional<T>` |
| A caller's bug | `Q_ASSERT`, or a clear early return in release |

**No new `QString *error` out-parameters.** Many existing functions have one;
they are migrated to `std::expected` when they are touched for a reason, not in
a sweep. (F.20, F.21: return what you make.) Mark functions whose result must not
be ignored `[[nodiscard]]`.

## Values, types and expressions

- **`const auto` by default** for locals; drop `const` only when the variable
  changes. Spell the type where `auto` would hide a conversion that matters.
- **Initialise everything, with `{}`** where it rejects narrowing (ES.20, ES.23).
- **Named casts only**: `static_cast`, `qobject_cast` for QObjects, `std::bit_cast`.
  No C-style casts (ES.48, ES.49), including `(void)`: use the result, call a
  form that returns nothing, or write `std::ignore = f();` with a comment saying
  why a value must be dropped; leave an unused parameter unnamed or mark it
  `[[maybe_unused]]`. `const_cast` is a design error; `reinterpret_cast` needs a
  comment.
- `nullptr`, never `NULL` or `0` (ES.47). `enum class` (Enum.3). `using`, not
  `typedef` (T.43). `constexpr` for anything known at compile time (Con.5).
- Strong types over comments: `std::chrono` durations, not `int timeoutMs`.
- **Lambdas, not `std::bind`**. Range-for and `<algorithm>`/`<ranges>` over
  index loops (ES.71, P.3); `std::as_const` when iterating a Qt container.
- `std::print`/`std::println` (C++23) for new command-line output instead of
  `printf`/`fprintf`.

## Qt

- **Connect with function pointers or lambdas**, never `SIGNAL()`/`SLOT()`
  (except D-Bus, which only takes `SLOT()`). Give a lambda a context object so
  it is disconnected when that object goes.
- **Long work leaves the window's thread**: `QtConcurrent::run`, with the
  result sent back through a signal or `QMetaObject::invokeMethod`. A worker
  opens its own `Library` (SQLite connections are per thread). Never block the
  event loop with a nested `QEventLoop` in the window.
- **Never `std::thread::detach()`**; prefer `std::jthread` or QtConcurrent (CP.26).
- `Q_PROPERTY` with a `NOTIFY` signal for anything QML shows; QML never reaches
  into C++ internals.

## Naming (checked by readability-identifier-naming)

Types `CamelCase`, functions and variables `camelBack`, private and protected
members `m_camelBack`, namespaces `CamelCase` (`BookOps`), macros `UPPER_CASE`.

## Comments

Say *why*, in full sentences, in the voice of the existing code. The code says
what.
