#pragma once

#include <cassert>
#include <type_traits>
#include <utility>
#include <variant>

namespace eventbook {

/// Marks a value as the *error* arm when constructing a Result.
///
/// Without a tag, `Result<T, E>` would be ambiguous whenever T and E are the
/// same type or convert to one another. Writing `return Failure{Error::Empty};`
/// also states the intent at the return site, which is where a reader looks.
template <typename E>
struct Failure {
    E error;
};

template <typename E>
Failure(E) -> Failure<E>;

/// A value or an explanation of why there isn't one.
///
/// EventBook targets C++20, where `std::expected` (C++23) is unavailable, so
/// this is the project's single explicit result type for *recoverable* failures:
/// unparseable prices, ineligible markets, HTTP errors. It is deliberately a
/// drop-in shape for `std::expected` so the eventual swap is mechanical.
///
/// Exceptions are reserved for irrecoverable setup failures handled at an
/// application boundary. A malformed market message is not exceptional -- it is
/// Tuesday -- and must be counted, not thrown.
template <typename T, typename E>
class Result {
    static_assert(!std::is_void_v<T>, "Result<void, E> is not supported yet");
    static_assert(!std::is_reference_v<T>, "Result stores values, not references");

public:
    using value_type = T;
    using error_type = E;

    /// Implicit on purpose: `return Price{1200};` beats naming Result twice.
    constexpr Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}

    constexpr Result(Failure<E> failure)
        : storage_(std::in_place_index<1>, std::move(failure.error)) {}

    [[nodiscard]] constexpr bool has_value() const noexcept {
        return storage_.index() == 0;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return has_value();
    }

    /// Precondition: has_value(). Dereferencing an error is a programmer bug,
    /// not a market condition, so it is asserted rather than encoded in a type.
    ///
    /// std::get, not std::get_if, and not by preference. libstdc++'s get_if
    /// opens with `if (__ptr && ...)`, and comparing the address of a temporary
    /// against null is not a constant expression to GCC -- which makes every
    /// constexpr use of a Result prvalue fail to compile there, while libc++
    /// accepts it. std::get performs no such comparison.
    ///
    /// It also behaves better once NDEBUG removes the assert: get_if would
    /// return null and this would dereference it, whereas std::get throws
    /// std::bad_variant_access. That is an irrecoverable programmer error
    /// surfacing at an application boundary, not an exception standing in for
    /// a market condition.
    [[nodiscard]] constexpr const T& operator*() const& {
        assert(has_value());
        return std::get<0>(storage_);
    }

    [[nodiscard]] constexpr const T* operator->() const {
        return &**this;
    }

    [[nodiscard]] constexpr T value_or(T fallback) const {
        return has_value() ? **this : std::move(fallback);
    }

    /// Precondition: !has_value(). See operator*() for why this is std::get.
    [[nodiscard]] constexpr const E& error() const& {
        assert(!has_value());
        return std::get<1>(storage_);
    }

private:
    std::variant<T, E> storage_;
};

}  // namespace eventbook
