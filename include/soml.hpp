/*
SOML for C++: a reader and writer for SOML, a config format for humans.

https://soml.sh

The license is at the end of this file.
*/

#ifndef SOML_HPP
#define SOML_HPP

#if __cplusplus < 202400L
#error "SOML needs C++26: compile with -std=c++26 (or -std=c++2c)"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <charconv>
#include <chrono>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if defined(__cpp_impl_reflection)
#include <meta>
#endif

// A function result that points into an argument, so the compiler can warn when the argument dies first. Clang has the attribute, GCC does not.
#if __has_cpp_attribute(clang::lifetimebound)
#define SOML_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define SOML_LIFETIMEBOUND
#endif

// Runtime floating-point conversion uses std::from_chars and std::to_chars; constant evaluation uses the exact decimal converter.
// Performance notes on rejected alternatives below refer to Apple Clang 21/libc++ and GCC 16/libstdc++ at -O2; rebenchmark both before revisiting them.

static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8, "SOML needs `double` to be IEEE 754 binary64");

#if !defined(__SIZEOF_INT128__)
#error "SOML needs a 64-bit target, whose compiler has __int128"
#endif

namespace soml {

namespace detail {

// Exact time arithmetic: 64-bit counts times 64-bit ratios fit in 128 bits.
__extension__ using int128 = __int128;

template<class T>
concept is_character = std::same_as<T, char> || std::same_as<T, char8_t> || std::same_as<T, char16_t> || std::same_as<T, char32_t> || std::same_as<T, wchar_t>;

template<class Representation>
concept chrono_representation = std::chrono::treat_as_floating_point_v<Representation> || (std::integral<Representation> && sizeof(Representation) <= sizeof(std::int64_t));

// The denominator of a chrono period is always positive.
constexpr int128 floor_divide(int128 dividend, int128 divisor) noexcept {
	const int128 quotient = dividend / divisor;
	return dividend % divisor < 0 ? quotient - 1 : quotient;
}

template<std::integral Integer>
	requires (sizeof(Integer) <= sizeof(std::int64_t))
constexpr bool fits_integer(int128 number) noexcept {
	return number >= static_cast<int128>(std::numeric_limits<Integer>::min()) && number <= static_cast<int128>(std::numeric_limits<Integer>::max());
}

} // namespace detail

/**
The spec's 100-level nesting limit. Every array and object counts, including the root with or without braces.
*/
inline constexpr std::size_t max_depth = 100;

// MARK: Errors

/**
What kind of error an operation failed with.
*/
enum class error_code : std::uint8_t {
	/**
	The text is not a valid SOML document.
	*/
	syntax = 1,

	/**
	The text or the value is nested more than `soml::max_depth` levels deep.
	*/
	too_deep,

	/**
	The writer got a value that SOML cannot hold: NaN, a string that is not valid UTF-8 or holds a carriage return, a date that does not exist in the proleptic Gregorian calendar in years 0001 to 9999, an instant outside the years 0001 to 9999, a duplicate key, or a root that is not an array or an object.
	*/
	invalid_value,

	/**
	Decoding into a C++ type found a value of another type, such as a string where the type has an int.
	*/
	type_mismatch,

	/**
	Decoding into a C++ type found a value that the type cannot hold, such as 70000 for a `std::uint16_t`, or encoding found one that SOML cannot hold, such as a `std::uint64_t` above the int64 range.
	*/
	out_of_range,

	/**
	Decoding into a struct found a key that the struct has no member for. Pass `{.ignore_unknown_keys = true}` to skip such keys.
	*/
	unknown_key,
};

/**
The name of an error code, such as `"syntax"`.
*/
[[nodiscard]] constexpr std::string_view to_string(error_code code) noexcept {
	static constexpr std::array<std::string_view, 6> names{"syntax", "too_deep", "invalid_value", "type_mismatch", "out_of_range", "unknown_key"};
	const auto index = std::to_underlying(code);
	return index >= 1 && index <= names.size() ? names[index - 1] : "unknown";
}

/**
Why an operation failed.

A reading or text-decoding error has a position: `offset` is the byte offset in the text, and `line` and `column` are 1-based, with the column counted in Unicode scalar values. Reading positions are the same as the reference implementation's for every invalid case in the conformance suite. Errors from `from_value` and writing have a `line` of 0.

A writing or decoding error has a `path` to the value it is about, such as `servers[1].port`.
*/
struct error {
	error_code code = error_code::syntax;

	/**
	What is wrong, such as `Duplicate key name`.
	*/
	std::string message;

	std::size_t offset = 0;
	std::size_t line = 0;
	std::size_t column = 0;

	/**
	Where in the value a writing or decoding error is, as in `servers[1].port`, or empty.
	*/
	std::string path;

	/**
	The message with its path and position when available, such as `Duplicate key name at line 3, column 1`. C1 and bidirectional controls and Unicode line and paragraph separators are shown as U+FFFD; the original `message` and `path` are unchanged.
	*/
	[[nodiscard]] constexpr std::string description() const;

	/**
	Up to three lines of `text` that end at the error, with a caret under its position, as in:

	```
	  1 | name: 'api'
	> 2 | port: 80 80
	    |          ^
	```

	Pass the original text. Invalid UTF-8, controls other than tabs and line feeds, bidirectional marks, and Unicode line and paragraph separators are shown as U+FFFD. Empty for an error without a position.
	*/
	[[nodiscard]] constexpr std::string code_frame(std::string_view text) const;

	friend constexpr bool operator==(const error&, const error&) = default;
};

// MARK: Values

class value;
struct member;
class object;
class view;

namespace detail {

struct object_access;

} // namespace detail

/**
The type of a value.
*/
enum class type : std::uint8_t {
	null,
	boolean,
	integer,
	floating,
	string,
	date,
	instant,
	duration,
	array,
	object,
};

/**
The spec's name of a type: `null`, `bool`, `int`, `float`, `string`, `date`, `instant`, `duration`, `array`, or `object`.
*/
[[nodiscard]] constexpr std::string_view to_string(type kind) noexcept {
	static constexpr std::array<std::string_view, 10> names{"null", "bool", "int", "float", "string", "date", "instant", "duration", "array", "object"};
	const auto index = std::to_underlying(kind);
	return index < names.size() ? names[index] : "unknown";
}

/**
A calendar date in the proleptic Gregorian calendar, with no time or timezone. Parsed dates have years 0001 through 9999. Native `year_month_day` values may be invalid; typed encoding and every writer reject them without normalization or conversion through an instant.
*/
using date = std::chrono::year_month_day;

/**
A length of time, in nanoseconds. This is exactly the spec's duration: a signed count of nanoseconds in the int64 range.
*/
using duration = std::chrono::nanoseconds;

/**
A point in time, as seconds and nanoseconds since 1970-01-01T00:00:00Z, in the proleptic Gregorian calendar without leap seconds.

Custom chrono periods whose nanosecond conversion ratios overflow `std::intmax_t` are unsupported. SOML instants span the years 0001 to 9999, which `std::chrono::sys_time<std::chrono::nanoseconds>` cannot hold, so an instant has its own type, with conversions from a `std::chrono::sys_time` with a built-in integer count of at most 64 bits, and to a `std::chrono::sys_time` with a floating-point count or a built-in integer count of at most 64 bits that can hold it.

An instant is valid when `nanoseconds` is 0 to 999,999,999 and it is from `instant::min()` to `instant::max()`. A writer refuses one that is not.
*/
struct instant {
	std::int64_t seconds = 0;
	std::int32_t nanoseconds = 0;

	constexpr instant() noexcept = default;

	constexpr instant(std::int64_t whole_seconds, std::int32_t fraction_nanoseconds) noexcept :
		seconds(whole_seconds),
		nanoseconds(fraction_nanoseconds) {}

	/**
	The instant of a `std::chrono::sys_time` with a built-in integer count of at most 64 bits, such as `std::chrono::system_clock::now()`, rounded down to a nanosecond. A time whose seconds do not fit 64 bits gives an instant that is not valid.
	*/
	template<class Duration>
		requires (!std::chrono::treat_as_floating_point_v<typename Duration::rep> && detail::chrono_representation<typename Duration::rep>)
	constexpr instant(std::chrono::sys_time<Duration> time) noexcept { // NOLINT(google-explicit-constructor)
		// The time in seconds is count × num / den, which 128 bits hold exactly for any 64-bit count and ratio.
		using period = typename Duration::period;
		const detail::int128 scaled = static_cast<detail::int128>(time.time_since_epoch().count()) * period::num;
		const detail::int128 whole = detail::floor_divide(scaled, period::den);

		if (!detail::fits_integer<std::int64_t>(whole)) {
			seconds = whole < 0 ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max();
			nanoseconds = 0;
			return;
		}

		seconds = static_cast<std::int64_t>(whole);
		nanoseconds = static_cast<std::int32_t>((scaled - (whole * period::den)) * 1'000'000'000 / period::den);
	}

	/**
	0001-01-01T00:00:00Z, the earliest instant.
	*/
	[[nodiscard]] static constexpr instant min() noexcept {
		return {-62'135'596'800, 0};
	}

	/**
	9999-12-31T23:59:59.999999999Z, the latest instant.
	*/
	[[nodiscard]] static constexpr instant max() noexcept {
		return {253'402'300'799, 999'999'999};
	}

	/**
	Whether the nanoseconds are 0 to 999,999,999 and the instant is from `min()` to `max()`.
	*/
	[[nodiscard]] constexpr bool is_valid() const noexcept {
		return nanoseconds >= 0 && nanoseconds <= 999'999'999 && seconds >= min().seconds && seconds <= max().seconds;
	}

	/**
	The instant as a `std::chrono::sys_time` of `Duration`, or `std::nullopt` when the instant is not valid. Built-in integer counts are limited to 64 bits and must hold the instant exactly; a count that does not fit or is not a whole number of `Duration` gives `std::nullopt`. A floating-point count uses ordinary floating-point conversion and may lose precision. A `std::chrono::sys_time<std::chrono::nanoseconds>` holds the years 1678 to 2262.
	*/
	template<class Duration = std::chrono::nanoseconds>
		requires detail::chrono_representation<typename Duration::rep>
	[[nodiscard]] constexpr std::optional<std::chrono::sys_time<Duration>> to_sys_time() const noexcept;

	friend constexpr auto operator<=>(const instant&, const instant&) = default;
};

/**
An array: a sequence of values.
*/
using array = std::vector<value>;

/**
An object: members in the order they were written or added.

Member order is not part of a value, as the spec says, so two objects with the same members in another order are equal. The writer keeps the order, and canonical form sorts by key.

Lookup is a linear scan, which is the fastest choice for the small objects of a config file. For an object with thousands of members that you look up often, copy them into a map.
*/
class object {
public:
	using value_type = member;
	using iterator = std::vector<member>::iterator;
	using const_iterator = std::vector<member>::const_iterator;
	using size_type = std::size_t;

	constexpr object() noexcept;
	constexpr object(const object& other);
	constexpr object(object&& other) noexcept;
	constexpr object& operator=(const object& other);
	constexpr object& operator=(object&& other) noexcept;
	constexpr ~object();

	/**
	An object with these members, in this order. A later member with the key of an earlier one replaces its value, and keeps the earlier position.
	*/
	constexpr object(std::initializer_list<member> members);

	/**
	Builds an object from a batch of members in O(n log n) time. The first occurrence of a key sets its position, and the last sets its value, as with `insert_or_assign`. Moving the vector avoids copying its values. Duplicate compaction reuses the input vector's allocation and retains its capacity.
	*/
	constexpr explicit object(std::vector<member> members);

	[[nodiscard]] constexpr std::size_t size() const noexcept;
	[[nodiscard]] constexpr bool empty() const noexcept;
	constexpr void reserve(std::size_t capacity);
	constexpr void clear() noexcept;

	[[nodiscard]] constexpr auto begin(this auto&& self SOML_LIFETIMEBOUND) noexcept {
		return self.object::members_.begin();
	}

	[[nodiscard]] constexpr auto end(this auto&& self SOML_LIFETIMEBOUND) noexcept {
		return self.object::members_.end();
	}

	/**
	The members, in order.
	*/
	[[nodiscard]] constexpr std::span<const member> members() const noexcept SOML_LIFETIMEBOUND;

	/**
	The value of the member with this key, or `nullptr`.
	*/
	[[nodiscard]] constexpr auto find(this auto&& self SOML_LIFETIMEBOUND, std::string_view key) noexcept;

	[[nodiscard]] constexpr bool contains(std::string_view key) const noexcept;

	/**
	The value of the member with this key. A new member with a null value is added at the end when there is none. As with `std::vector`, adding a member can move the others, so it invalidates references to them, including one from an earlier `operator[]` in the same expression.
	*/
	constexpr soml::value& operator[](std::string_view key) SOML_LIFETIMEBOUND;

	/**
	Sets the member with this key to `value`, or adds it at the end when there is none.
	*/
	constexpr soml::value& insert_or_assign(std::string_view key, soml::value value) SOML_LIFETIMEBOUND;

	/**
	Removes the member with this key, and keeps the order of the others. Returns whether there was one.
	*/
	constexpr bool erase(std::string_view key);

	friend constexpr bool operator==(const object& left, const object& right);

private:
	friend struct detail::object_access;
	std::vector<member> members_;
};

/**
A value: null, a bool, an int, a float, a string, a date, an instant, a duration, an array, or an object.

An int and a float are different types, as in the spec, so `3` and `3.0` are not equal and nothing converts one into the other. A default value is null.

```
soml::value config = soml::object{
	{"name", "api-gateway"},
	{"replicas", 3},
	{"timeout", soml::duration{std::chrono::seconds{30}}},
	{"labels", soml::array{"prod", "eu-west"}},
};
```
*/
class value {
public:
	using storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, soml::date, soml::instant, soml::duration, soml::array, soml::object>;

	constexpr value() noexcept = default;

	constexpr value(std::nullptr_t) noexcept {} // NOLINT(google-explicit-constructor)

	/**
	A bool. It is a template, so that a pointer, a `char`, or a `std::size_t` does not silently become a bool.
	*/
	template<std::same_as<bool> Boolean>
	constexpr value(Boolean boolean) noexcept : // NOLINT(google-explicit-constructor)
		storage_(static_cast<bool>(boolean)) {}

	/**
	An int. A `std::uint64_t` or `std::size_t` is not accepted here, because it can be above the int64 range; cast it, or use `soml::to_value`, which checks the range. Neither is `char` or another character type, which is a character rather than a number.
	*/
	template<std::integral Integer>
		requires (!std::same_as<Integer, bool> && !detail::is_character<Integer> && sizeof(Integer) <= sizeof(std::int64_t) && (std::signed_integral<Integer> || sizeof(Integer) < sizeof(std::int64_t)))
	constexpr value(Integer integer) noexcept : // NOLINT(google-explicit-constructor)
		storage_(static_cast<std::int64_t>(integer)) {}

	template<std::floating_point Float>
		requires (sizeof(Float) <= sizeof(double))
	constexpr value(Float number) noexcept : // NOLINT(google-explicit-constructor)
		storage_(static_cast<double>(number)) {}

	constexpr value(std::string string) noexcept : // NOLINT(google-explicit-constructor)
		storage_(std::move(string)) {}

	// Note: Not using std::in_place_type<std::string> because it regressed common allocating string lengths with libstdc++, despite reducing constructor code size.
	constexpr value(std::string_view string) : // NOLINT(google-explicit-constructor)
		storage_(std::string(string)) {}

	constexpr value(const char* string) : // NOLINT(google-explicit-constructor)
		storage_(std::string(string)) {}

	constexpr value(soml::date calendar) noexcept : // NOLINT(google-explicit-constructor)
		storage_(calendar) {}

	constexpr value(soml::instant instant) noexcept : // NOLINT(google-explicit-constructor)
		storage_(instant) {}

	/**
	An instant from a `std::chrono::sys_time` with a built-in integer count of at most 64 bits, such as `std::chrono::system_clock::now()`.
	*/
	template<class Duration>
		requires (!std::chrono::treat_as_floating_point_v<typename Duration::rep> && detail::chrono_representation<typename Duration::rep>)
	constexpr value(std::chrono::sys_time<Duration> time) noexcept : // NOLINT(google-explicit-constructor)
		storage_(soml::instant{time}) {}

	/**
	An exact `soml::duration`. Use `soml::to_value` for other `std::chrono::duration` types, which may require rounding or fail the range check.
	*/
	template<std::same_as<soml::duration> Duration>
	constexpr value(Duration length) noexcept : // NOLINT(google-explicit-constructor)
		storage_(length) {}

	constexpr value(soml::array items) noexcept : // NOLINT(google-explicit-constructor)
		storage_(std::move(items)) {}

	constexpr value(soml::object members) noexcept : // NOLINT(google-explicit-constructor)
		storage_(std::move(members)) {}

	constexpr value(const value& other) = default;
	constexpr value(value&& other) noexcept = default;
	constexpr ~value() = default;

	/**
	Assignment copies or moves the source out first, so that a value can be assigned a part of itself, as in `config = *config.find("server")`.
	*/
	constexpr value& operator=(const value& other) {
		// Note: Not using copy-and-swap because it grew assignment code with libstdc++ and regressed scalar variant assignment with libc++.
		if (this != &other) {
			storage copy = other.storage_;
			storage_ = std::move(copy);
		}

		return *this;
	}

	constexpr value& operator=(value&& other) noexcept {
		if (this != &other) {
			storage moved = std::move(other.storage_);
			storage_ = std::move(moved);
		}

		return *this;
	}

	[[nodiscard]] constexpr soml::type type() const noexcept {
		// The alternatives of `storage` are in the order of `soml::type`, which a `static_assert` after the class checks.
		return static_cast<soml::type>(storage_.index());
	}

	// Note: Not using std::holds_alternative because it brought no measured benefit for these queries; GCC already generates identical code.
	[[nodiscard]] constexpr bool is_null() const noexcept {
		return type() == soml::type::null;
	}

	[[nodiscard]] constexpr bool is_bool() const noexcept {
		return type() == soml::type::boolean;
	}

	[[nodiscard]] constexpr bool is_int() const noexcept {
		return type() == soml::type::integer;
	}

	[[nodiscard]] constexpr bool is_float() const noexcept {
		return type() == soml::type::floating;
	}

	[[nodiscard]] constexpr bool is_string() const noexcept {
		return type() == soml::type::string;
	}

	[[nodiscard]] constexpr bool is_date() const noexcept {
		return type() == soml::type::date;
	}

	[[nodiscard]] constexpr bool is_instant() const noexcept {
		return type() == soml::type::instant;
	}

	[[nodiscard]] constexpr bool is_duration() const noexcept {
		return type() == soml::type::duration;
	}

	[[nodiscard]] constexpr bool is_array() const noexcept {
		return type() == soml::type::array;
	}

	[[nodiscard]] constexpr bool is_object() const noexcept {
		return type() == soml::type::object;
	}

	/**
	A pointer to the value as `T`, or `nullptr` when it holds another type. `T` is one of `std::nullptr_t`, `bool`, `std::int64_t`, `double`, `std::string`, `soml::date`, `soml::instant`, `soml::duration`, `soml::array`, and `soml::object`.

	```
	if (const auto* name = value.get_if<std::string>()) {
		…
	}
	```
	*/
	template<class T, class Self>
	[[nodiscard]] constexpr auto get_if(this Self&& self SOML_LIFETIMEBOUND) noexcept {
		return std::get_if<T>(&self.value::storage_);
	}

	/**
	The value as `T`, or `std::nullopt` when it does not hold a `T`, as `soml::from_value` reads it. So `as<int>()` gives an int that fits an `int`, and `as<std::chrono::seconds>()` gives a duration that is a whole number of seconds. `as<std::string_view>()` gives a view of the string in the value.
	*/
	template<class T>
	[[nodiscard]] constexpr std::optional<T> as() const;

	/**
	The member with this key of an object, or an empty view: for another key, for a value that is not an object, and when chained after an empty view. It never throws, so lookups chain:

	```
	auto port = config["server"]["port"].as<int>().value_or(8080);
	```
	*/
	[[nodiscard]] constexpr soml::view operator[](std::string_view key) const noexcept SOML_LIFETIMEBOUND;

	/**
	The item at this index of an array, or an empty view.
	*/
	[[nodiscard]] constexpr soml::view operator[](std::size_t index) const noexcept SOML_LIFETIMEBOUND;

	/**
	The value of the member with this key, when this is an object that has one, or `nullptr`.
	*/
	[[nodiscard]] constexpr auto find(this auto&& self SOML_LIFETIMEBOUND, std::string_view key) noexcept;

	/**
	The number of items of an array, or members of an object, or 0 for any other value.
	*/
	[[nodiscard]] constexpr std::size_t size() const noexcept;

	/**
	The underlying `std::variant`, for `std::visit`.
	*/
	[[nodiscard]] constexpr auto& variant(this auto&& self SOML_LIFETIMEBOUND) noexcept {
		return self.value::storage_;
	}

	/**
	Equality as the spec defines it: members in any order, `0.0` equal to `-0.0`, and an int never equal to a float.
	*/
	friend constexpr bool operator==(const value& left, const value& right);

private:
	storage storage_;
};

static_assert(std::same_as<std::variant_alternative_t<static_cast<std::size_t>(type::integer), value::storage>, std::int64_t> && std::same_as<std::variant_alternative_t<static_cast<std::size_t>(type::object), value::storage>, object> && std::variant_size_v<value::storage> == static_cast<std::size_t>(type::object) + 1);

/**
A member of an object.
*/
struct member {
	std::string key;
	soml::value value;

	friend constexpr bool operator==(const member&, const member&) = default;
};

/**
A view of a value that may be missing, for lookups that chain. Returned by `value::operator[]`. It points into the value it came from, so it must not outlive it.
*/
class view {
public:
	constexpr view() noexcept = default;

	constexpr view(const soml::value* pointer) noexcept : // NOLINT(google-explicit-constructor)
		pointer_(pointer) {}

	constexpr view(const soml::value& value SOML_LIFETIMEBOUND) noexcept : // NOLINT(google-explicit-constructor)
		pointer_(&value) {}

	/**
	Whether the value exists. A null value exists.
	*/
	[[nodiscard]] constexpr bool has_value() const noexcept {
		return pointer_ != nullptr;
	}

	constexpr explicit operator bool() const noexcept {
		return has_value();
	}

	/**
	The value, or `nullptr`.
	*/
	[[nodiscard]] constexpr const soml::value* get() const noexcept {
		return pointer_;
	}

	[[nodiscard]] constexpr const soml::value* operator->() const noexcept {
		return pointer_;
	}

	/**
	The value, which must not be missing.
	*/
	[[nodiscard]] constexpr const soml::value& operator*() const noexcept {
		return *pointer_;
	}

	/**
	The type of the value, or `std::nullopt` when it is missing.
	*/
	[[nodiscard]] constexpr std::optional<soml::type> type() const noexcept {
		return pointer_ == nullptr ? std::nullopt : std::optional{pointer_->type()};
	}

	[[nodiscard]] constexpr view operator[](std::string_view key) const noexcept {
		return pointer_ == nullptr ? view{} : (*pointer_)[key];
	}

	[[nodiscard]] constexpr view operator[](std::size_t index) const noexcept {
		return pointer_ == nullptr ? view{} : (*pointer_)[index];
	}

	/**
	The value as `T`, as `value::as`, or `std::nullopt` when it is missing.
	*/
	template<class T>
	[[nodiscard]] constexpr std::optional<T> as() const {
		return pointer_ == nullptr ? std::nullopt : pointer_->as<T>();
	}

	/**
	The value as `T`, or `fallback` when it is missing or not a `T`.
	*/
	template<class T>
		requires (!std::same_as<T, char*>)
	[[nodiscard]] constexpr T value_or(T fallback) const {
		return as<T>().value_or(std::move(fallback));
	}

	/**
	A string value, or `fallback`.
	*/
	[[nodiscard]] constexpr std::string_view value_or(const char* fallback) const;

	/**
	Two views are equal when both are empty, or both have values that are equal.
	*/
	friend constexpr bool operator==(const view& left, const view& right) {
		if (left.pointer_ == nullptr || right.pointer_ == nullptr) {
			return left.pointer_ == right.pointer_;
		}

		return *left.pointer_ == *right.pointer_;
	}

private:
	// Note: Not using std::optional<const value&> because it grew equality code on both tested libraries despite keeping pointer-sized storage.
	const soml::value* pointer_ = nullptr;
};

// MARK: Reading and writing

/**
Reads a UTF-8 document into an owning object or array.

The input need not outlive the result. Works in constant expressions:

```
static_assert(soml::parse("port: 8080")->find("port")->as<int>() == 8080);
```

Checks every spec rule. Invalid UTF-8, a BOM, and raw controls are checked first, in that order; otherwise reports the first error in the text. Error positions match the reference implementation's conformance cases.
*/
[[nodiscard]] constexpr std::expected<value, error> parse(std::string_view text);

/**
Reads a document from UTF-8 text in `char8_t`, as from a `u8"…"` literal.
*/
[[nodiscard]] constexpr std::expected<value, error> parse(std::u8string_view text);

/**
Reads a document from bytes, as read from a file.
*/
[[nodiscard]] std::expected<value, error> parse(std::span<const std::byte> bytes);

/**
Reads exactly the characters in a buffer, with no terminating NUL required. Embedded NUL bytes are rejected by the parser.
*/
[[nodiscard]] constexpr std::expected<value, error> parse(const std::vector<char>& text);

/**
Writes a document in member order: one entry per line, tabs, canonical spellings, and no commas or comments.

Validates the collection root, UTF-8, carriage returns, NaN, dates, instants, duplicate keys, and the 100-level nesting limit.
*/
[[nodiscard]] constexpr std::expected<std::string, error> write(const value& document);

/**
Writes as `soml::write`, sorting every object's keys. Equal values give identical bytes for hashing, signing, or comparison.
*/
[[nodiscard]] constexpr std::expected<std::string, error> write_canonical(const value& document);

/**
Appends a document in member order to `output`. Returns an error without changing `output` if the value is invalid. The complete document is rendered before appending, so allocation failures also leave `output` unchanged.
*/
[[nodiscard]] constexpr std::expected<void, error> write(const value& document, std::string& output);

/**
Appends a document in canonical form to `output`, with the same unchanged-output guarantee as `write(document, output)`.
*/
[[nodiscard]] constexpr std::expected<void, error> write_canonical(const value& document, std::string& output);

/**
Writes a native array or object without copying it into a value first. Validation and output are the same as the value overloads.
*/
template<class Root>
	requires (std::same_as<Root, array> || std::same_as<Root, object>)
[[nodiscard]] constexpr std::expected<std::string, error> write(const Root& document);

template<class Root>
	requires (std::same_as<Root, array> || std::same_as<Root, object>)
[[nodiscard]] constexpr std::expected<std::string, error> write_canonical(const Root& document);

template<class Root>
	requires (std::same_as<Root, array> || std::same_as<Root, object>)
[[nodiscard]] constexpr std::expected<void, error> write(const Root& document, std::string& output);

template<class Root>
	requires (std::same_as<Root, array> || std::same_as<Root, object>)
[[nodiscard]] constexpr std::expected<void, error> write_canonical(const Root& document, std::string& output);

/**
Whether a key can be written without quotes: one or more ASCII letters, digits, `_`, or `-`.
*/
[[nodiscard]] constexpr bool is_bare_key(std::string_view key) noexcept;

// MARK: Typed mapping

/**
Options for decoding into C++ types.
*/
struct decode_options {
	/**
	Skip unknown struct keys instead of failing with `error_code::unknown_key`.
	*/
	bool ignore_unknown_keys = false;
};

/**
Converts between `T` and a value. Specialize it for a type that the library does not know:

```
template<>
struct soml::converter<rgb> {
	static constexpr std::expected<rgb, soml::error> from_value(const soml::value& value, const soml::decode_options&) {
		…
	}

	static constexpr std::expected<soml::value, soml::error> to_value(const rgb& color) {
		…
	}
};
```

Either function may be left out, for a type that is only read or only written.

A specialization may also provide `static std::optional<T> try_from_value(const value&)` for `value::as<T>()`, avoiding discarded error messages. Its success or failure must agree with `from_value` using the default options.
*/
template<class T>
struct converter;

/**
The fields of a struct, for compilers without static reflection, or to give the fields other keys. Specialize it as a tuple of `soml::field`:

```
template<>
inline constexpr auto soml::fields<server> = std::tuple{
	soml::field{"host", &server::host},
	soml::field{"port", &server::port},
	soml::field{"max-connections", &server::max_connections},
};
```
*/
template<class T>
inline constexpr auto fields = nullptr;

/**
One field of a struct for `soml::fields`: its key and its member pointer.
*/
template<class Class, class Member>
struct field {
	std::string_view key;
	Member Class::* pointer;
};

/**
The names of an enum's values, for compilers without static reflection, or to give them other names. Specialize it as an array of pairs:

```
template<>
inline constexpr auto soml::enum_names<level> = std::array{
	std::pair{level::debug, std::string_view{"debug"}},
	std::pair{level::info, std::string_view{"info"}},
};
```
*/
template<class Enum>
	requires std::is_enum_v<Enum>
inline constexpr auto enum_names = nullptr;

/**
Reads a C++ value from a value. See `soml::decode`.
*/
template<class T>
[[nodiscard]] constexpr std::expected<T, error> from_value(const value& source, const decode_options& options = {});

/**
Moves owned strings and containers from `source`, including those nested in typed containers, optionals, and structs. A source remains valid with an unspecified value after conversion, including a failed conversion that has already consumed some members. A native type mismatch leaves `source` unchanged.
*/
template<class T>
[[nodiscard]] constexpr std::expected<T, error> from_value(value&& source, const decode_options& options = {});

/**
Makes a value from a C++ value. See `soml::encode`.
*/
template<class T>
[[nodiscard]] constexpr std::expected<value, error> to_value(const T& source);

/**
Moves owned strings and containers into a value, including those nested in typed containers, optionals, and structs. Passing an lvalue or a const rvalue copies it instead. A moved source remains valid with an unspecified value, including after a failed conversion.
*/
template<class T>
	requires (!std::is_lvalue_reference_v<T> && !std::is_const_v<T>)
[[nodiscard]] constexpr std::expected<value, error> to_value(T&& source);

/**
Reads a document into a C++ value: a struct, a container, or any type that `soml::converter` knows and whose result can be moved or copied. `soml::from_value` also supports immovable results.

```
struct config {
	std::string name;
	int replicas = 1;
	std::chrono::seconds timeout{30};
	std::vector<std::string> labels;
};

auto config = soml::decode<struct config>(text);
```

A member that the document leaves out keeps its default value. A key that the struct has no member for is an error, unless `options.ignore_unknown_keys` is set. Structs are read with C++26 static reflection where the compiler has it, and with `soml::fields` otherwise.

Decoding errors include their path and source position. The position points to the value, or to the key for an unknown key. Positions are recovered only after a failure, so successful decoding does not track source locations. Unresolved or ambiguous diagnostic paths have no position.
*/
template<class T>
	requires std::is_move_constructible_v<std::expected<T, error>>
[[nodiscard]] constexpr std::expected<T, error> decode(std::string_view text, const decode_options& options = {});

/**
Writes a C++ value as a document, with the members in the order of the struct's fields. See `soml::write`.
*/
template<class T>
[[nodiscard]] constexpr std::expected<std::string, error> encode(const T& source);

/**
Encodes an owning temporary or rvalue, allowing its strings and containers to be moved during conversion. The source remains valid with an unspecified value, including after failure.
*/
template<class T>
	requires (!std::is_lvalue_reference_v<T> && !std::is_const_v<T>)
[[nodiscard]] constexpr std::expected<std::string, error> encode(T&& source);

/**
Writes a C++ value as a document in canonical form. See `soml::write_canonical`.
*/
template<class T>
[[nodiscard]] constexpr std::expected<std::string, error> encode_canonical(const T& source);

template<class T>
	requires (!std::is_lvalue_reference_v<T> && !std::is_const_v<T>)
[[nodiscard]] constexpr std::expected<std::string, error> encode_canonical(T&& source);

} // namespace soml

// MARK: Implementation

namespace soml::detail {

// MARK: Characters

constexpr bool is_digit(int character) noexcept {
	return character >= '0' && character <= '9';
}

constexpr bool is_letter(int character) noexcept {
	return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

constexpr bool is_bare_key_character(int character) noexcept {
	return is_letter(character) || is_digit(character) || character == '_' || character == '-';
}

constexpr bool is_space(int character) noexcept {
	return character == ' ' || character == '\t';
}

constexpr bool is_uppercase_hex_digit(int character) noexcept {
	return is_digit(character) || (character >= 'A' && character <= 'F');
}

constexpr bool is_hex_digit(int character) noexcept {
	return is_uppercase_hex_digit(character) || (character >= 'a' && character <= 'f');
}

/*
The value of a hexadecimal digit in either case.
*/
constexpr int hex_digit_value(int character) noexcept {
	return is_digit(character) ? character - '0' : (character | 0x20) - 'a' + 10;
}

/*
The value of a lowercase hexadecimal digit, as used in `\u{…}`, or -1.
*/
constexpr int lowercase_hex_value(int character) noexcept {
	if (is_digit(character)) {
		return character - '0';
	}

	return character >= 'a' && character <= 'f' ? character - 'a' + 10 : -1;
}

/*
The radix that a prefix letter names, as in `0x`, `0o`, and `0b`, or 0.
*/
constexpr int radix_of(int letter) noexcept {
	switch (letter) {
		case 'x': {
			return 16;
		}

		case 'o': {
			return 8;
		}

		case 'b': {
			return 2;
		}

		default: {
			return 0;
		}
	}
}

constexpr bool is_radix_digit(int character, int radix) noexcept {
	switch (radix) {
		case 16: {
			return is_uppercase_hex_digit(character);
		}

		case 8: {
			return character >= '0' && character <= '7';
		}

		case 2: {
			return character == '0' || character == '1';
		}

		default: {
			return is_digit(character);
		}
	}
}

/*
What may directly follow a scalar value: whitespace, a separator, a closing bracket, or the start of a comment.
*/
constexpr bool is_value_end(int character) noexcept {
	switch (character) {
		case ' ':
		case '\t':
		case '\n':
		case ',':
		case ']':
		case '}':
		case '#':
		case '/': {
			return true;
		}

		default: {
			return false;
		}
	}
}

/*
A number, a date, an instant, or a duration is read as one maximal run of these characters, and then checked as a whole.
*/
constexpr bool is_number_character(int character) noexcept {
	return is_letter(character) || is_digit(character) || character == '_' || character == '.' || character == ':' || character == '+' || character == '-';
}

/*
A C0 control character other than tab and line feed, or DEL, which may not appear raw anywhere.
*/
constexpr bool is_control(int character) noexcept {
	return (character >= 0 && character < 0x20 && character != '\t' && character != '\n') || character == 0x7F;
}

/*
A character as a byte from 0 to 255, because `char` is signed on most platforms.
*/
constexpr int byte_of(char character) noexcept {
	return static_cast<unsigned char>(character);
}

/*
The high bits of the ASCII byte lanes equal to `byte`. Setting each high bit before subtraction prevents borrowing between lanes.
*/
constexpr std::uint64_t ascii_equal_lanes(std::uint64_t word, std::uint64_t byte) noexcept {
	constexpr std::uint64_t high_bits = 0x8080'8080'8080'8080;
	constexpr std::uint64_t ones = 0x0101'0101'0101'0101;
	return ~(((word ^ (ones * byte)) | high_bits) - ones) & high_bits;
}

/*
The high bits of byte lanes below `byte` in an ASCII word. The bound is at most 128.
*/
constexpr std::uint64_t ascii_below_lanes(std::uint64_t word, std::uint64_t byte) noexcept {
	constexpr std::uint64_t high_bits = 0x8080'8080'8080'8080;
	return ~((word | high_bits) - 0x0101'0101'0101'0101 * byte) & high_bits;
}

/*
The byte offset of the first marked high bit in memory order, or eight when no byte is marked.
*/
constexpr std::size_t first_marked_byte(std::uint64_t lanes) noexcept {
	// Note: Not using std::byteswap because normalization generated extra byte and bit reversals on big-endian AArch64 compared with this direct bit count.
	if constexpr (std::endian::native == std::endian::little) {
		return static_cast<std::size_t>(std::countr_zero(lanes)) / 8;
	} else {
		return static_cast<std::size_t>(std::countl_zero(lanes)) / 8;
	}
}

/*
Skips spaces and tabs in a borrowed source string, with bounded word reads at runtime.
*/
constexpr std::size_t skip_spaces(std::string_view text, std::size_t index) noexcept {
	if !consteval {
		while (index < text.size() && text.size() - index >= sizeof(std::uint64_t)) {
			std::uint64_t word = 0;
			std::memcpy(&word, text.data() + index, sizeof(word));
			const std::uint64_t blanks = (ascii_equal_lanes(word, ' ') | ascii_equal_lanes(word, '\t')) & ~word;

			if (blanks != 0x8080'8080'8080'8080) {
				return index + first_marked_byte(blanks ^ 0x8080'8080'8080'8080);
			}

			index += sizeof(word);
		}
	}

	while (index < text.size() && is_space(text[index])) {
		index++;
	}

	return index;
}

// MARK: UTF-8

/*
The length of the valid UTF-8 sequence at `index`, or 0 when it is not valid. Overlong forms, surrogates, and values above U+10FFFF are not valid. When it is not valid only because the text ends too soon, `is_truncated` is set.
*/
constexpr std::size_t utf8_sequence_length(std::string_view text, std::size_t index, bool* is_truncated = nullptr) noexcept {
	const int first = byte_of(text[index]);

	if (first < 0x80) {
		return 1;
	}

	// The length from the first byte, and the range of the second byte, which rules out overlong forms, surrogates, and values above U+10FFFF.
	std::size_t length = 0;
	int low = 0x80;
	int high = 0xBF;

	if (first >= 0xC2 && first <= 0xDF) {
		length = 2;
	} else if (first >= 0xE0 && first <= 0xEF) {
		length = 3;

		if (first == 0xE0) {
			low = 0xA0;
		} else if (first == 0xED) {
			high = 0x9F;
		}
	} else if (first >= 0xF0 && first <= 0xF4) {
		length = 4;

		if (first == 0xF0) {
			low = 0x90;
		} else if (first == 0xF4) {
			high = 0x8F;
		}
	} else {
		return 0;
	}

	for (std::size_t offset = 1; offset < length; offset++) {
		if (index + offset >= text.size()) {
			if (is_truncated != nullptr) {
				*is_truncated = true;
			}

			return 0;
		}

		const int byte = byte_of(text[index + offset]);
		const int minimum = offset == 1 ? low : 0x80;
		const int maximum = offset == 1 ? high : 0xBF;

		if (byte < minimum || byte > maximum) {
			return 0;
		}
	}

	return length;
}

/*
The scalar value of the valid UTF-8 sequence at `index`.
*/
constexpr char32_t decode_utf8(std::string_view text, std::size_t index, std::size_t length) noexcept {
	if (length <= 1) {
		return static_cast<char32_t>(byte_of(text[index]));
	}

	auto scalar = static_cast<char32_t>(byte_of(text[index]) & (0x7F >> length));

	for (std::size_t offset = 1; offset < length; offset++) {
		scalar = (scalar << 6U) | static_cast<char32_t>(byte_of(text[index + offset]) & 0x3F);
	}

	return scalar;
}

/*
The scalar value of the valid UTF-8 sequence at `index`, checking its length first.
*/
constexpr char32_t decode_utf8(std::string_view text, std::size_t index) noexcept {
	return decode_utf8(text, index, utf8_sequence_length(text, index));
}

/*
A C1 or bidirectional control, or a Unicode line or paragraph separator, that can disrupt diagnostic layout.
*/
constexpr bool is_diagnostic_control(char32_t scalar) noexcept {
	return (scalar >= 0x80 && scalar < 0xA0) || (scalar >= 0x2028 && scalar <= 0x202E) || (scalar >= 0x2066 && scalar <= 0x2069) || scalar == 0x200E || scalar == 0x200F || scalar == 0x061C;
}

/*
Appends diagnostic text with layout-changing controls shown as U+FFFD, without changing ordinary Unicode text.
*/
constexpr void append_error_text(std::string& output, std::string_view text) {
	std::size_t start = 0;
	// Every control handled here starts with one of these three UTF-8 bytes.
	for (std::size_t index = text.find_first_of("\xC2\xD8\xE2"); index != std::string_view::npos; index = text.find_first_of("\xC2\xD8\xE2", index + 1)) {
		const std::size_t length = utf8_sequence_length(text, index);
		if (length != 0 && is_diagnostic_control(decode_utf8(text, index, length))) {
			output += text.substr(start, index - start);
			output += "�";
			start = index + length;
			index += length - 1;
		}
	}
	output += text.substr(start);
}

constexpr void append_utf8(std::string& output, char32_t scalar) {
	// Note: Not using append({...}) because it regressed growing-string and complete Unicode-escape parsing workloads with libstdc++, even for two and four bytes.
	if (scalar < 0x80) {
		output.push_back(static_cast<char>(scalar));
	} else if (scalar < 0x800) {
		output.append_range(std::array{static_cast<char>(0xC0 | (scalar >> 6U)), static_cast<char>(0x80 | (scalar & 0x3F))});
	} else if (scalar < 0x10000) {
		output.append_range(std::array{static_cast<char>(0xE0 | (scalar >> 12U)), static_cast<char>(0x80 | ((scalar >> 6U) & 0x3F)), static_cast<char>(0x80 | (scalar & 0x3F))});
	} else {
		output.append_range(std::array{static_cast<char>(0xF0 | (scalar >> 18U)), static_cast<char>(0x80 | ((scalar >> 12U) & 0x3F)), static_cast<char>(0x80 | ((scalar >> 6U) & 0x3F)), static_cast<char>(0x80 | (scalar & 0x3F))});
	}
}

/*
Whether a scalar value of U+00A0 or above is whitespace in Unicode (the White_Space property).
*/
constexpr bool is_unicode_space(char32_t scalar) noexcept {
	return scalar == 0xA0 || scalar == 0x1680 || (scalar >= 0x2000 && scalar <= 0x200A) || scalar == 0x2028 || scalar == 0x2029 || scalar == 0x202F || scalar == 0x205F || scalar == 0x3000;
}

/*
Whether a scalar value looks like whitespace but is not whitespace in SOML: a Unicode space, U+0085, U+200B, or U+FEFF.
*/
constexpr bool is_lookalike_space(char32_t scalar) noexcept {
	return scalar == 0x85 || scalar == 0x200B || scalar == 0xFEFF || is_unicode_space(scalar);
}

// BEGIN GENERATED UNICODE RANGES
// Unicode 17.0.0: General_Category Other or Separator, or Default_Ignorable_Code_Point.
// Generated by tools/generate-unicode.py.
inline constexpr std::array<std::pair<char32_t, char32_t>, 747> diagnostic_code_point_ranges{{
	{0x0, 0x20},
	{0x7F, 0xA0},
	{0xAD, 0xAD},
	{0x34F, 0x34F},
	{0x378, 0x379},
	{0x380, 0x383},
	{0x38B, 0x38B},
	{0x38D, 0x38D},
	{0x3A2, 0x3A2},
	{0x530, 0x530},
	{0x557, 0x558},
	{0x58B, 0x58C},
	{0x590, 0x590},
	{0x5C8, 0x5CF},
	{0x5EB, 0x5EE},
	{0x5F5, 0x605},
	{0x61C, 0x61C},
	{0x6DD, 0x6DD},
	{0x70E, 0x70F},
	{0x74B, 0x74C},
	{0x7B2, 0x7BF},
	{0x7FB, 0x7FC},
	{0x82E, 0x82F},
	{0x83F, 0x83F},
	{0x85C, 0x85D},
	{0x85F, 0x85F},
	{0x86B, 0x86F},
	{0x890, 0x896},
	{0x8E2, 0x8E2},
	{0x984, 0x984},
	{0x98D, 0x98E},
	{0x991, 0x992},
	{0x9A9, 0x9A9},
	{0x9B1, 0x9B1},
	{0x9B3, 0x9B5},
	{0x9BA, 0x9BB},
	{0x9C5, 0x9C6},
	{0x9C9, 0x9CA},
	{0x9CF, 0x9D6},
	{0x9D8, 0x9DB},
	{0x9DE, 0x9DE},
	{0x9E4, 0x9E5},
	{0x9FF, 0xA00},
	{0xA04, 0xA04},
	{0xA0B, 0xA0E},
	{0xA11, 0xA12},
	{0xA29, 0xA29},
	{0xA31, 0xA31},
	{0xA34, 0xA34},
	{0xA37, 0xA37},
	{0xA3A, 0xA3B},
	{0xA3D, 0xA3D},
	{0xA43, 0xA46},
	{0xA49, 0xA4A},
	{0xA4E, 0xA50},
	{0xA52, 0xA58},
	{0xA5D, 0xA5D},
	{0xA5F, 0xA65},
	{0xA77, 0xA80},
	{0xA84, 0xA84},
	{0xA8E, 0xA8E},
	{0xA92, 0xA92},
	{0xAA9, 0xAA9},
	{0xAB1, 0xAB1},
	{0xAB4, 0xAB4},
	{0xABA, 0xABB},
	{0xAC6, 0xAC6},
	{0xACA, 0xACA},
	{0xACE, 0xACF},
	{0xAD1, 0xADF},
	{0xAE4, 0xAE5},
	{0xAF2, 0xAF8},
	{0xB00, 0xB00},
	{0xB04, 0xB04},
	{0xB0D, 0xB0E},
	{0xB11, 0xB12},
	{0xB29, 0xB29},
	{0xB31, 0xB31},
	{0xB34, 0xB34},
	{0xB3A, 0xB3B},
	{0xB45, 0xB46},
	{0xB49, 0xB4A},
	{0xB4E, 0xB54},
	{0xB58, 0xB5B},
	{0xB5E, 0xB5E},
	{0xB64, 0xB65},
	{0xB78, 0xB81},
	{0xB84, 0xB84},
	{0xB8B, 0xB8D},
	{0xB91, 0xB91},
	{0xB96, 0xB98},
	{0xB9B, 0xB9B},
	{0xB9D, 0xB9D},
	{0xBA0, 0xBA2},
	{0xBA5, 0xBA7},
	{0xBAB, 0xBAD},
	{0xBBA, 0xBBD},
	{0xBC3, 0xBC5},
	{0xBC9, 0xBC9},
	{0xBCE, 0xBCF},
	{0xBD1, 0xBD6},
	{0xBD8, 0xBE5},
	{0xBFB, 0xBFF},
	{0xC0D, 0xC0D},
	{0xC11, 0xC11},
	{0xC29, 0xC29},
	{0xC3A, 0xC3B},
	{0xC45, 0xC45},
	{0xC49, 0xC49},
	{0xC4E, 0xC54},
	{0xC57, 0xC57},
	{0xC5B, 0xC5B},
	{0xC5E, 0xC5F},
	{0xC64, 0xC65},
	{0xC70, 0xC76},
	{0xC8D, 0xC8D},
	{0xC91, 0xC91},
	{0xCA9, 0xCA9},
	{0xCB4, 0xCB4},
	{0xCBA, 0xCBB},
	{0xCC5, 0xCC5},
	{0xCC9, 0xCC9},
	{0xCCE, 0xCD4},
	{0xCD7, 0xCDB},
	{0xCDF, 0xCDF},
	{0xCE4, 0xCE5},
	{0xCF0, 0xCF0},
	{0xCF4, 0xCFF},
	{0xD0D, 0xD0D},
	{0xD11, 0xD11},
	{0xD45, 0xD45},
	{0xD49, 0xD49},
	{0xD50, 0xD53},
	{0xD64, 0xD65},
	{0xD80, 0xD80},
	{0xD84, 0xD84},
	{0xD97, 0xD99},
	{0xDB2, 0xDB2},
	{0xDBC, 0xDBC},
	{0xDBE, 0xDBF},
	{0xDC7, 0xDC9},
	{0xDCB, 0xDCE},
	{0xDD5, 0xDD5},
	{0xDD7, 0xDD7},
	{0xDE0, 0xDE5},
	{0xDF0, 0xDF1},
	{0xDF5, 0xE00},
	{0xE3B, 0xE3E},
	{0xE5C, 0xE80},
	{0xE83, 0xE83},
	{0xE85, 0xE85},
	{0xE8B, 0xE8B},
	{0xEA4, 0xEA4},
	{0xEA6, 0xEA6},
	{0xEBE, 0xEBF},
	{0xEC5, 0xEC5},
	{0xEC7, 0xEC7},
	{0xECF, 0xECF},
	{0xEDA, 0xEDB},
	{0xEE0, 0xEFF},
	{0xF48, 0xF48},
	{0xF6D, 0xF70},
	{0xF98, 0xF98},
	{0xFBD, 0xFBD},
	{0xFCD, 0xFCD},
	{0xFDB, 0xFFF},
	{0x10C6, 0x10C6},
	{0x10C8, 0x10CC},
	{0x10CE, 0x10CF},
	{0x115F, 0x1160},
	{0x1249, 0x1249},
	{0x124E, 0x124F},
	{0x1257, 0x1257},
	{0x1259, 0x1259},
	{0x125E, 0x125F},
	{0x1289, 0x1289},
	{0x128E, 0x128F},
	{0x12B1, 0x12B1},
	{0x12B6, 0x12B7},
	{0x12BF, 0x12BF},
	{0x12C1, 0x12C1},
	{0x12C6, 0x12C7},
	{0x12D7, 0x12D7},
	{0x1311, 0x1311},
	{0x1316, 0x1317},
	{0x135B, 0x135C},
	{0x137D, 0x137F},
	{0x139A, 0x139F},
	{0x13F6, 0x13F7},
	{0x13FE, 0x13FF},
	{0x1680, 0x1680},
	{0x169D, 0x169F},
	{0x16F9, 0x16FF},
	{0x1716, 0x171E},
	{0x1737, 0x173F},
	{0x1754, 0x175F},
	{0x176D, 0x176D},
	{0x1771, 0x1771},
	{0x1774, 0x177F},
	{0x17B4, 0x17B5},
	{0x17DE, 0x17DF},
	{0x17EA, 0x17EF},
	{0x17FA, 0x17FF},
	{0x180B, 0x180F},
	{0x181A, 0x181F},
	{0x1879, 0x187F},
	{0x18AB, 0x18AF},
	{0x18F6, 0x18FF},
	{0x191F, 0x191F},
	{0x192C, 0x192F},
	{0x193C, 0x193F},
	{0x1941, 0x1943},
	{0x196E, 0x196F},
	{0x1975, 0x197F},
	{0x19AC, 0x19AF},
	{0x19CA, 0x19CF},
	{0x19DB, 0x19DD},
	{0x1A1C, 0x1A1D},
	{0x1A5F, 0x1A5F},
	{0x1A7D, 0x1A7E},
	{0x1A8A, 0x1A8F},
	{0x1A9A, 0x1A9F},
	{0x1AAE, 0x1AAF},
	{0x1ADE, 0x1ADF},
	{0x1AEC, 0x1AFF},
	{0x1B4D, 0x1B4D},
	{0x1BF4, 0x1BFB},
	{0x1C38, 0x1C3A},
	{0x1C4A, 0x1C4C},
	{0x1C8B, 0x1C8F},
	{0x1CBB, 0x1CBC},
	{0x1CC8, 0x1CCF},
	{0x1CFB, 0x1CFF},
	{0x1F16, 0x1F17},
	{0x1F1E, 0x1F1F},
	{0x1F46, 0x1F47},
	{0x1F4E, 0x1F4F},
	{0x1F58, 0x1F58},
	{0x1F5A, 0x1F5A},
	{0x1F5C, 0x1F5C},
	{0x1F5E, 0x1F5E},
	{0x1F7E, 0x1F7F},
	{0x1FB5, 0x1FB5},
	{0x1FC5, 0x1FC5},
	{0x1FD4, 0x1FD5},
	{0x1FDC, 0x1FDC},
	{0x1FF0, 0x1FF1},
	{0x1FF5, 0x1FF5},
	{0x1FFF, 0x200F},
	{0x2028, 0x202F},
	{0x205F, 0x206F},
	{0x2072, 0x2073},
	{0x208F, 0x208F},
	{0x209D, 0x209F},
	{0x20C2, 0x20CF},
	{0x20F1, 0x20FF},
	{0x218C, 0x218F},
	{0x242A, 0x243F},
	{0x244B, 0x245F},
	{0x2B74, 0x2B75},
	{0x2CF4, 0x2CF8},
	{0x2D26, 0x2D26},
	{0x2D28, 0x2D2C},
	{0x2D2E, 0x2D2F},
	{0x2D68, 0x2D6E},
	{0x2D71, 0x2D7E},
	{0x2D97, 0x2D9F},
	{0x2DA7, 0x2DA7},
	{0x2DAF, 0x2DAF},
	{0x2DB7, 0x2DB7},
	{0x2DBF, 0x2DBF},
	{0x2DC7, 0x2DC7},
	{0x2DCF, 0x2DCF},
	{0x2DD7, 0x2DD7},
	{0x2DDF, 0x2DDF},
	{0x2E5E, 0x2E7F},
	{0x2E9A, 0x2E9A},
	{0x2EF4, 0x2EFF},
	{0x2FD6, 0x2FEF},
	{0x3000, 0x3000},
	{0x3040, 0x3040},
	{0x3097, 0x3098},
	{0x3100, 0x3104},
	{0x3130, 0x3130},
	{0x3164, 0x3164},
	{0x318F, 0x318F},
	{0x31E6, 0x31EE},
	{0x321F, 0x321F},
	{0xA48D, 0xA48F},
	{0xA4C7, 0xA4CF},
	{0xA62C, 0xA63F},
	{0xA6F8, 0xA6FF},
	{0xA7DD, 0xA7F0},
	{0xA82D, 0xA82F},
	{0xA83A, 0xA83F},
	{0xA878, 0xA87F},
	{0xA8C6, 0xA8CD},
	{0xA8DA, 0xA8DF},
	{0xA954, 0xA95E},
	{0xA97D, 0xA97F},
	{0xA9CE, 0xA9CE},
	{0xA9DA, 0xA9DD},
	{0xA9FF, 0xA9FF},
	{0xAA37, 0xAA3F},
	{0xAA4E, 0xAA4F},
	{0xAA5A, 0xAA5B},
	{0xAAC3, 0xAADA},
	{0xAAF7, 0xAB00},
	{0xAB07, 0xAB08},
	{0xAB0F, 0xAB10},
	{0xAB17, 0xAB1F},
	{0xAB27, 0xAB27},
	{0xAB2F, 0xAB2F},
	{0xAB6C, 0xAB6F},
	{0xABEE, 0xABEF},
	{0xABFA, 0xABFF},
	{0xD7A4, 0xD7AF},
	{0xD7C7, 0xD7CA},
	{0xD7FC, 0xF8FF},
	{0xFA6E, 0xFA6F},
	{0xFADA, 0xFAFF},
	{0xFB07, 0xFB12},
	{0xFB18, 0xFB1C},
	{0xFB37, 0xFB37},
	{0xFB3D, 0xFB3D},
	{0xFB3F, 0xFB3F},
	{0xFB42, 0xFB42},
	{0xFB45, 0xFB45},
	{0xFDD0, 0xFDEF},
	{0xFE00, 0xFE0F},
	{0xFE1A, 0xFE1F},
	{0xFE53, 0xFE53},
	{0xFE67, 0xFE67},
	{0xFE6C, 0xFE6F},
	{0xFE75, 0xFE75},
	{0xFEFD, 0xFF00},
	{0xFFA0, 0xFFA0},
	{0xFFBF, 0xFFC1},
	{0xFFC8, 0xFFC9},
	{0xFFD0, 0xFFD1},
	{0xFFD8, 0xFFD9},
	{0xFFDD, 0xFFDF},
	{0xFFE7, 0xFFE7},
	{0xFFEF, 0xFFFB},
	{0xFFFE, 0xFFFF},
	{0x1000C, 0x1000C},
	{0x10027, 0x10027},
	{0x1003B, 0x1003B},
	{0x1003E, 0x1003E},
	{0x1004E, 0x1004F},
	{0x1005E, 0x1007F},
	{0x100FB, 0x100FF},
	{0x10103, 0x10106},
	{0x10134, 0x10136},
	{0x1018F, 0x1018F},
	{0x1019D, 0x1019F},
	{0x101A1, 0x101CF},
	{0x101FE, 0x1027F},
	{0x1029D, 0x1029F},
	{0x102D1, 0x102DF},
	{0x102FC, 0x102FF},
	{0x10324, 0x1032C},
	{0x1034B, 0x1034F},
	{0x1037B, 0x1037F},
	{0x1039E, 0x1039E},
	{0x103C4, 0x103C7},
	{0x103D6, 0x103FF},
	{0x1049E, 0x1049F},
	{0x104AA, 0x104AF},
	{0x104D4, 0x104D7},
	{0x104FC, 0x104FF},
	{0x10528, 0x1052F},
	{0x10564, 0x1056E},
	{0x1057B, 0x1057B},
	{0x1058B, 0x1058B},
	{0x10593, 0x10593},
	{0x10596, 0x10596},
	{0x105A2, 0x105A2},
	{0x105B2, 0x105B2},
	{0x105BA, 0x105BA},
	{0x105BD, 0x105BF},
	{0x105F4, 0x105FF},
	{0x10737, 0x1073F},
	{0x10756, 0x1075F},
	{0x10768, 0x1077F},
	{0x10786, 0x10786},
	{0x107B1, 0x107B1},
	{0x107BB, 0x107FF},
	{0x10806, 0x10807},
	{0x10809, 0x10809},
	{0x10836, 0x10836},
	{0x10839, 0x1083B},
	{0x1083D, 0x1083E},
	{0x10856, 0x10856},
	{0x1089F, 0x108A6},
	{0x108B0, 0x108DF},
	{0x108F3, 0x108F3},
	{0x108F6, 0x108FA},
	{0x1091C, 0x1091E},
	{0x1093A, 0x1093E},
	{0x1095A, 0x1097F},
	{0x109B8, 0x109BB},
	{0x109D0, 0x109D1},
	{0x10A04, 0x10A04},
	{0x10A07, 0x10A0B},
	{0x10A14, 0x10A14},
	{0x10A18, 0x10A18},
	{0x10A36, 0x10A37},
	{0x10A3B, 0x10A3E},
	{0x10A49, 0x10A4F},
	{0x10A59, 0x10A5F},
	{0x10AA0, 0x10ABF},
	{0x10AE7, 0x10AEA},
	{0x10AF7, 0x10AFF},
	{0x10B36, 0x10B38},
	{0x10B56, 0x10B57},
	{0x10B73, 0x10B77},
	{0x10B92, 0x10B98},
	{0x10B9D, 0x10BA8},
	{0x10BB0, 0x10BFF},
	{0x10C49, 0x10C7F},
	{0x10CB3, 0x10CBF},
	{0x10CF3, 0x10CF9},
	{0x10D28, 0x10D2F},
	{0x10D3A, 0x10D3F},
	{0x10D66, 0x10D68},
	{0x10D86, 0x10D8D},
	{0x10D90, 0x10E5F},
	{0x10E7F, 0x10E7F},
	{0x10EAA, 0x10EAA},
	{0x10EAE, 0x10EAF},
	{0x10EB2, 0x10EC1},
	{0x10EC8, 0x10ECF},
	{0x10ED9, 0x10EF9},
	{0x10F28, 0x10F2F},
	{0x10F5A, 0x10F6F},
	{0x10F8A, 0x10FAF},
	{0x10FCC, 0x10FDF},
	{0x10FF7, 0x10FFF},
	{0x1104E, 0x11051},
	{0x11076, 0x1107E},
	{0x110BD, 0x110BD},
	{0x110C3, 0x110CF},
	{0x110E9, 0x110EF},
	{0x110FA, 0x110FF},
	{0x11135, 0x11135},
	{0x11148, 0x1114F},
	{0x11177, 0x1117F},
	{0x111E0, 0x111E0},
	{0x111F5, 0x111FF},
	{0x11212, 0x11212},
	{0x11242, 0x1127F},
	{0x11287, 0x11287},
	{0x11289, 0x11289},
	{0x1128E, 0x1128E},
	{0x1129E, 0x1129E},
	{0x112AA, 0x112AF},
	{0x112EB, 0x112EF},
	{0x112FA, 0x112FF},
	{0x11304, 0x11304},
	{0x1130D, 0x1130E},
	{0x11311, 0x11312},
	{0x11329, 0x11329},
	{0x11331, 0x11331},
	{0x11334, 0x11334},
	{0x1133A, 0x1133A},
	{0x11345, 0x11346},
	{0x11349, 0x1134A},
	{0x1134E, 0x1134F},
	{0x11351, 0x11356},
	{0x11358, 0x1135C},
	{0x11364, 0x11365},
	{0x1136D, 0x1136F},
	{0x11375, 0x1137F},
	{0x1138A, 0x1138A},
	{0x1138C, 0x1138D},
	{0x1138F, 0x1138F},
	{0x113B6, 0x113B6},
	{0x113C1, 0x113C1},
	{0x113C3, 0x113C4},
	{0x113C6, 0x113C6},
	{0x113CB, 0x113CB},
	{0x113D6, 0x113D6},
	{0x113D9, 0x113E0},
	{0x113E3, 0x113FF},
	{0x1145C, 0x1145C},
	{0x11462, 0x1147F},
	{0x114C8, 0x114CF},
	{0x114DA, 0x1157F},
	{0x115B6, 0x115B7},
	{0x115DE, 0x115FF},
	{0x11645, 0x1164F},
	{0x1165A, 0x1165F},
	{0x1166D, 0x1167F},
	{0x116BA, 0x116BF},
	{0x116CA, 0x116CF},
	{0x116E4, 0x116FF},
	{0x1171B, 0x1171C},
	{0x1172C, 0x1172F},
	{0x11747, 0x117FF},
	{0x1183C, 0x1189F},
	{0x118F3, 0x118FE},
	{0x11907, 0x11908},
	{0x1190A, 0x1190B},
	{0x11914, 0x11914},
	{0x11917, 0x11917},
	{0x11936, 0x11936},
	{0x11939, 0x1193A},
	{0x11947, 0x1194F},
	{0x1195A, 0x1199F},
	{0x119A8, 0x119A9},
	{0x119D8, 0x119D9},
	{0x119E5, 0x119FF},
	{0x11A48, 0x11A4F},
	{0x11AA3, 0x11AAF},
	{0x11AF9, 0x11AFF},
	{0x11B0A, 0x11B5F},
	{0x11B68, 0x11BBF},
	{0x11BE2, 0x11BEF},
	{0x11BFA, 0x11BFF},
	{0x11C09, 0x11C09},
	{0x11C37, 0x11C37},
	{0x11C46, 0x11C4F},
	{0x11C6D, 0x11C6F},
	{0x11C90, 0x11C91},
	{0x11CA8, 0x11CA8},
	{0x11CB7, 0x11CFF},
	{0x11D07, 0x11D07},
	{0x11D0A, 0x11D0A},
	{0x11D37, 0x11D39},
	{0x11D3B, 0x11D3B},
	{0x11D3E, 0x11D3E},
	{0x11D48, 0x11D4F},
	{0x11D5A, 0x11D5F},
	{0x11D66, 0x11D66},
	{0x11D69, 0x11D69},
	{0x11D8F, 0x11D8F},
	{0x11D92, 0x11D92},
	{0x11D99, 0x11D9F},
	{0x11DAA, 0x11DAF},
	{0x11DDC, 0x11DDF},
	{0x11DEA, 0x11EDF},
	{0x11EF9, 0x11EFF},
	{0x11F11, 0x11F11},
	{0x11F3B, 0x11F3D},
	{0x11F5B, 0x11FAF},
	{0x11FB1, 0x11FBF},
	{0x11FF2, 0x11FFE},
	{0x1239A, 0x123FF},
	{0x1246F, 0x1246F},
	{0x12475, 0x1247F},
	{0x12544, 0x12F8F},
	{0x12FF3, 0x12FFF},
	{0x13430, 0x1343F},
	{0x13456, 0x1345F},
	{0x143FB, 0x143FF},
	{0x14647, 0x160FF},
	{0x1613A, 0x167FF},
	{0x16A39, 0x16A3F},
	{0x16A5F, 0x16A5F},
	{0x16A6A, 0x16A6D},
	{0x16ABF, 0x16ABF},
	{0x16ACA, 0x16ACF},
	{0x16AEE, 0x16AEF},
	{0x16AF6, 0x16AFF},
	{0x16B46, 0x16B4F},
	{0x16B5A, 0x16B5A},
	{0x16B62, 0x16B62},
	{0x16B78, 0x16B7C},
	{0x16B90, 0x16D3F},
	{0x16D7A, 0x16E3F},
	{0x16E9B, 0x16E9F},
	{0x16EB9, 0x16EBA},
	{0x16ED4, 0x16EFF},
	{0x16F4B, 0x16F4E},
	{0x16F88, 0x16F8E},
	{0x16FA0, 0x16FDF},
	{0x16FE5, 0x16FEF},
	{0x16FF7, 0x16FFF},
	{0x18CD6, 0x18CFE},
	{0x18D1F, 0x18D7F},
	{0x18DF3, 0x1AFEF},
	{0x1AFF4, 0x1AFF4},
	{0x1AFFC, 0x1AFFC},
	{0x1AFFF, 0x1AFFF},
	{0x1B123, 0x1B131},
	{0x1B133, 0x1B14F},
	{0x1B153, 0x1B154},
	{0x1B156, 0x1B163},
	{0x1B168, 0x1B16F},
	{0x1B2FC, 0x1BBFF},
	{0x1BC6B, 0x1BC6F},
	{0x1BC7D, 0x1BC7F},
	{0x1BC89, 0x1BC8F},
	{0x1BC9A, 0x1BC9B},
	{0x1BCA0, 0x1CBFF},
	{0x1CCFD, 0x1CCFF},
	{0x1CEB4, 0x1CEB9},
	{0x1CED1, 0x1CEDF},
	{0x1CEF1, 0x1CEFF},
	{0x1CF2E, 0x1CF2F},
	{0x1CF47, 0x1CF4F},
	{0x1CFC4, 0x1CFFF},
	{0x1D0F6, 0x1D0FF},
	{0x1D127, 0x1D128},
	{0x1D173, 0x1D17A},
	{0x1D1EB, 0x1D1FF},
	{0x1D246, 0x1D2BF},
	{0x1D2D4, 0x1D2DF},
	{0x1D2F4, 0x1D2FF},
	{0x1D357, 0x1D35F},
	{0x1D379, 0x1D3FF},
	{0x1D455, 0x1D455},
	{0x1D49D, 0x1D49D},
	{0x1D4A0, 0x1D4A1},
	{0x1D4A3, 0x1D4A4},
	{0x1D4A7, 0x1D4A8},
	{0x1D4AD, 0x1D4AD},
	{0x1D4BA, 0x1D4BA},
	{0x1D4BC, 0x1D4BC},
	{0x1D4C4, 0x1D4C4},
	{0x1D506, 0x1D506},
	{0x1D50B, 0x1D50C},
	{0x1D515, 0x1D515},
	{0x1D51D, 0x1D51D},
	{0x1D53A, 0x1D53A},
	{0x1D53F, 0x1D53F},
	{0x1D545, 0x1D545},
	{0x1D547, 0x1D549},
	{0x1D551, 0x1D551},
	{0x1D6A6, 0x1D6A7},
	{0x1D7CC, 0x1D7CD},
	{0x1DA8C, 0x1DA9A},
	{0x1DAA0, 0x1DAA0},
	{0x1DAB0, 0x1DEFF},
	{0x1DF1F, 0x1DF24},
	{0x1DF2B, 0x1DFFF},
	{0x1E007, 0x1E007},
	{0x1E019, 0x1E01A},
	{0x1E022, 0x1E022},
	{0x1E025, 0x1E025},
	{0x1E02B, 0x1E02F},
	{0x1E06E, 0x1E08E},
	{0x1E090, 0x1E0FF},
	{0x1E12D, 0x1E12F},
	{0x1E13E, 0x1E13F},
	{0x1E14A, 0x1E14D},
	{0x1E150, 0x1E28F},
	{0x1E2AF, 0x1E2BF},
	{0x1E2FA, 0x1E2FE},
	{0x1E300, 0x1E4CF},
	{0x1E4FA, 0x1E5CF},
	{0x1E5FB, 0x1E5FE},
	{0x1E600, 0x1E6BF},
	{0x1E6DF, 0x1E6DF},
	{0x1E6F6, 0x1E6FD},
	{0x1E700, 0x1E7DF},
	{0x1E7E7, 0x1E7E7},
	{0x1E7EC, 0x1E7EC},
	{0x1E7EF, 0x1E7EF},
	{0x1E7FF, 0x1E7FF},
	{0x1E8C5, 0x1E8C6},
	{0x1E8D7, 0x1E8FF},
	{0x1E94C, 0x1E94F},
	{0x1E95A, 0x1E95D},
	{0x1E960, 0x1EC70},
	{0x1ECB5, 0x1ED00},
	{0x1ED3E, 0x1EDFF},
	{0x1EE04, 0x1EE04},
	{0x1EE20, 0x1EE20},
	{0x1EE23, 0x1EE23},
	{0x1EE25, 0x1EE26},
	{0x1EE28, 0x1EE28},
	{0x1EE33, 0x1EE33},
	{0x1EE38, 0x1EE38},
	{0x1EE3A, 0x1EE3A},
	{0x1EE3C, 0x1EE41},
	{0x1EE43, 0x1EE46},
	{0x1EE48, 0x1EE48},
	{0x1EE4A, 0x1EE4A},
	{0x1EE4C, 0x1EE4C},
	{0x1EE50, 0x1EE50},
	{0x1EE53, 0x1EE53},
	{0x1EE55, 0x1EE56},
	{0x1EE58, 0x1EE58},
	{0x1EE5A, 0x1EE5A},
	{0x1EE5C, 0x1EE5C},
	{0x1EE5E, 0x1EE5E},
	{0x1EE60, 0x1EE60},
	{0x1EE63, 0x1EE63},
	{0x1EE65, 0x1EE66},
	{0x1EE6B, 0x1EE6B},
	{0x1EE73, 0x1EE73},
	{0x1EE78, 0x1EE78},
	{0x1EE7D, 0x1EE7D},
	{0x1EE7F, 0x1EE7F},
	{0x1EE8A, 0x1EE8A},
	{0x1EE9C, 0x1EEA0},
	{0x1EEA4, 0x1EEA4},
	{0x1EEAA, 0x1EEAA},
	{0x1EEBC, 0x1EEEF},
	{0x1EEF2, 0x1EFFF},
	{0x1F02C, 0x1F02F},
	{0x1F094, 0x1F09F},
	{0x1F0AF, 0x1F0B0},
	{0x1F0C0, 0x1F0C0},
	{0x1F0D0, 0x1F0D0},
	{0x1F0F6, 0x1F0FF},
	{0x1F1AE, 0x1F1E5},
	{0x1F203, 0x1F20F},
	{0x1F23C, 0x1F23F},
	{0x1F249, 0x1F24F},
	{0x1F252, 0x1F25F},
	{0x1F266, 0x1F2FF},
	{0x1F6D9, 0x1F6DB},
	{0x1F6ED, 0x1F6EF},
	{0x1F6FD, 0x1F6FF},
	{0x1F7DA, 0x1F7DF},
	{0x1F7EC, 0x1F7EF},
	{0x1F7F1, 0x1F7FF},
	{0x1F80C, 0x1F80F},
	{0x1F848, 0x1F84F},
	{0x1F85A, 0x1F85F},
	{0x1F888, 0x1F88F},
	{0x1F8AE, 0x1F8AF},
	{0x1F8BC, 0x1F8BF},
	{0x1F8C2, 0x1F8CF},
	{0x1F8D9, 0x1F8FF},
	{0x1FA58, 0x1FA5F},
	{0x1FA6E, 0x1FA6F},
	{0x1FA7D, 0x1FA7F},
	{0x1FA8B, 0x1FA8D},
	{0x1FAC7, 0x1FAC7},
	{0x1FAC9, 0x1FACC},
	{0x1FADD, 0x1FADE},
	{0x1FAEB, 0x1FAEE},
	{0x1FAF9, 0x1FAFF},
	{0x1FB93, 0x1FB93},
	{0x1FBFB, 0x1FFFF},
	{0x2A6E0, 0x2A6FF},
	{0x2B81E, 0x2B81F},
	{0x2CEAE, 0x2CEAF},
	{0x2EBE1, 0x2EBEF},
	{0x2EE5E, 0x2F7FF},
	{0x2FA1E, 0x2FFFF},
	{0x3134B, 0x3134F},
	{0x3347A, 0x10FFFF},
}};
// END GENERATED UNICODE RANGES

/*
Whether a scalar value is invisible or ambiguous on its own, so a message shows it by its code point: a control, format, separator, private-use, unassigned, or default-ignorable character.
*/
constexpr bool is_shown_by_code_point(char32_t scalar) noexcept {
	if (scalar < 0x80) {
		return scalar <= 0x20 || scalar == 0x7F;
	}
	const auto range = std::ranges::lower_bound(diagnostic_code_point_ranges, scalar, {}, &std::pair<char32_t, char32_t>::second);
	return range != diagnostic_code_point_ranges.end() && range->first <= scalar;
}

/*
The number of Unicode scalar values in valid UTF-8 text.
*/
constexpr std::size_t count_scalars(std::string_view text) noexcept {
	std::size_t count = 0;
	std::size_t index = 0;
	if !consteval {
		for (; text.size() - index >= sizeof(std::uint64_t); index += sizeof(std::uint64_t)) {
			std::uint64_t word = 0;
			std::memcpy(&word, text.data() + index, sizeof(word));
			// A continuation byte has its high bit set and its next bit clear. Counting those lanes is independent of byte order.
			const std::uint64_t continuations = word & ~(word << 1U) & 0x8080'8080'8080'8080;
			count += sizeof(word) - static_cast<std::size_t>(std::popcount(continuations));
		}
	}

	for (const char character : text.substr(index)) {
		if ((byte_of(character) & 0xC0) != 0x80) {
			count++;
		}
	}

	return count;
}

// MARK: Numbers

/*
Appends a decimal integer with one string append. Twenty digits and a sign hold every 64-bit integer.
*/
template<std::integral Integer>
constexpr void append_integer(std::string& output, Integer number) {
	std::array<char, 21> buffer;
	const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
	output.append(std::string_view{buffer.data(), result.ptr});
}

/*
The magnitude of a 64-bit integer. For a negative value it is ~x + 1, which never wraps, so it holds for the smallest int64 too.
*/
constexpr std::uint64_t magnitude(std::int64_t number) noexcept {
	return number < 0 ? ~static_cast<std::uint64_t>(number) + 1 : static_cast<std::uint64_t>(number);
}

/*
The 64-bit integer with this sign and magnitude, which must be at most 2^63 for a negative value and at most the largest int64 for a positive one.
*/
constexpr std::int64_t to_signed(std::uint64_t magnitude, bool is_negative) noexcept {
	return std::bit_cast<std::int64_t>(is_negative ? std::uint64_t{0} - magnitude : magnitude);
}

/*
`left × right + addend`, or `false` when it overflows. Both compilers allow these builtins in constant expressions.
*/
constexpr bool multiply_add(std::uint64_t& output, std::uint64_t left, std::uint64_t right, std::uint64_t addend) noexcept {
	std::uint64_t product = 0;
	return !__builtin_mul_overflow(left, right, &product) && !__builtin_add_overflow(product, addend, &output);
}

constexpr bool add(std::uint64_t& output, std::uint64_t left, std::uint64_t right) noexcept {
	return !__builtin_add_overflow(left, right, &output);
}

/*
Appends a 32-bit integer with the chosen hexadecimal alphabet and at least `minimum_digits` digits.
*/
constexpr void append_hex(std::string& output, std::uint32_t number, std::size_t minimum_digits, std::string_view digits) {
	std::array<char, 8> buffer;
	std::size_t start = buffer.size();
	do {
		buffer[--start] = digits[number % 16];
		number /= 16;
	} while (number > 0);
	output.append(minimum_digits > buffer.size() - start ? minimum_digits - (buffer.size() - start) : 0, '0');
	output.append(buffer.data() + start, buffer.size() - start);
}

constexpr std::string hex_upper(std::uint32_t number, std::size_t minimum_digits) {
	std::string output;
	append_hex(output, number, minimum_digits, "0123456789ABCDEF");
	return output;
}

constexpr std::string hex_lower(std::uint32_t number) {
	std::string output;
	append_hex(output, number, 1, "0123456789abcdef");
	return output;
}

constexpr std::string decimal_string(std::uint64_t number) {
	std::string output;
	append_integer(output, number);
	return output;
}

// MARK: Dates

/*
Days since 1970-01-01 for a valid date in SOML's year range, using the standard Gregorian calendar conversion.
*/
constexpr std::int64_t days_from_civil(soml::date date) noexcept {
	return std::chrono::sys_days{date}.time_since_epoch().count();
}

constexpr std::int64_t days_in_month(std::int64_t year, std::int64_t month) noexcept {
	const std::chrono::year_month_day_last date{std::chrono::year{static_cast<int>(year)}, std::chrono::month_day_last{std::chrono::month{static_cast<unsigned>(month)}}};
	return static_cast<unsigned>(date.day());
}

/*
Checks the SOML year range as well as the native Gregorian calendar components.
*/
constexpr bool valid_date(soml::date date) noexcept {
	const int year = static_cast<int>(date.year());
	return year >= 1 && year <= 9999 && date.ok();
}

// MARK: High-precision decimal

/*
Fixed-capacity unsigned big integer, base 2^32, little-endian.

The number of limbs in use is tracked, so zero is `used_ == 0` and limbs outside the used prefix are ignored. 128 limbs hold 4096 bits, which covers every intermediate value in the conversions below.
*/
class bigint {
	constexpr auto& limb(this auto& self, int index) noexcept {
		return self.limbs_[static_cast<std::size_t>(index)];
	}

public:
	static constexpr int limb_capacity = 128;

	constexpr void clear() noexcept {
		used_ = 0;
	}

	constexpr bool is_zero() const noexcept {
		return used_ == 0;
	}

	constexpr int bit_length() const noexcept {
		if (used_ == 0) {
			return 0;
		}

		return (used_ - 1) * 32 + std::bit_width(limb(used_ - 1));
	}

	constexpr void assign_u64(std::uint64_t value) noexcept {
		clear();
		if (value != 0) {
			limb(0) = static_cast<std::uint32_t>(value);
			used_ = 1;
			std::uint64_t high = value >> 32;
			if (high != 0) {
				limb(1) = static_cast<std::uint32_t>(high);
				used_ = 2;
			}
		}
	}

	/*
	Build the integer from decimal digits, most significant first.
	*/
	constexpr void assign_digits(std::span<const std::uint8_t> digits) noexcept {
		static constexpr auto scales = [] consteval {
			std::array<std::uint32_t, 10> powers{1};
			for (std::size_t index = 1; index < powers.size(); ++index) {
				powers[index] = powers[index - 1] * 10;
			}
			return powers;
		}();

		clear();
		while (!digits.empty()) {
			const auto chunk_digits = digits.first(std::min(digits.size(), 9UZ));
			std::uint32_t chunk = 0;
			for (const auto digit : chunk_digits) {
				chunk = chunk * 10 + digit;
			}

			multiply_small(scales[chunk_digits.size()]);
			add_small(chunk);
			digits = digits.subspan(chunk_digits.size());
		}
	}

	constexpr void add_small(std::uint32_t value) noexcept {
		std::uint64_t carry = value;
		int index = 0;
		while (carry != 0 && index < limb_capacity) {
			std::uint64_t sum = static_cast<std::uint64_t>(limb_at(index)) + carry;
			limb(index) = static_cast<std::uint32_t>(sum);
			carry = sum >> 32;
			++index;
		}

		if (index > used_) {
			used_ = index;
		}
	}

	constexpr void multiply_small(std::uint32_t value) noexcept {
		if (used_ == 0) {
			return;
		}

		if (value == 0) {
			clear();
			return;
		}

		std::uint64_t carry = 0;
		for (int index = 0; index < used_; ++index) {
			std::uint64_t product = static_cast<std::uint64_t>(limb(index)) * value + carry;
			limb(index) = static_cast<std::uint32_t>(product);
			carry = product >> 32;
		}

		if (carry != 0 && used_ < limb_capacity) {
			limb(used_++) = static_cast<std::uint32_t>(carry);
		}
	}

	constexpr std::uint32_t divide_small(std::uint32_t divisor) noexcept {
		std::uint64_t remainder = 0;
		for (int index = used_ - 1; index >= 0; --index) {
			std::uint64_t current = (remainder << 32) | limb(index);
			limb(index) = static_cast<std::uint32_t>(current / divisor);
			remainder = current % divisor;
		}

		while (used_ > 0 && limb(used_ - 1) == 0) {
			--used_;
		}

		return static_cast<std::uint32_t>(remainder);
	}

	constexpr void shift_left(int bits) noexcept {
		if (bits <= 0 || used_ == 0) {
			return;
		}

		int words = bits / 32;
		int rest = bits % 32;
		int width = used_ + words + (rest != 0 ? 1 : 0);
		if (width > limb_capacity) {
			width = limb_capacity;
		}

		for (int index = width - 1; index >= 0; --index) {
			int source = index - words;
			std::uint32_t value = 0;
			if (source >= 0 && source < used_) {
				value = limb(source) << rest;
			}

			if (rest != 0 && source - 1 >= 0 && source - 1 < used_) {
				value |= limb(source - 1) >> (32 - rest);
			}

			limb(index) = value;
		}

		used_ = width;
		while (used_ > 0 && limb(used_ - 1) == 0) {
			--used_;
		}
	}

	/*
	Multiply by 5 `exponent` times, in chunks of 5^13 for speed.
	*/
	constexpr void multiply_pow5(int exponent) noexcept {
		while (exponent >= 13) {
			multiply_small(powers_of_five.back());
			exponent -= 13;
		}

		if (exponent > 0) {
			multiply_small(powers_of_five[static_cast<std::size_t>(exponent)]);
		}
	}

	/*
	Divide by 5 `exponent` times, setting `sticky` for every nonzero remainder.
	*/
	constexpr void divide_pow5(int exponent, bool& sticky) noexcept {
		while (exponent >= 13) {
			if (divide_small(powers_of_five.back()) != 0) {
				sticky = true;
			}

			exponent -= 13;
		}

		if (exponent > 0 && divide_small(powers_of_five[static_cast<std::size_t>(exponent)]) != 0) {
			sticky = true;
		}
	}

	/*
	Bits [shift, shift + 64) of the value; the caller guarantees they fit in 64 bits.
	*/
	constexpr std::uint64_t window_right(int shift) const noexcept {
		int word = shift >> 5;
		int offset = shift & 31;
		std::uint64_t low = limb_at(word) | (static_cast<std::uint64_t>(limb_at(word + 1)) << 32);
		std::uint64_t result = low >> offset;
		if (offset != 0) {
			result |= static_cast<std::uint64_t>(limb_at(word + 2)) << (64 - offset);
		}

		return result;
	}

	/*
	The value shifted left by `shift`; the caller guarantees that `shift` is below 64 and the result fits in 64 bits.
	*/
	constexpr std::uint64_t window_left(int shift) const noexcept {
		return window_right(0) << shift;
	}

	constexpr bool bit(int index) const noexcept {
		int word = index >> 5;
		return word >= 0 && word < used_ && ((limb(word) >> (index & 31)) & 1u) != 0;
	}

	/*
	True if any bit strictly below `index` is set.
	*/
	constexpr bool any_below(int index) const noexcept {
		if (index <= 0) {
			return false;
		}

		int word = index >> 5;
		int offset = index & 31;
		for (int i = 0; i < word && i < used_; ++i) {
			if (limb(i) != 0) {
				return true;
			}
		}

		if (offset != 0 && word < used_ && (limb(word) & ((1u << offset) - 1)) != 0) {
			return true;
		}

		return false;
	}

	/*
	Write the value as decimal digits, most significant first. Returns the digit count.
	*/
	constexpr int to_decimal(std::uint8_t* output) const noexcept {
		if (used_ == 0) {
			return 0;
		}

		bigint remaining = *this;
		std::array<std::uint32_t, limb_capacity * 2> chunks;
		int chunk_count = 0;
		while (!remaining.is_zero()) {
			chunks[static_cast<std::size_t>(chunk_count++)] = remaining.divide_small(1000000000u);
		}

		int length = 0;
		std::array<char, 9> buffer;
		const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), chunks[static_cast<std::size_t>(chunk_count - 1)]);
		for (const char digit : std::string_view{buffer.data(), converted.ptr}) {
			output[length++] = static_cast<std::uint8_t>(digit - '0');
		}

		for (int chunk = chunk_count - 2; chunk >= 0; --chunk) {
			std::uint32_t value = chunks[static_cast<std::size_t>(chunk)];
			for (int i = 8; i >= 0; --i) {
				output[length + i] = static_cast<std::uint8_t>(value % 10);
				value /= 10;
			}

			length += 9;
		}

		return length;
	}

private:
	static constexpr auto powers_of_five = [] consteval {
		std::array<std::uint32_t, 14> powers{1};
		for (std::size_t index = 1; index < powers.size(); ++index) {
			powers[index] = powers[index - 1] * 5;
		}
		return powers;
	}();

	constexpr std::uint32_t limb_at(int index) const noexcept {
		return index >= 0 && index < used_ ? limb(index) : 0;
	}

	std::array<std::uint32_t, limb_capacity> limbs_{};
	int used_ = 0;
};

/*
An exact decimal number, as digits and a decimal point, converted to and from binary64 or binary32. The value is 0.digits x 10^point.

The conversions scale the decimal exponent into a power of two with a base 2^32 big integer, multiplying or dividing by 5, then round the resulting bits to nearest with ties to even. `above` records nonzero digits dropped past the 800-digit capacity, so a value that looks exactly halfway is known to be above it.
*/
struct decimal {
	static constexpr int capacity = 800;

	std::array<std::uint8_t, capacity> digits{}; // Decimal digits, most significant first, without leading zeros
	int count = 0;                               // Number of digits
	int point = 0;                               // The value is 0.<digits> x 10^point
	bool above = false;                           // The exact value is strictly above the stored digits

	/*
	Read a validated positive float literal into the digits. Stops at 'e' or the end.
	*/
	constexpr void assign_significand(std::string_view literal) noexcept {
		count = 0;
		above = false;
		for (char character : literal) {
			if (character == 'e' || character == 'E') {
				break;
			}

			if (character < '0' || character > '9') {
				continue;
			}

			std::uint8_t digit = static_cast<std::uint8_t>(character - '0');
			if (count == 0 && digit == 0) {
				continue;
			}

			if (count == capacity) {
				if (digit != 0) {
					above = true;
				}

				continue;
			}

			digits[static_cast<std::size_t>(count++)] = digit;
		}

		// Trailing zeros do not affect the value or the discarded tail, and need no big-integer arithmetic.
		while (count > 0 && digits[static_cast<std::size_t>(count - 1)] == 0) {
			--count;
		}
	}

	/*
	Round to the nearest binary64, ties to even. `std::nullopt` means overflow.
	*/
	constexpr std::optional<double> to_double() const noexcept {
		return to_bits<52, -1023, 11>(digits.data(), count, point, above).transform([](std::uint64_t bits) {
			return std::bit_cast<double>(bits);
		});
	}

	/*
	The shortest digits that read back as the finite, positive value in `bits`, choosing the closest, and then an even last digit, among the shortest. The default parameters describe binary64; binary32 uses 23, -127, and 8.
	*/
	template<int significand_bits = 52, int exponent_offset = -1023, int exponent_width = 11>
	constexpr void assign_shortest(std::uint64_t bits) noexcept {
		std::uint64_t exponent_field = (bits >> significand_bits) & ((std::uint64_t{1} << exponent_width) - 1);
		std::uint64_t mantissa = bits & ((std::uint64_t{1} << significand_bits) - 1);
		std::uint64_t target = (exponent_field << significand_bits) | mantissa;

		std::uint64_t value;
		int exponent;
		if (exponent_field == 0) {
			value = mantissa;
			exponent = exponent_offset + 1 - significand_bits;
		} else {
			value = mantissa | (std::uint64_t{1} << significand_bits);
			exponent = static_cast<int>(exponent_field) + exponent_offset - significand_bits;
		}

		count = 0;
		point = 0;
		above = false;
		if (value == 0) {
			return;
		}

		// Cancel powers of two before constructing the exact decimal, avoiding redundant powers of five and trailing zeros.
		const int trailing_bits = std::countr_zero(value);
		value >>= trailing_bits;
		exponent += trailing_bits;

		// The exact value is `exact x 10^decimal_exponent`.
		bigint exact;
		exact.assign_u64(value);
		int decimal_exponent = 0;
		if (exponent >= 0) {
			exact.shift_left(exponent);
		} else {
			exact.multiply_pow5(-exponent);
			decimal_exponent = exponent;
		}

		int exact_count = exact.to_decimal(digits.data());
		int exact_point = exact_count + decimal_exponent;

		constexpr int maximum_digits = (significand_bits + 1) * 30103 / 100000 + 2;
		std::array<std::uint8_t, static_cast<std::size_t>(maximum_digits)> high_digits{};
		bool use_high = false;
		const auto find_candidate = [&](int length) constexpr {
			// Truncate to `length` significant digits.
			int low_count = length;
			while (low_count > 1 && digits[static_cast<std::size_t>(low_count - 1)] == 0) {
				--low_count;
			}

			// The truncation plus one unit in the last place.
			std::ranges::copy_n(digits.begin(), length, high_digits.begin());

			int high_count = length;
			int high_point = exact_point;
			int index = length - 1;
			while (index >= 0 && high_digits[static_cast<std::size_t>(index)] == 9) {
				high_digits[static_cast<std::size_t>(index)] = 0;
				--index;
			}

			if (index < 0) {
				high_digits[0] = 1;
				high_count = 1;
				++high_point;
			} else {
				++high_digits[static_cast<std::size_t>(index)];
				while (high_count > 1 && high_digits[static_cast<std::size_t>(high_count - 1)] == 0) {
					--high_count;
				}
			}

			// Try the closest candidate first, resolving ties to even, so a successful probe needs only one conversion.
			const bool prefer_high = closer_to_high(digits.data(), exact_count, length);
			for (const bool high : std::array{prefer_high, !prefer_high}) {
				const int candidate_count = high ? high_count : low_count;
				const int candidate_point = high ? high_point : exact_point;
				if (to_bits<significand_bits, exponent_offset, exponent_width>(high ? high_digits.data() : digits.data(), candidate_count, candidate_point, false) == target) {
					use_high = high;
					count = candidate_count;
					point = candidate_point;
					return true;
				}
			}

			return false;
		};

		// Decimal grids nest as precision increases: once a candidate round-trips, every longer length has one too.
		const auto lengths = std::views::iota(1, std::min(maximum_digits + 1, exact_count));
		auto shortest = lengths.begin();
		// Common one-digit values need only one probe; search the remaining precisions logarithmically.
		if (shortest != lengths.end() && !find_candidate(*shortest)) {
			shortest = std::ranges::partition_point(std::ranges::next(shortest), lengths.end(), [&](int length) constexpr {
				return !find_candidate(length);
			});
			// The search's last probe need not be the shortest, so restore that candidate before copying its digits.
			if (shortest != lengths.end()) {
				find_candidate(*shortest);
			}
		}
		if (shortest != lengths.end()) {
			if (use_high) {
				std::ranges::copy_n(high_digits.begin(), count, digits.begin());
			}
			return;
		}

		// No shorter prefix works, so keep the exact digits without converting them back to binary.
		count = exact_count;
		point = exact_point;
	}

private:
	template<int significand_bits, int exponent_offset, int exponent_width>
	static constexpr std::optional<std::uint64_t> to_bits(const std::uint8_t* source, int source_count, int source_point, bool source_above) noexcept {
		using u64 = std::uint64_t;
		constexpr int precision = significand_bits + 1;
		constexpr int minimum_exponent = exponent_offset + 1;
		constexpr int maximum_exponent = ((1 << exponent_width) - 2) + exponent_offset;

		if (source_count == 0) {
			return u64{0};
		}

		if (source_point < -330) {
			return u64{0};
		}

		if (source_point > 310) {
			return std::nullopt;
		}

		// The exact value is `(integer + fraction) x 2^shift` with `fraction` below one, and `sticky` covers every discarded nonzero part.
		bigint integer;
		integer.assign_digits(std::span{source, static_cast<std::size_t>(source_count)});
		bool sticky = source_above;
		int exponent = source_point - source_count;
		int shift = exponent;
		if (exponent >= 0) {
			integer.multiply_pow5(exponent);
		} else {
			int divisor = -exponent;

			// Scale so the quotient keeps at least 64 bits: the sticky flag alone is not enough when the quotient has few bits, because the discarded fraction then exceeds the rounding bit. `2322 / 1000` bounds `log2(5)` from above, so `bound` is a lower bound for the quotient bit length.
			int bound = integer.bit_length() - (2322 * divisor + 999) / 1000 - 3;
			int extra = bound >= 64 ? 0 : 64 - bound;
			if (extra > 0) {
				integer.shift_left(extra);
			}

			integer.divide_pow5(divisor, sticky);
			shift -= extra;
		}

		int length = integer.bit_length();
		int leading_exponent = length - 1 + shift;
		const int clamped = std::max(leading_exponent, minimum_exponent);
		int drop = clamped - significand_bits - shift;
		u64 mantissa;
		if (drop > 0) {
			mantissa = integer.window_right(drop);
			bool half = integer.bit(drop - 1);
			bool below = integer.any_below(drop - 1);
			if (half && (below || sticky || (mantissa & 1) != 0)) {
				++mantissa;
			}
		} else {
			mantissa = integer.window_left(-drop);
		}

		if (mantissa >= (u64{1} << precision)) {
			mantissa >>= 1;
			++leading_exponent;
		}

		if (mantissa == 0) {
			return u64{0};
		}

		if (leading_exponent > maximum_exponent) {
			return std::nullopt;
		}

		if (mantissa >= (u64{1} << significand_bits)) {
			// A subnormal that rounded up to exactly 2^emin is the smallest normal.
			const int biased = std::max(leading_exponent, minimum_exponent) - exponent_offset;
			return (u64(biased) << significand_bits) | (mantissa - (u64{1} << significand_bits));
		}

		return mantissa;
	}

	/*
	Whether to choose the truncation plus one unit: the closest value, with ties going to an even last digit.
	*/
	static constexpr bool closer_to_high(const std::uint8_t* source, int source_count, int length) noexcept {
		if (length >= source_count) {
			return false;
		}

		if (source[length] != 5) {
			return source[length] > 5;
		}

		for (int i = length + 1; i < source_count; ++i) {
			if (source[i] != 0) {
				return true;
			}
		}

		// Exactly halfway: keep the even last digit.
		return (source[length - 1] & 1) != 0;
	}
};

// MARK: Floats

inline constexpr std::array<double, 23> powers_of_ten{1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

enum class float_status : std::uint8_t {
	ok,
	too_large,
	too_small,
};

/*
Whether the current thread rounds floats to nearest, which Clinger's fast path and libc++'s `std::from_chars` need. Constant evaluation always rounds to nearest.
*/
constexpr bool rounds_to_nearest() noexcept {
	if consteval {
		return true;
	} else {
		// Note: Not using __builtin_fegetround because it is unavailable on Apple Clang and generated the same code as std::fegetround on GCC.
		return std::fegetround() == FE_TONEAREST;
	}
}

/*
A float literal that matches the grammar, measured: where its decimal point is, how many significant digits it has, and its first 19 digits.
*/
struct float_literal {
	// The number of significant digits, from the first nonzero digit to the last.
	std::int64_t significant_count = 0;
	// The decimal point relative to the first significant digit, so the value is 0.digits × 10^decimal_point. It saturates far beyond any finite double.
	std::int64_t decimal_point = 0;
	// The first 19 significant digits as an integer.
	std::uint64_t mantissa = 0;
	bool is_negative = false;
	bool has_underscore = false;
};

/*
Measures a float literal for range checking and the fast conversion paths.
*/
constexpr void scan_float(std::string_view literal, float_literal& output) noexcept {
	output.is_negative = literal.front() == '-';
	std::size_t index = output.is_negative ? 1 : 0;
	std::int64_t point = 0;
	std::int64_t count = 0;
	bool has_seen_point = false;

	for (; index < literal.size() && literal[index] != 'e'; index++) {
		const char character = literal[index];

		if (character == '_') {
			output.has_underscore = true;
			continue;
		}

		if (character == '.') {
			has_seen_point = true;
			continue;
		}

		const int digit = character - '0';

		if (count == 0 && digit == 0) {
			point -= has_seen_point ? 1 : 0;
			continue;
		}

		point += has_seen_point ? 0 : 1;
		count++;

		if (digit != 0) {
			output.significant_count = count;
		}

		if (count <= 19) {
			output.mantissa = (output.mantissa * 10) + static_cast<std::uint64_t>(digit);
		}

	}

	std::int64_t exponent = 0;

	if (index < literal.size()) {
		index++;
		const bool is_exponent_negative = index < literal.size() && literal[index] == '-';

		if (is_exponent_negative) {
			index++;
		}

		for (; index < literal.size(); index++) {
			if (literal[index] == '_') {
				output.has_underscore = true;
			} else if (exponent < 1'000'000'000'000) {
				exponent = (exponent * 10) + (literal[index] - '0');
			}
		}

		if (is_exponent_negative) {
			exponent = -exponent;
		}
	}

	// The mantissa holds the first min(count, 19) digits, so trailing zeros among them are dropped.
	for (std::int64_t extra = std::min<std::int64_t>(count, 19) - output.significant_count; extra > 0; extra--) {
		output.mantissa /= 10;
	}

	output.decimal_point = point + exponent;
}

/*
Converts a nonzero float literal exactly, with the high-precision decimal.
*/
constexpr float_status exact_float(std::string_view literal, const float_literal& scanned, double& output) noexcept {
	decimal digits;
	digits.assign_significand(literal);
	// A decimal point this far out is infinite or zero either way, so clamping it changes nothing.
	digits.point = static_cast<int>(std::clamp<std::int64_t>(scanned.decimal_point, -1000, 1000));
	const std::optional<double> result = digits.to_double();

	if (!result) {
		return float_status::too_large;
	}

	if (*result == 0.0) {
		return float_status::too_small;
	}

	// Note: Not using __builtin_copysign here or in parse_float because it needed an extra +/-1.0 sign argument without improving both compilers.
	output = scanned.is_negative ? -*result : *result;
	return float_status::ok;
}

/*
Converts a nonzero float literal exactly when its metadata has not been measured yet.
*/
constexpr float_status exact_float(std::string_view literal, double& output) noexcept {
	float_literal scanned;
	scan_float(literal, scanned);
	return exact_float(literal, scanned, output);
}

/*
Reads a float literal that matches the grammar, rounding to nearest with ties to even. A literal that is not zero but rounds to zero, or that rounds to an infinity, is an error, as the spec says. Negative zero becomes zero.

Clinger's fast path takes most literals: at most 15 significant digits are an exact binary64 integer, and so is 10^e up to 10^22, so one correctly rounded multiplication or division gives the correctly rounded value. Other literals go to `std::from_chars` (fast_float in both standard libraries), or to the high-precision decimal in constant evaluation and for rounding modes or tiny magnitudes that need the exact path.
*/
constexpr float_status parse_float(std::string_view literal, double& output) {
	float_literal scanned;
	scan_float(literal, scanned);

	if (scanned.significant_count == 0) {
		output = 0.0;
		return float_status::ok;
	}

	// Far beyond binary64's range, no second scan or temporary underscore-free copy is needed.
	if (scanned.decimal_point > 310) {
		return float_status::too_large;
	}
	if (scanned.decimal_point < -330) {
		return float_status::too_small;
	}

	const std::int64_t power = scanned.decimal_point - scanned.significant_count;
	// A const integral initializer can trial-evaluate if consteval as true at runtime.
	bool nearest = rounds_to_nearest();

	if (scanned.significant_count <= 15 && power >= -22 && power <= 22 && nearest) {
		const auto number = static_cast<double>(scanned.mantissa);
		const double result = power >= 0 ? number * powers_of_ten[static_cast<std::size_t>(power)] : number / powers_of_ten[static_cast<std::size_t>(-power)];
		output = scanned.is_negative ? -result : result;
		return float_status::ok;
	}

#if defined(_LIBCPP_VERSION)
	// libc++ rounds in the thread's rounding mode, so another mode takes the exact path below. It also takes 0.1 to 1 ms for a value below the normal range, such as `5e-324`, where the exact path takes microseconds, so a literal below 10^-307 takes the exact path too.
	const bool can_use_from_chars = nearest && scanned.decimal_point > -307;
#else
	const bool can_use_from_chars = true;
#endif

	if !consteval {
		if (can_use_from_chars) {
			std::string stripped;
			std::string_view text = literal;

			if (scanned.has_underscore) {
				stripped = literal;
				std::erase(stripped, '_');

				text = stripped;
			}

			double result = 0.0;
			const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), result, std::chars_format::general);

			if (code == std::errc::result_out_of_range) {
				// A nonzero literal whose decimal point is after its first digit is at least 1, so it overflowed rather than underflowed.
				return scanned.decimal_point > 0 ? float_status::too_large : float_status::too_small;
			}

			if (code == std::errc{} && end == text.data() + text.size() && result != 0.0) {
				output = result;
				return float_status::ok;
			}
		}
	}

	return exact_float(literal, scanned, output);
}

/*
The shortest digits of a finite, positive double that read back as it, choosing the closest and then an even last digit among the shortest, as `digits` (no leading or trailing zeros) and `point`, where the value is 0.digits × 10^point.
*/
struct shortest_digits {
	std::array<char, std::numeric_limits<double>::max_digits10> digits{};
	int count = 0;
	int point = 0;
};

constexpr shortest_digits find_shortest_digits(double magnitude) noexcept {
	shortest_digits result;

	// A whole number below 2^53 is exact, and its neighbors are at most 1 away, so its own digits are the shortest that read back.
	// Note: Not using std::trunc because it is not constexpr in the supported Apple Clang 21 library.
	if (magnitude < 9007199254740992.0 && static_cast<double>(static_cast<std::uint64_t>(magnitude)) == magnitude) {
		const auto [end, code] = std::to_chars(result.digits.data(), result.digits.data() + result.digits.size(), static_cast<std::uint64_t>(magnitude));
		static_cast<void>(code); // The buffer holds every integer below 2^53.
		result.point = static_cast<int>(end - result.digits.data());
		result.count = result.point;
		while (result.count > 1 && result.digits[static_cast<std::size_t>(result.count - 1)] == '0') {
			// Trailing zeros are left out of the digits; the point keeps the magnitude.
			result.count--;
		}

		return result;
	}

	if !consteval {
		// The shortest scientific form, as `d.ddde±x`, which is the closest of the shortest and then rounds to even, as the spec's rule 11 asks.
		std::array<char, 40> buffer;
		const auto [end, code] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), magnitude, std::chars_format::scientific);

		if (code == std::errc{}) {
			const char* exponent_marker = std::find(buffer.data(), end, 'e');
			result.digits[0] = buffer[0];
			const char* fraction = buffer[1] == '.' ? buffer.data() + 2 : exponent_marker;
			std::ranges::copy(fraction, exponent_marker, result.digits.begin() + 1);
			result.count = 1 + static_cast<int>(exponent_marker - fraction);

			int exponent = 0;
			static_cast<void>(std::from_chars(exponent_marker + 2, end, exponent));
			// Scientific to_chars already omits trailing zeros and always writes an exponent sign.
			result.point = 1 + (exponent_marker[1] == '-' ? -exponent : exponent);
			return result;
		}
	}

	decimal digits;
	digits.assign_shortest(std::bit_cast<std::uint64_t>(magnitude));

	std::ranges::transform(digits.digits.begin(), digits.digits.begin() + digits.count, result.digits.begin(), [](std::uint8_t digit) {
		return static_cast<char>('0' + digit);
	});

	result.count = digits.count;
	result.point = digits.point;
	return result;
}

constexpr bool is_nan(double number) noexcept {
	// By its bits, because `number != number` is optimized away under `-ffast-math`.
	const auto bits = std::bit_cast<std::uint64_t>(number);
	return (bits & 0x7FFF'FFFF'FFFF'FFFF) > 0x7FF0'0000'0000'0000;
}

/*
Writes a float in canonical form (rule 11). NaN is the caller's to refuse.
*/
constexpr void append_float(std::string& output, double number) {
	const auto bits = std::bit_cast<std::uint64_t>(number);
	const std::uint64_t magnitude_bits = bits & ~(std::uint64_t{1} << 63U);

	if (magnitude_bits == 0) {
		output += "0.0";
		return;
	}

	// Note: Not using __builtin_signbit because it brought no improvement and has different signed-zero behavior under GCC -ffast-math; reuse the number's existing bit pattern.
	if ((bits >> 63U) != 0) {
		output.push_back('-');
	}

	if (magnitude_bits == 0x7FF0'0000'0000'0000) {
		output += "infinity";
		return;
	}

	const shortest_digits shortest = find_shortest_digits(std::bit_cast<double>(magnitude_bits));
	const int count = shortest.count;
	const int point = shortest.point;
	const std::string_view digits{shortest.digits.data(), static_cast<std::size_t>(count)};

	if (count <= point && point <= 21) {
		output += digits;
		output.append(static_cast<std::size_t>(point - count), '0');
		output += ".0";
	} else if (0 < point && point < count) {
		output += digits.substr(0, static_cast<std::size_t>(point));
		output.push_back('.');
		output += digits.substr(static_cast<std::size_t>(point));
	} else if (-6 < point && point <= 0) {
		output += "0.";
		output.append(static_cast<std::size_t>(-point), '0');
		output += digits;
	} else {
		output.push_back(digits[0]);

		if (count > 1) {
			output.push_back('.');
			output += digits.substr(1);
		}

		output.push_back('e');
		append_integer(output, point - 1);
	}
}

} // namespace soml::detail

namespace soml::detail {

// MARK: Messages

template<class... Parts>
constexpr std::string concat(const Parts&... parts) {
	std::string output;
	output.reserve((std::size_t{} + ... + std::string_view{parts}.size()));
	(output.append(std::string_view{parts}), ...);
	return output;
}

/*
Text quoted in a message, cut to `maximum` characters, so that a huge token does not make a huge message. Invalid UTF-8 bytes count individually.
*/
constexpr std::string abbreviate(std::string_view text, std::size_t maximum = 40) {
	if (text.size() <= maximum) {
		return std::string{text};
	}

	std::size_t count = 0;
	std::size_t index = 0;
	while (index < text.size() && count < maximum) {
		if !consteval {
			while (text.size() - index >= sizeof(std::uint64_t) && maximum - count >= sizeof(std::uint64_t)) {
				std::uint64_t word = 0;
				std::memcpy(&word, text.data() + index, sizeof(word));
				if ((word & 0x8080'8080'8080'8080) != 0) {
					break;
				}
				index += sizeof(word);
				count += sizeof(word);
			}
			if (index == text.size() || count == maximum) {
				break;
			}
		}
		index += std::max<std::size_t>(utf8_sequence_length(text, index), 1);
		count++;
	}

	return index < text.size() ? concat(text.substr(0, index), "…") : std::string{text};
}

constexpr std::string format_code_point(char32_t scalar) {
	// Note: Not using std::format because it lacks constexpr support in the supported Apple Clang 21 and GCC 16 libraries.
	std::string output = "U+";
	append_hex(output, static_cast<std::uint32_t>(scalar), 4, "0123456789ABCDEF");
	return output;
}

/*
The names of characters that are invisible or easy to mistake, so that a message can say what it found.
*/
constexpr std::string_view character_name(char32_t scalar) noexcept {
	if (scalar < 0x85) {
		return {};
	}
	static constexpr std::array<std::pair<char32_t, std::string_view>, 37> names{{
		{0x85, "NEXT LINE"},
		{0xA0, "NO-BREAK SPACE"},
		{0xAD, "SOFT HYPHEN"},
		{0x061C, "ARABIC LETTER MARK"},
		{0x1680, "OGHAM SPACE MARK"},
		{0x2000, "EN QUAD"},
		{0x2001, "EM QUAD"},
		{0x2002, "EN SPACE"},
		{0x2003, "EM SPACE"},
		{0x2004, "THREE-PER-EM SPACE"},
		{0x2005, "FOUR-PER-EM SPACE"},
		{0x2006, "SIX-PER-EM SPACE"},
		{0x2007, "FIGURE SPACE"},
		{0x2008, "PUNCTUATION SPACE"},
		{0x2009, "THIN SPACE"},
		{0x200A, "HAIR SPACE"},
		{0x200B, "ZERO WIDTH SPACE"},
		{0x200C, "ZERO WIDTH NON-JOINER"},
		{0x200D, "ZERO WIDTH JOINER"},
		{0x200E, "LEFT-TO-RIGHT MARK"},
		{0x200F, "RIGHT-TO-LEFT MARK"},
		{0x2028, "LINE SEPARATOR"},
		{0x2029, "PARAGRAPH SEPARATOR"},
		{0x202A, "LEFT-TO-RIGHT EMBEDDING"},
		{0x202B, "RIGHT-TO-LEFT EMBEDDING"},
		{0x202C, "POP DIRECTIONAL FORMATTING"},
		{0x202D, "LEFT-TO-RIGHT OVERRIDE"},
		{0x202E, "RIGHT-TO-LEFT OVERRIDE"},
		{0x202F, "NARROW NO-BREAK SPACE"},
		{0x205F, "MEDIUM MATHEMATICAL SPACE"},
		{0x2060, "WORD JOINER"},
		{0x2066, "LEFT-TO-RIGHT ISOLATE"},
		{0x2067, "RIGHT-TO-LEFT ISOLATE"},
		{0x2068, "FIRST STRONG ISOLATE"},
		{0x2069, "POP DIRECTIONAL ISOLATE"},
		{0x3000, "IDEOGRAPHIC SPACE"},
		{0xFEFF, "ZERO WIDTH NO-BREAK SPACE"},
	}};
	// Note: Not using linear std::ranges::find because it was much slower for late entries and missing character names.
	const auto found = std::ranges::lower_bound(names, scalar, {}, &decltype(names)::value_type::first);
	return found != names.end() && found->first == scalar ? found->second : std::string_view{};
}

/*
A character for a message, such as `“$”` or `U+00A0 (NO-BREAK SPACE; only space, tab, and line feed are whitespace)`.
*/
constexpr std::string describe_character(char32_t scalar) {
	const std::string_view name = character_name(scalar);

	if (!name.empty()) {
		return concat(format_code_point(scalar), " (", name, is_lookalike_space(scalar) ? "; only space, tab, and line feed are whitespace" : "", ")");
	}

	if (scalar != ' ' && is_shown_by_code_point(scalar)) {
		return format_code_point(scalar);
	}

	std::string output = "“";
	append_utf8(output, scalar);
	output += "”";
	return output;
}

/*
Text as a JSON string, as `JSON.stringify` writes it for valid UTF-8. Invalid bytes are escaped individually as `\u00xx`, so a diagnostic is always valid UTF-8.
*/
constexpr std::string json_quote(std::string_view text) {
	std::string output = "\"";
	output.reserve(text.size() + 2);
	std::size_t chunk_start = 0;

	for (std::size_t index = 0; index < text.size(); index++) {
		if !consteval {
			while (text.size() - index >= sizeof(std::uint64_t)) {
				std::uint64_t word = 0;
				std::memcpy(&word, text.data() + index, sizeof(word));
				const std::uint64_t special = (word & 0x8080'8080'8080'8080) | ascii_below_lanes(word, 0x20) | ascii_equal_lanes(word, '"') | ascii_equal_lanes(word, '\\');

				if (special != 0) {
					index += first_marked_byte(special);
					break;
				}

				index += sizeof(word);
			}

			if (index == text.size()) {
				break;
			}
		}

		const char character = text[index];
		const int byte = byte_of(character);

		if (byte >= 0x80) {
			const std::size_t length = utf8_sequence_length(text, index);

			if (length != 0) {
				index += length - 1;
			} else {
				output += text.substr(chunk_start, index - chunk_start);
				chunk_start = index + 1;
				output += "\\u00";
				output.push_back("0123456789abcdef"[byte >> 4]);
				output.push_back("0123456789abcdef"[byte & 0xF]);
			}

			continue;
		}

		if (byte >= 0x20 && character != '"' && character != '\\') {
			continue;
		}
		output += text.substr(chunk_start, index - chunk_start);
		chunk_start = index + 1;
		if (character == '"' || character == '\\') {
			output.push_back('\\');
			output.push_back(character);
		} else if (character == '\n') {
			output += "\\n";
		} else if (character == '\t') {
			output += "\\t";
		} else if (character == '\b') {
			output += "\\b";
		} else if (character == '\f') {
			output += "\\f";
		} else if (character == '\r') {
			output += "\\r";
		} else if (byte < 0x20) {
			output += "\\u00";
			output.push_back("0123456789abcdef"[byte >> 4]);
			output.push_back("0123456789abcdef"[byte & 0xF]);
		}
	}

	output += text.substr(chunk_start);
	output.push_back('"');
	return output;
}

// MARK: Writing strings

enum class string_kind : std::uint8_t {
	// Written as `'...'`.
	literal,
	// Written as `"..."`, because it holds a `'`, a tab, a line feed, another C0 control, or DEL.
	escaped,
	carriage_return,
	invalid_utf8,
};

/*
Finds in one scan whether a string can be written, valid UTF-8 without a carriage return, and which quotes canonical form gives it. Eight ASCII bytes without a `'`, a control character, or DEL are checked at once.
*/
constexpr string_kind classify_string(std::string_view text) noexcept {
	constexpr std::uint64_t high_bits = 0x8080'8080'8080'8080;
	bool needs_escapes = false;
	std::size_t index = 0;

	while (index < text.size()) {
		if !consteval {
			while (text.size() - index >= sizeof(std::uint64_t)) {
				std::uint64_t word = 0;
				std::memcpy(&word, text.data() + index, sizeof(word));

				// Each lane is exact for ASCII bytes, as in the reader's character check.
				const std::uint64_t special = needs_escapes ? ascii_equal_lanes(word, '\r') : ascii_below_lanes(word, 0x20) | ascii_equal_lanes(word, '\'') | ascii_equal_lanes(word, 0x7F);

				if ((word & high_bits) != 0 || special != 0) {
					index += first_marked_byte((word & high_bits) | special);
					break;
				}

				index += 8;
			}

			if (index >= text.size()) {
				break;
			}
		}

		const int byte = byte_of(text[index]);

		if (byte == '\r') {
			return string_kind::carriage_return;
		}

		if (byte < 0x80) {
			needs_escapes = needs_escapes || byte == '\'' || byte < 0x20 || byte == 0x7F;
			index++;
			continue;
		}

		const std::size_t length = utf8_sequence_length(text, index);

		if (length == 0) {
			return string_kind::invalid_utf8;
		}

		index += length;
	}

	return needs_escapes ? string_kind::escaped : string_kind::literal;
}

// An object with at most this many members checks for duplicate keys by comparing each pair, and a larger one by sorting.
inline constexpr std::size_t linear_duplicate_limit = 16;

/*
A fingerprint of a key, so that a check for duplicate keys compares two keys only when their fingerprints are equal: the length, and the first and last bytes.
*/
constexpr std::uint64_t key_fingerprint(std::string_view key) noexcept {
	if (key.empty()) {
		return 0;
	}

	return (static_cast<std::uint64_t>(key.size()) << 16U) | (static_cast<std::uint64_t>(byte_of(key.front())) << 8U) | static_cast<std::uint64_t>(byte_of(key.back()));
}

/*
The index of the first key, in order, that repeats an earlier one, among at most `linear_duplicate_limit` keys, or `npos`.
*/
[[gnu::noinline]] constexpr std::size_t find_duplicate_key(std::span<const member> items) {
	std::array<std::uint64_t, linear_duplicate_limit> fingerprints;

	for (std::size_t later = 0; later < items.size(); later++) {
		fingerprints[later] = key_fingerprint(items[later].key);

		for (std::size_t earlier = 0; earlier < later; earlier++) {
			if (fingerprints[earlier] == fingerprints[later] && items[earlier].key == items[later].key) {
				return later;
			}
		}
	}

	return std::string_view::npos;
}

/*
Writes a string with the canonical quotes and escapes (rules 8 and 9). The string must be valid UTF-8 without a carriage return.
*/
constexpr void append_string(std::string& output, std::string_view text, string_kind kind) {
	if (kind == string_kind::literal) {
		output.push_back('\'');
		output += text;
		output.push_back('\'');
		return;
	}

	output.push_back('"');
	std::size_t chunk_start = 0;

	for (std::size_t index = 0; index < text.size(); index++) {
		if !consteval {
			while (text.size() - index >= sizeof(std::uint64_t)) {
				std::uint64_t word = 0;
				std::memcpy(&word, text.data() + index, sizeof(word));
				// UTF-8 bytes above ASCII never need escaping; mask their lanes out of the ASCII checks.
				const std::uint64_t special = (ascii_below_lanes(word, 0x20) | ascii_equal_lanes(word, '\\') | ascii_equal_lanes(word, '"') | ascii_equal_lanes(word, 0x7F)) & ~word;
				if (special != 0) {
					index += first_marked_byte(special);
					break;
				}
				index += sizeof(word);
			}
			if (index >= text.size()) {
				break;
			}
		}
		const char character = text[index];
		const int byte = byte_of(character);

		if (character != '\\' && character != '"' && byte >= 0x20 && byte != 0x7F) {
			continue;
		}

		output += text.substr(chunk_start, index - chunk_start);
		chunk_start = index + 1;

		if (character == '\\' || character == '"') {
			output.push_back('\\');
			output.push_back(character);
		} else if (character == '\n') {
			output += "\\n";
		} else if (character == '\t') {
			output += "\\t";
		} else {
			output += "\\u{";
			append_hex(output, static_cast<std::uint32_t>(byte), 1, "0123456789abcdef");
			output.push_back('}');
		}
	}

	output += text.substr(chunk_start);
	output.push_back('"');
}

/*
Writes a key bare when it can be, and otherwise as a string.
*/
constexpr void append_string(std::string& output, std::string_view text) {
	append_string(output, text, classify_string(text));
}

constexpr void append_key(std::string& output, std::string_view key) {
	if (soml::is_bare_key(key)) {
		output += key;
	} else {
		append_string(output, key);
	}
}

constexpr std::string format_key(std::string_view key) {
	std::string output;
	append_key(output, key);
	return output;
}

/*
A key as a message shows it: bare when it can be, and otherwise as a JSON string, cut short when it is long.
*/
constexpr std::string describe_key(std::string_view key) {
	return soml::is_bare_key(key) ? abbreviate(key) : json_quote(abbreviate(key));
}

/*
Writes nine digits of a fraction of a second without trailing zeros, after a `.`, or nothing for zero.
*/
constexpr void append_fraction(std::string& output, std::uint64_t nanoseconds) {
	if (nanoseconds == 0) {
		return;
	}

	std::array<char, 9> digits;
	const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), nanoseconds);
	const std::size_t count = static_cast<std::size_t>(converted.ptr - digits.data());
	std::size_t kept = count;
	while (digits[kept - 1] == '0') {
		kept--;
	}
	output.push_back('.');
	output.append(digits.size() - count, '0');
	output.append(digits.data(), kept);
}

constexpr void append_two_digits(std::string& output, std::int64_t number) {
	output.append_range(std::array{static_cast<char>('0' + (number / 10)), static_cast<char>('0' + (number % 10))});
}

/*
Writes a valid calendar date as exactly ten ASCII characters, without an instant conversion.
*/
constexpr void append_date(std::string& output, soml::date date) {
	const int year = static_cast<int>(date.year());
	append_two_digits(output, year / 100);
	append_two_digits(output, year % 100);
	output.push_back('-');
	append_two_digits(output, static_cast<unsigned>(date.month()));
	output.push_back('-');
	append_two_digits(output, static_cast<unsigned>(date.day()));
}

/*
Writes a valid instant in canonical form: UTC, with the fraction left out when it is zero and otherwise without trailing zeros.
*/
constexpr void append_instant(std::string& output, soml::instant instant) {
	std::int64_t days = instant.seconds / 86'400;
	std::int64_t second_of_day = instant.seconds % 86'400;

	if (second_of_day < 0) {
		second_of_day += 86'400;
		days--;
	}

	const std::chrono::year_month_day date{std::chrono::sys_days{std::chrono::days{days}}};
	append_date(output, date);
	output.push_back('T');
	const std::int64_t minute_of_day = second_of_day / 60;
	append_two_digits(output, minute_of_day / 60);
	output.push_back(':');
	append_two_digits(output, minute_of_day % 60);
	output.push_back(':');
	append_two_digits(output, second_of_day % 60);
	append_fraction(output, static_cast<std::uint64_t>(instant.nanoseconds));
	output.push_back('Z');
}

/*
Writes a duration in canonical form (rule 13): hours, minutes, and seconds with a fraction, each left out when it is zero, and `0s` for zero.
*/
constexpr void append_duration(std::string& output, soml::duration length) {
	const std::int64_t nanoseconds = length.count();

	if (nanoseconds == 0) {
		output += "0s";
		return;
	}

	const std::uint64_t total = magnitude(nanoseconds);
	const std::uint64_t hours = total / 3'600'000'000'000;
	const std::uint64_t minutes = total / 60'000'000'000 % 60;
	const std::uint64_t seconds = total / 1'000'000'000 % 60;
	const std::uint64_t fraction = total % 1'000'000'000;

	if (nanoseconds < 0) {
		output.push_back('-');
	}

	if (hours > 0) {
		append_integer(output, hours);
		output.push_back('h');
	}

	if (minutes > 0) {
		append_integer(output, minutes);
		output.push_back('m');
	}

	if (seconds > 0 || fraction > 0) {
		append_integer(output, seconds);
		append_fraction(output, fraction);
		output.push_back('s');
	}
}

// MARK: Positions

struct location {
	std::size_t line;
	std::size_t column;
};

/*
The 1-based line, and the 1-based column in Unicode scalar values, of a byte offset in text whose bytes before the offset are valid UTF-8.
*/
constexpr location locate(std::string_view text, std::size_t offset) noexcept {
	const std::string_view prefix = text.substr(0, std::min(offset, text.size()));
	const std::size_t line_start = prefix.rfind('\n') + 1;
	return {.line = static_cast<std::size_t>(std::ranges::count(prefix, '\n')) + 1, .column = count_scalars(prefix.substr(line_start)) + 1};
}

constexpr error make_error(error_code code, std::string message) {
	return {.code = code, .message = std::move(message), .path = {}};
}

/*
Builds an object from members without the duplicate check of the public API, for the reader, which has checked them already, and for converters, whose source cannot hold duplicates.
*/
struct object_access {
	static constexpr object make(std::vector<member>&& members) {
		object result;
		result.members_ = std::move(members);
		return result;
	}

	static constexpr std::vector<member>& members(object& target) noexcept {
		return target.members_;
	}
};

} // namespace soml::detail

namespace soml::detail {

// MARK: Parser

inline constexpr std::string_view key_quoting_hint = ". A key that contains characters other than letters, digits, “_”, and “-” must be quoted";
inline constexpr std::string_view instant_format = "An instant is written as 2026-09-19T14:00:00Z, with an optional fraction of up to nine digits and an offset of Z or ±HH:MM";

// The longest token or line that a diagnosis looks at. A longer one gets a general message.
inline constexpr std::size_t max_diagnosed_length = 1000;

constexpr bool is_valid_value(std::string_view text);

/*
The duration units in their required order, with their length in nanoseconds.
*/
struct duration_unit {
	std::string_view name;
	std::uint64_t nanoseconds;
	// The power of ten in `nanoseconds`, and what is left: `nanoseconds = multiplier × 10^power`.
	std::uint64_t multiplier;
	int power;

	/*
	Finds a lowercase unit, or returns nullptr for an unknown spelling.
	*/
	static constexpr const duration_unit* find(std::string_view name) noexcept;
};

inline constexpr std::array<duration_unit, 6> duration_units{{
	{.name = "h", .nanoseconds = 3'600'000'000'000, .multiplier = 36, .power = 11},
	{.name = "m", .nanoseconds = 60'000'000'000, .multiplier = 6, .power = 10},
	{.name = "s", .nanoseconds = 1'000'000'000, .multiplier = 1, .power = 9},
	{.name = "ms", .nanoseconds = 1'000'000, .multiplier = 1, .power = 6},
	{.name = "us", .nanoseconds = 1000, .multiplier = 1, .power = 3},
	{.name = "ns", .nanoseconds = 1, .multiplier = 1, .power = 0},
}};

constexpr const duration_unit* duration_unit::find(std::string_view name) noexcept {
	if (name.empty() || name.size() > 2 || (name.size() == 2 && name[1] != 's')) {
		return nullptr;
	}

	// The table has the one-letter units h/m/s, then the two-letter units ms/us/ns.
	const std::string_view initials = name.size() == 1 ? "hms" : "mun";
	const std::size_t index = initials.find(name[0]);
	return index == std::string_view::npos ? nullptr : &duration_units[index + (name.size() == 1 ? 0U : 3U)];
}

/*
Powers needed when scaling a duration's fractional digits to nanoseconds.
*/
inline constexpr auto duration_powers = [] {
	std::array<std::uint64_t, 14> powers{};
	powers[0] = 1;
	// Note: Not using std::ranges::generate because it needed a mutable stateful lambda for this compile-time recurrence without adding a runtime benefit.
	for (std::size_t index = 1; index < powers.size(); index++) {
		powers[index] = powers[index - 1] * 10;
	}
	return powers;
}();

constexpr bool equals_ignoring_case(std::string_view text, std::string_view lowercase_word) noexcept {
	if (text.size() != lowercase_word.size()) {
		return false;
	}

	for (std::size_t index = 0; index < text.size(); index++) {
		if ((byte_of(text[index]) | 0x20) != byte_of(lowercase_word[index])) {
			return false;
		}
	}

	return true;
}

constexpr bool has_digits(std::string_view text, std::size_t start, std::size_t count) noexcept {
	if (start > text.size() || count > text.size() - start) {
		return false;
	}

	for (std::size_t index = start; index < start + count; index++) {
		if (!is_digit(text[index])) {
			return false;
		}
	}

	return true;
}

constexpr std::int64_t read_digits(std::string_view text, std::size_t start, std::size_t count) noexcept {
	// Note: Not using std::from_chars because it repeated validation already done by the grammar and slowed instant parsing on both compilers.
	std::int64_t number = 0;

	for (std::size_t index = start; index < start + count; index++) {
		number = (number * 10) + (text[index] - '0');
	}

	return number;
}

/*
Reads a grammar-checked unsigned magnitude, allowing underscores between digits. The ordinary digit run is parsed in one call, then continued past any underscores from its first unconsumed byte.
*/
constexpr bool read_integer_magnitude(std::string_view digits, int radix, std::uint64_t& output) noexcept {
	const char* const end = digits.data() + digits.size();
	const auto converted = std::from_chars(digits.data(), end, output, radix);

	if (!converted) {
		return false;
	}

	for (const char* digit = converted.ptr; digit < end; digit++) {
		if (*digit != '_' && !multiply_add(output, output, static_cast<std::uint64_t>(radix), static_cast<std::uint64_t>(hex_digit_value(*digit)))) {
			return false;
		}
	}

	return true;
}

/*
The index after a run of digits of the radix with single underscores between them, or `index` when no digit is there.
*/
constexpr std::size_t skip_digits(std::string_view text, std::size_t index, int radix) noexcept {
	if (index >= text.size() || !is_radix_digit(text[index], radix)) {
		return index;
	}

	index++;

	for (;;) {
		if (index < text.size() && is_radix_digit(text[index], radix)) {
			index++;
		} else if (index + 1 < text.size() && text[index] == '_' && is_radix_digit(text[index + 1], radix)) {
			index += 2;
		} else {
			return index;
		}
	}
}

/*
The index after an integer part, which is `0` or digits without a leading zero, or `index` when there is none.
*/
constexpr std::size_t skip_integer_part(std::string_view text, std::size_t index) noexcept {
	if (index < text.size() && text[index] == '0') {
		return index + 1;
	}

	return skip_digits(text, index, 10);
}

/*
The index after a run of digits and underscores in any order, for describing a number that does not follow the grammar.
*/
constexpr std::size_t skip_digits_and_underscores(std::string_view text, std::size_t index) noexcept {
	while (index < text.size() && (is_digit(text[index]) || text[index] == '_')) {
		index++;
	}

	return index;
}

enum class number_kind : std::uint8_t {
	none,
	decimal,
	radix,
	floating,
};

/*
Whether a token is an int, in any radix, or a float, by the grammar.
*/
constexpr number_kind classify_number(std::string_view text) noexcept {
	const int radix = text.size() >= 2 && text[0] == '0' ? radix_of(text[1]) : 0;

	if (radix != 0) {
		const std::size_t end = skip_digits(text, 2, radix);
		return end > 2 && end == text.size() ? number_kind::radix : number_kind::none;
	}

	std::size_t index = !text.empty() && text[0] == '-' ? 1 : 0;
	const std::size_t integer_end = skip_integer_part(text, index);

	if (integer_end == index) {
		return number_kind::none;
	}

	index = integer_end;
	number_kind kind = number_kind::decimal;

	if (index < text.size() && text[index] == '.') {
		const std::size_t end = skip_digits(text, index + 1, 10);

		if (end == index + 1) {
			return number_kind::none;
		}

		index = end;
		kind = number_kind::floating;
	}

	if (index < text.size() && text[index] == 'e') {
		const bool is_negative = index + 1 < text.size() && text[index + 1] == '-';
		const std::size_t exponent_start = index + (is_negative ? 2 : 1);
		// The exponent follows the integer part's rule for leading zeros, and its zero is only `e0`.
		const std::size_t end = is_negative && exponent_start < text.size() && text[exponent_start] == '0' ? exponent_start : skip_integer_part(text, exponent_start);

		if (end == exponent_start) {
			return number_kind::none;
		}

		index = end;
		kind = number_kind::floating;
	}

	return index == text.size() ? kind : number_kind::none;
}

/*
Whether a token that is not a number, a date, or an instant was meant as a duration: its first letter after the digits could begin a unit, a day, or a week.
*/
constexpr bool is_duration_like(std::string_view text) noexcept {
	std::size_t index = !text.empty() && text[0] == '-' ? 1 : 0;

	if (index >= text.size() || !is_digit(text[index])) {
		return false;
	}

	index = skip_digits_and_underscores(text, index);

	if (index < text.size() && text[index] == '.') {
		index = skip_digits_and_underscores(text, index + 1);
	}

	if (index >= text.size()) {
		return false;
	}

	switch (byte_of(text[index]) | 0x20) {
		case 'y': {
			// A year unit, as in `1y` or `2years`, is only one when the token or another part follows it, so that a word such as `100yen` stays a string to quote.
			for (const std::string_view unit : {"years", "year", "yrs", "yr", "y"}) {
				if (equals_ignoring_case(text.substr(index, unit.size()), unit) && (index + unit.size() == text.size() || is_digit(text[index + unit.size()]))) {
					return true;
				}
			}

			return false;
		}

		case 'd':
		case 'h':
		case 'm':
		case 'n':
		case 's':
		case 'u':
		case 'w': {
			return true;
		}

		default: {
			return false;
		}
	}
}

/*
Whether the text starts with a `YYYY-MM-DD` date, before checking whether that date exists.
*/
constexpr bool has_date(std::string_view text) noexcept {
	return text.size() >= 10 && text[4] == '-' && text[7] == '-' && has_digits(text, 0, 4) && has_digits(text, 5, 2) && has_digits(text, 8, 2);
}

/*
Whether the text, which starts with a date, continues with `THH:MM:SS`.
*/
constexpr bool has_time(std::string_view text) noexcept {
	return text.size() >= 19 && text[10] == 'T' && text[13] == ':' && text[16] == ':' && has_digits(text, 11, 2) && has_digits(text, 14, 2) && has_digits(text, 17, 2);
}

/*
Whether a time, as `H:MM` or `HH:MM`, follows `index` after spaces or tabs, as in a date and a time that TOML and Python write with a space between them.
*/
constexpr bool has_time_after_space(std::string_view text, std::size_t index) noexcept {
	const std::size_t start = skip_spaces(text, index);

	if (start == index || start >= text.size() || !is_digit(text[start])) {
		return false;
	}

	const std::size_t hour_length = start + 1 < text.size() && is_digit(text[start + 1]) ? 2 : 1;
	return text.size() - start >= hour_length + 3 && text[start + hour_length] == ':' && has_digits(text, start + hour_length + 1, 2);
}

/*
Whether the text, whose date prefix was checked already, continues with `THH:MM:SS(.fraction)?(Z|±HH:MM)`.
*/
constexpr bool has_instant_time_and_offset(std::string_view text) noexcept {
	if (!has_time(text)) {
		return false;
	}

	std::size_t index = 19;

	if (index < text.size() && text[index] == '.') {
		const std::size_t fraction_start = ++index;

		while (index < text.size() && is_digit(text[index])) {
			index++;
		}

		if (index == fraction_start) {
			return false;
		}
	}

	const bool has_utc_offset = index + 1 == text.size() && text[index] == 'Z';
	const bool has_numeric_offset = index + 6 == text.size() && (text[index] == '+' || text[index] == '-') && has_digits(text, index + 1, 2) && text[index + 3] == ':' && has_digits(text, index + 4, 2);
	return has_utc_offset || has_numeric_offset;
}

/*
Whether the text is a date and a time without an offset: `YYYY-MM-DDTHH:MM:SS(.fraction)?`.
*/
constexpr bool is_local_date_time(std::string_view text) noexcept {
	return has_date(text) && has_time(text) && (text.size() == 19 || (text.size() > 20 && text[19] == '.' && has_digits(text, 20, text.size() - 20)));
}

/*
Reads a date prefix, with every component range-checked. Returns the reason for the first check that fails, or an empty string. On a day failure, `last_day` is the last day of that month.
*/
constexpr std::string_view read_date(std::string_view text, soml::date& output, std::int64_t& last_day) noexcept {
	const std::int64_t year = read_digits(text, 0, 4);
	const std::int64_t month = read_digits(text, 5, 2);
	const std::int64_t day = read_digits(text, 8, 2);

	if (year == 0) {
		return "the year must be 0001 to 9999";
	}

	if (month < 1 || month > 12) {
		return "the month must be 01 to 12";
	}

	last_day = days_in_month(year, month);

	if (day < 1 || day > last_day) {
		return "day";
	}

	output = soml::date{std::chrono::year{static_cast<int>(year)}, std::chrono::month{static_cast<unsigned>(month)}, std::chrono::day{static_cast<unsigned>(day)}};
	return {};
}

/*
Reads an instant in the shape of one, with every component range-checked, the date checked to exist, and the instant checked to be in range in UTC. Returns the reason for the first check that fails, or an empty string.
*/
constexpr std::string_view read_instant(std::string_view text, soml::instant& output, std::int64_t& last_day) noexcept {
	constexpr std::size_t fraction_start = 20;
	std::size_t offset_start = 19;

	if (text[19] == '.') {
		offset_start = fraction_start;

		while (is_digit(text[offset_start])) {
			offset_start++;
		}
	}

	const std::size_t fraction_length = offset_start > fraction_start ? offset_start - fraction_start : 0;

	if (fraction_length > 9) {
		return "a fractional second has at most nine digits";
	}

	soml::date calendar;

	if (const std::string_view reason = read_date(text, calendar, last_day); !reason.empty()) {
		return reason;
	}

	const std::int64_t hour = read_digits(text, 11, 2);
	const std::int64_t minute = read_digits(text, 14, 2);
	const std::int64_t second = read_digits(text, 17, 2);

	if (hour > 23) {
		return "the hour must be 00 to 23";
	}

	if (minute > 59) {
		return "the minute must be 00 to 59";
	}

	if (second > 59) {
		return "the second must be 00 to 59, and a leap second is not representable";
	}

	std::int64_t offset_seconds = 0;

	if (text[offset_start] != 'Z') {
		const std::int64_t offset_hour = read_digits(text, offset_start + 1, 2);
		const std::int64_t offset_minute = read_digits(text, offset_start + 4, 2);

		if (offset_hour > 23) {
			return "the offset hour must be 00 to 23";
		}

		if (offset_minute > 59) {
			return "the offset minute must be 00 to 59";
		}

		const bool is_negative = text[offset_start] == '-';

		if (is_negative && offset_hour == 0 && offset_minute == 0) {
			return "-00:00 means “offset unknown” in RFC 3339, which is not representable; use Z or +00:00";
		}

		offset_seconds = ((offset_hour * 3600) + (offset_minute * 60)) * (is_negative ? -1 : 1);
	}

	const std::int64_t seconds = (days_from_civil(calendar) * 86'400) + (hour * 3600) + (minute * 60) + second - offset_seconds;

	if (seconds < soml::instant::min().seconds || seconds > soml::instant::max().seconds) {
		return "in UTC it falls outside the years 0001 to 9999";
	}

	const auto nanoseconds = static_cast<std::int32_t>(read_digits(text, fraction_start, fraction_length) * static_cast<std::int64_t>(duration_powers[9 - fraction_length]));

	output = {seconds, nanoseconds};
	return {};
}

constexpr std::string describe_unit(std::string_view unit, std::string_view text) {
	if (unit.empty()) {
		return "every number needs a unit: h, m, s, ms, us, or ns";
	}

	if (unit == "d" || unit == "day" || unit == "days") {
		return "there is no day unit, because a day is not a fixed length. Write 24h for a fixed 24 hours";
	}

	if (unit == "w" || unit == "week" || unit == "weeks") {
		return "there is no week unit, because a day is not a fixed length. Write 168h for a fixed 168 hours";
	}

	for (const std::string_view year : {"y", "yr", "yrs", "year", "years"}) {
		if (equals_ignoring_case(unit, year)) {
			return "there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days";
		}
	}

	if (unit == "M") {
		// A number with `M` alone is more often a size, as in `memory: 512M`, and `512m` would read as minutes.
		const bool is_size = text.size() <= max_diagnosed_length && is_digit(text[0]) && skip_digits_and_underscores(text, 0) + 1 == text.size();
		return concat("there is no month unit, because a month is not a fixed length", is_size ? concat(". A size, such as 512M, is a string: '", abbreviate(text), "'") : "");
	}

	for (const duration_unit& candidate : duration_units) {
		if (equals_ignoring_case(unit, candidate.name)) {
			return concat("the units are lowercase: ", candidate.name);
		}
	}

	return concat("“", abbreviate(unit, 10), "” is not a unit. The units are h, m, s, ms, us, and ns, and a string must be quoted");
}

/*
The index where the digits that end before `end` start.
*/
constexpr std::size_t digits_start(std::string_view text, std::size_t end) noexcept {
	while (end > 0 && is_digit(text[end - 1])) {
		end--;
	}

	return end;
}

/*
The example in a message that a string must be quoted, which quotes `text` as a `'...'` string. That cannot hold a `'`, so then there is no example.
*/
constexpr std::string quoting_example(std::string_view text) {
	return text.contains('\'') ? std::string{} : concat(", as in '", abbreviate(text), "'");
}

/*
A message for the word where a value should be, an unquoted string or a misspelled keyword. `text` is the unquoted string that `word` begins. A keyword hint is only for the word on its own: in `Yes please` or `Nan Goldin`, following it would leave text behind or change the value.
*/
constexpr std::string describe_unknown_word(std::string_view word, std::string_view text) {
	if (text == word) {
		for (const std::string_view keyword : {"true", "false", "yes", "no", "on", "off"}) {
			if (equals_ignoring_case(word, keyword)) {
				return concat("“", word, "” is not a value. Booleans are written true and false, in lowercase");
			}
		}

		for (const std::string_view keyword : {"null", "nil", "none", "undefined"}) {
			if (equals_ignoring_case(word, keyword)) {
				return concat("“", word, "” is not a value. Null is written null, in lowercase");
			}
		}

		if (equals_ignoring_case(word, "nan")) {
			return "NaN is not representable. Use null for a missing value";
		}

		if (equals_ignoring_case(word, "inf") || equals_ignoring_case(word, "infinity")) {
			return concat("“", word, "” is not a value. Infinity is written infinity, in lowercase");
		}
	}

	return concat("Unexpected “", abbreviate(word), "”. A string value must be quoted", quoting_example(text));
}

/*
A message for a token that is not a number, a date, an instant, or a duration, following the reference implementation's checks in order. `unquoted` is the unquoted text that the token begins.
*/
constexpr std::string describe_bad_number(std::string_view full_text, std::string_view unquoted) {
	const std::string_view text = full_text.substr(0, max_diagnosed_length);
	const std::size_t sign = text[0] == '-' ? 1 : 0;

	if (text.contains('+')) {
		return "A “+” sign is not allowed in a number, including in an exponent";
	}

	if (sign + 1 < text.size() && text[sign] == '0' && (text[sign + 1] == 'B' || text[sign + 1] == 'O' || text[sign + 1] == 'X')) {
		return "A number prefix is lowercase: 0x, 0o, or 0b";
	}

	const int radix = sign + 1 < full_text.size() && full_text[sign] == '0' ? radix_of(full_text[sign + 1]) : 0;

	if (radix != 0) {
		const std::string_view name = radix == 16 ? "hexadecimal" : (radix == 8 ? "octal" : "binary");
		const std::string_view digits = full_text.substr(sign + 2);

		if (sign == 1) {
			return concat(radix == 8 ? "An " : "A ", name, " integer cannot have a sign, because it states a bit pattern rather than a quantity");
		}

		if (digits.empty()) {
			return concat("Expected ", name, " digits after “0", full_text.substr(1, 1), "”");
		}

		if (radix == 16 && std::ranges::any_of(digits, [](char character) {
			return character >= 'a' && character <= 'f';
		}) && std::ranges::all_of(digits, [](char character) {
			return is_hex_digit(character) || character == '_';
		})) {
			// The uppercase spelling is only suggested when it is valid, so a misplaced underscore is reported first, and a value outside the 64-bit range gets no example.
			if (digits.front() == '_' || digits.back() == '_' || digits.contains("__")) {
				return "An underscore in a number must be between two digits";
			}

			if (full_text.size() > max_diagnosed_length) {
				return "Hexadecimal digits are uppercase";
			}

			std::string uppercase{full_text};
			std::ranges::transform(uppercase, uppercase.begin(), [](char character) {
				return character >= 'a' && character <= 'f' ? static_cast<char>(character - 'a' + 'A') : character;
			});

			return is_valid_value(uppercase) ? concat("Hexadecimal digits are uppercase: ", abbreviate(uppercase)) : std::string{"Hexadecimal digits are uppercase"};
		}

		// A lowercase hexadecimal digit is a digit in the wrong case, so the one named is a character that is no digit in either case.
		for (std::size_t index = 0; index < digits.size(); index++) {
			const char character = digits[index];
			const bool is_valid = character == '_' || (radix == 16 ? is_hex_digit(character) : is_radix_digit(character, radix));

			if (!is_valid) {
				const std::size_t length = utf8_sequence_length(digits, index);
				return concat("Invalid ", name, " digit “", digits.substr(index, length == 0 ? 1 : length), "”");
			}
		}

		return "An underscore in a number must be between two digits";
	}

	// The shape `-?digits(.digits)?` followed by what comes next, where digits may hold underscores.
	const std::size_t position = skip_digits_and_underscores(text, sign);
	const bool has_integer_digit = position > sign && is_digit(text[sign]);
	std::size_t mantissa_end = position;
	bool has_fraction = false;

	if (position < text.size() && text[position] == '.') {
		const std::size_t fraction_end = skip_digits_and_underscores(text, position + 1);
		has_fraction = fraction_end > position + 1;

		if (has_fraction) {
			mantissa_end = fraction_end;
		}
	}

	// Only when the “E” is where an exponent marker goes, with digits, a `-`, or nothing after it. In `5EUR` or `10EB`, it is part of a word, so lowercasing it would not help.
	if (has_integer_digit && mantissa_end < text.size() && text[mantissa_end] == 'E') {
		const std::size_t exponent = mantissa_end + 1 < text.size() && text[mantissa_end + 1] == '-' ? mantissa_end + 2 : mantissa_end + 1;

		if (skip_digits_and_underscores(text, exponent) == text.size()) {
			return "An exponent marker is a lowercase “e”";
		}
	}

	if (position > sign && position < text.size() && text[position] == '.' && !has_fraction) {
		return "A decimal point must be followed by a digit";
	}

	if (text.starts_with("-.")) {
		return "A number cannot begin with “.”; write a digit before it, as in -0.5";
	}

	// Groups of digits separated by colons, as in a time of day, such as `12:30`.
	{
		std::size_t group_count = 0;
		bool is_time = true;
		bool is_colon_separated = true;
		std::size_t group_start = 0;

		for (;;) {
			std::size_t group_end = group_start;

			while (group_end < text.size() && is_digit(text[group_end])) {
				group_end++;
			}

			const std::size_t group_length = group_end - group_start;

			if (group_length == 0) {
				is_colon_separated = false;
				break;
			}

			is_time = is_time && group_count < 3 && (group_count == 0 ? group_length <= 2 : group_length == 2);
			group_count++;

			if (group_end == text.size()) {
				break;
			}

			if (text[group_end] != ':') {
				is_colon_separated = false;
				break;
			}

			group_start = group_end + 1;
		}

		if (is_colon_separated && group_count >= 2) {
			return is_time ? std::string{"A time of day is a string, so it must be quoted"} : concat("Invalid number “", abbreviate(text), "”. A value that contains “:” must be quoted, as in '", abbreviate(text), "'");
		}
	}

	// A value with a `:` that starts with a zero, such as the MAC address `00:1A:2B`, is not a number with a leading zero, because removing the zero would not make it valid.
	if (sign + 1 < text.size() && text[sign] == '0' && (is_digit(text[sign + 1]) || text[sign + 1] == '_') && !text.contains(':')) {
		// Removing the zero gives a valid number with a different meaning, so the message says what the zero usually meant.
		const std::string identifier = concat("an identifier, such as a ZIP code, as a string", unquoted.contains('\'') ? std::string{} : concat(": '", abbreviate(unquoted), "'"));
		const bool is_octal = sign == 0 && full_text.size() <= max_diagnosed_length && std::ranges::all_of(full_text, [](char character) {
			return character >= '0' && character <= '7';
		});

		if (is_octal && unquoted == full_text) {
			const std::size_t significant = full_text.find_first_not_of('0');
			const std::string octal = concat("0o", significant == std::string_view::npos ? std::string_view{"0"} : full_text.substr(significant));

			if (is_valid_value(octal)) {
				return concat("Leading zeros are not allowed in a decimal number. Write an octal number, such as a file mode, as ", abbreviate(octal), ", and ", identifier);
			}
		}

		const bool is_digits = sign == 0 && std::ranges::all_of(text, [](char character) {
			return is_digit(character);
		});

		return is_digits ? concat("Leading zeros are not allowed in a decimal number. Write ", identifier) : std::string{"Leading zeros are not allowed in a decimal number"};
	}

	// Only in a number: an underscore next to a letter other than the exponent marker is part of a word, such as `4k_video`.
	const bool is_number_shaped = std::ranges::all_of(text, [](char character) {
		return is_digit(character) || character == '-' || character == '.' || character == '_' || character == 'e';
	});

	for (std::size_t index = 0; is_number_shaped && index < text.size(); index++) {
		if (text[index] == '_' && (index == 0 || index + 1 == text.size() || !is_digit(text[index - 1]) || !is_digit(text[index + 1]))) {
			return "An underscore in a number must be between two digits";
		}
	}

	if (has_integer_digit && mantissa_end < text.size() && text[mantissa_end] == 'e') {
		std::size_t exponent = mantissa_end + 1;
		const bool is_negative = exponent < text.size() && text[exponent] == '-';

		if (is_negative) {
			exponent++;
		}

		if (exponent < text.size() && text[exponent] == '0') {
			if (exponent + 1 < text.size() && (is_digit(text[exponent + 1]) || (text[exponent + 1] == '_' && exponent + 2 < text.size() && is_digit(text[exponent + 2])))) {
				return "Leading zeros are not allowed in an exponent";
			}

			if (is_negative && exponent + 1 == text.size()) {
				return "“e-0” is not allowed, because an exponent of zero has one spelling: e0";
			}
		}
	}

	// Only a number ends with its exponent marker. A word that ends with an “e”, such as `1byte` or `-verbose`, has no exponent.
	if (has_integer_digit && mantissa_end < text.size() && text[mantissa_end] == 'e' && (mantissa_end + 1 == text.size() || (mantissa_end + 2 == text.size() && text[mantissa_end + 1] == '-'))) {
		return "Expected digits after the exponent marker “e”";
	}

	if (const std::size_t dot = text.find('.'); dot != std::string_view::npos && text.find('.', dot + 1) != std::string_view::npos) {
		return concat("Invalid number “", abbreviate(text), "”. A value with several dots, such as a version number, must be quoted");
	}

	if (text[0] == '-' && (text.size() == 1 || !is_digit(text[1]))) {
		// The whole word, as for one without the “-”, so that a word such as `-nano` or `-info` is not taken for NaN or infinity.
		if (equals_ignoring_case(text.substr(1), "nan")) {
			return "NaN is not representable. Use null for a missing value";
		}

		if (equals_ignoring_case(text.substr(1), "inf") || equals_ignoring_case(text.substr(1), "infinity")) {
			return concat("“", abbreviate(text), "” is not a value. Negative infinity is written -infinity");
		}

		return "Expected a digit or “infinity” after “-”";
	}

	if (std::ranges::any_of(text, [](char character) {
		return is_letter(character);
	})) {
		return concat("Invalid number “", abbreviate(text), "”. A string value must be quoted", quoting_example(unquoted));
	}

	return concat("Invalid number “", abbreviate(text), "”");
}

/*
Skips ASCII bare-key characters in a borrowed source string, with bounded word reads at runtime.
*/
constexpr std::size_t skip_bare_key(std::string_view text, std::size_t index) noexcept {
	if !consteval {
		while (text.size() - index >= sizeof(std::uint64_t)) {
			std::uint64_t word = 0;
			std::memcpy(&word, text.data() + index, sizeof(word));
			const std::uint64_t lowercase = word | 0x2020'2020'2020'2020;
			const std::uint64_t valid = (ascii_below_lanes(word, '9' + 1) & ~ascii_below_lanes(word, '0')) | (ascii_below_lanes(lowercase, 'z' + 1) & ~ascii_below_lanes(lowercase, 'a')) | ascii_equal_lanes(word, '_') | ascii_equal_lanes(word, '-');

			const std::uint64_t invalid = (valid & ~word) ^ 0x8080'8080'8080'8080;

			if (invalid != 0) {
				return index + first_marked_byte(invalid);
			}

			index += sizeof(word);
		}
	}

	while (index < text.size() && is_bare_key_character(text[index])) {
		index++;
	}

	return index;
}

/*
Skips plain characters in a single-line string whose UTF-8 was checked before parsing. An escaped string also stops at a backslash. At runtime, eight bytes without a delimiter are skipped at once.
*/
constexpr std::size_t skip_string_characters(std::string_view text, std::size_t index, char quote) noexcept {
	if !consteval {
		while (text.size() - index >= sizeof(std::uint64_t)) {
			std::uint64_t word = 0;
			std::memcpy(&word, text.data() + index, sizeof(word));
			const std::uint64_t special = (ascii_equal_lanes(word, static_cast<std::uint64_t>(quote)) | ascii_equal_lanes(word, '\n') | (quote == '"' ? ascii_equal_lanes(word, '\\') : 0)) & ~word & 0x8080'8080'8080'8080;

			if (special != 0) {
				return index + first_marked_byte(special);
			}

			index += sizeof(word);
		}
	}

	while (index < text.size() && text[index] != quote && text[index] != '\n' && (quote != '"' || text[index] != '\\')) {
		index++;
	}

	return index;
}

/*
Reads a document. The text is checked first for characters that are errors anywhere, then parsed by recursive descent. The structure follows the reference implementation, so every error is reported at the same position.

Containers are parsed onto stacks shared by all open containers. Nested containers move into a vector of the exact size when they close; root containers keep the stack storage.
*/
class parser {
public:
	constexpr explicit parser(std::string_view text) noexcept :
		text_(text) {}

	constexpr std::expected<value, error> parse_document() {
		value root;

		if (!parse_document_into(root)) {
			const location position = locate(text_, error_.offset);
			// Note: Not using direct std::unexpect construction because it lost named error fields without a consistent performance gain.
			return std::unexpected(error{.code = error_.code, .message = std::move(error_.message), .offset = error_.offset, .line = position.line, .column = position.column, .path = {}});
		}

		return root;
	}

	/*
	Parses one value that fills the text, for checking that a suggestion in a message is valid.
	*/
	constexpr bool is_one_value() {
		if (!check_characters()) {
			return false;
		}

		value item;
		return parse_value(1, item) && position_ == text_.size();
	}

private:
	std::string_view text_;
	std::size_t position_ = 0;
	// Where the document's collection starts, after the comments and whitespace before it.
	std::size_t document_start_ = 0;

	struct error_state {
		soml::error_code code = soml::error_code::syntax;
		std::string message;
		std::size_t offset = 0;
	};

	error_state error_;

	// Stacks shared by all open containers. A container's part starts where the stack was when it opened, and is removed when it closes.
	std::vector<value> items_;
	std::vector<member> members_;
	// Where the key of each member in `members_` starts, which places a duplicate key error.
	std::vector<std::size_t> key_offsets_;
	// Scratch for sorting the keys of a large object.
	std::vector<std::size_t> order_;

	constexpr bool fail(std::size_t offset, std::string message) {
		error_ = {.code = soml::error_code::syntax, .message = std::move(message), .offset = offset};
		return false;
	}

	/*
	The byte at `offset` from the current position, or -1 past the end.
	*/
	constexpr int peek(std::size_t offset = 0) const noexcept {
		return byte_at(position_ + offset);
	}

	constexpr int byte_at(std::size_t index) const noexcept {
		return index < text_.size() ? byte_of(text_[index]) : -1;
	}

	constexpr bool is_at_end() const noexcept {
		return position_ >= text_.size();
	}

	/*
	Whether a value ends at `offset`, so that a suggestion that ends there leaves nothing out. A `/` only starts a comment with a `*` after it, or a `//` comment of another language, which the error for what follows names, so in `km/h` the text goes on.
	*/
	constexpr bool is_value_end_at(std::size_t offset) const noexcept {
		const int character = byte_at(offset);
		const int next = byte_at(offset + 1);
		return (character == -1 || is_value_end(character)) && (character != '/' || next == '*' || next == '/');
	}

	/*
	The scalar value of the character at `offset`, which is in the text. The text is valid UTF-8, which was checked first.
	*/
	constexpr char32_t scalar_at(std::size_t offset) const noexcept {
		return decode_utf8(text_, offset);
	}

	constexpr std::size_t skip_blanks_back(std::size_t index) const noexcept {
		while (index > 0 && is_space(text_[index - 1])) {
			index--;
		}

		return index;
	}

	constexpr std::size_t find_line_end(std::size_t offset) const noexcept {
		const std::size_t end = text_.find('\n', std::min(offset, text_.size()));
		return end == std::string_view::npos ? text_.size() : end;
	}

	constexpr std::size_t line_start_of(std::size_t offset) const noexcept {
		return text_.substr(0, offset).rfind('\n') + 1;
	}

	/*
	What is at the current position, for a message: the end of the document, a line break, or a character.
	*/
	constexpr std::string describe_here() const {
		if (is_at_end()) {
			return "the end of the document";
		}

		return peek() == '\n' ? std::string{"a line break"} : describe_character(scalar_at(position_));
	}

	/*
	The hint for a `//` comment, as in JavaScript.
	*/
	constexpr std::string_view slash_comment_hint() const noexcept {
		return peek() == '/' && peek(1) == '/' ? ". A comment starts with “#”" : "";
	}

	/*
	The text from `start` that was most likely meant as one unquoted string, such as `John Smith`: up to the end of the line, a comma, a closing bracket, or a comment after a space or a tab, without the spaces and tabs at its end.
	*/
	constexpr std::string_view unquoted_text(std::size_t start) const noexcept {
		const std::size_t limit = std::min(text_.size(), start + max_diagnosed_length);
		std::size_t end = start;

		for (; end < limit; end++) {
			const char character = text_[end];
			const bool is_comment = character == '#' || (character == '/' && byte_at(end + 1) == '*');

			if (character == '\n' || character == ',' || character == ']' || character == '}' || (is_comment && end > 0 && is_space(text_[end - 1]))) {
				break;
			}
		}

		return text_.substr(start, skip_blanks_back(end) - start);
	}

	/*
	The byte length of a visible character that can begin a key needing quotes, or zero when it has meaning in the grammar. A `=` is left out, because after a key, as in `name=foo`, it was most likely meant as the `:` of INI and TOML.
	*/
	constexpr std::size_t quotable_key_character_length(std::size_t offset) const noexcept {
		if (offset >= text_.size()) {
			return 0;
		}

		const std::size_t length = utf8_sequence_length(text_, offset);
		const char32_t scalar = decode_utf8(text_, offset, length);

		if (scalar < 0x80) {
			return scalar > ' ' && scalar < 0x7F && !std::string_view{"\"#'*,/:=[]{}"}.contains(static_cast<char>(scalar)) ? length : 0;
		}

		return is_shown_by_code_point(scalar) ? 0 : length;
	}

	/*
	The hint for a key that starts with a character a bare key cannot hold, such as the `$` in `$schema`, with the key quoted when a `:` ends it.
	*/
	constexpr std::string key_quoting_hint_here() const {
		const std::size_t limit = std::min(text_.size(), position_ + max_diagnosed_length);
		std::size_t end = position_;

		while (end < limit) {
			const std::size_t length = quotable_key_character_length(end);

			if (length == 0) {
				break;
			}

			end += length;
		}

		if (end == position_) {
			return {};
		}

		if (end < text_.size() && text_[end] == ':' && end <= limit) {
			return concat(key_quoting_hint, ", as in '", abbreviate(text_.substr(position_, end - position_)), "'");
		}

		return std::string{key_quoting_hint};
	}

	// MARK: Characters and trivia

	/*
	Characters that are errors wherever they appear are checked once, before parsing: invalid UTF-8 first, then a BOM, then the first raw control character, as in the reference implementation. Eight ASCII bytes without a control character are skipped at once.
	*/
	constexpr bool check_characters() {
		constexpr std::uint64_t high_bits = 0x8080'8080'8080'8080;
		const std::size_t length = text_.size();
		std::size_t first_control = length;
		std::size_t index = 0;

		while (index < length) {
			if !consteval {
				while (length - index >= sizeof(std::uint64_t)) {
					std::uint64_t word = 0;
					std::memcpy(&word, text_.data() + index, sizeof(word));

					if ((word & high_bits) != 0) {
						break;
					}

					if (first_control == length) {
						// For ASCII bytes, each lane is exact: no lane borrows from the next, because every lane has its high bit set before the subtraction.
						const std::uint64_t controls = (ascii_below_lanes(word, 0x20) & ~ascii_equal_lanes(word, '\t') & ~ascii_equal_lanes(word, '\n')) | ascii_equal_lanes(word, 0x7F);

						if (controls != 0) {
							first_control = index + first_marked_byte(controls);
						}
					}

					index += 8;
				}

				if (index >= length) {
					break;
				}
			}

			const int byte = byte_of(text_[index]);

			if (byte < 0x80) {
				if (first_control == length && is_control(byte)) {
					first_control = index;
				}

				index++;
				continue;
			}

			bool is_truncated = false;
			const std::size_t sequence_length = utf8_sequence_length(text_, index, &is_truncated);

			if (sequence_length == 0) {
				if (is_truncated) {
					return fail(index, "Incomplete UTF-8 sequence at the end of the input");
				}

				return fail(index, concat("Invalid UTF-8 byte 0x", hex_upper(static_cast<std::uint32_t>(byte), 2)));
			}

			index += sequence_length;
		}

		if (text_.starts_with("\xEF\xBB\xBF")) {
			return fail(0, "A byte order mark (BOM) is not allowed");
		}

		if (first_control < length) {
			const int control = byte_of(text_[first_control]);

			if (control == '\r') {
				return fail(first_control, "A carriage return (U+000D) is not allowed anywhere. Use LF line endings");
			}

			return fail(first_control, concat("A raw control character (", format_code_point(static_cast<char32_t>(control)), ") is not allowed anywhere, including in strings and comments. In a string, write it as the escape \\u{", hex_lower(static_cast<std::uint32_t>(control)), "} inside \"...\""));
		}

		return true;
	}

	constexpr bool skip_block_comment() {
		const std::size_t start = position_;
		const std::size_t end = text_.find("*/", start + 2);

		if (end == std::string_view::npos) {
			return fail(start, "Unterminated block comment");
		}

		// The body may not contain `/*`. An opening that overlaps the closing `*/`, as in `/*/`, is not in the body.
		const std::size_t body_start = start + 2;
		// Note: Not using std::ranges::search because it regressed long comment bodies compared with string_view::find on both supported libraries.
		const std::size_t nested = text_.substr(body_start, end - body_start).find("/*");

		if (nested != std::string_view::npos) {
			return fail(body_start + nested, "Block comments cannot be nested, and their body may not contain “/*”");
		}

		position_ = end + 2;
		return true;
	}

	/*
	Skips spaces, tabs, line breaks, and comments. `has_crossed_line_break` is set when a line break outside a block comment was crossed.
	*/
	constexpr bool skip_trivia(bool* has_crossed_line_break = nullptr) {
		bool has_crossed = false;
		// A local index, because a `char` read may alias `position_`, which would make the compiler store it after every character.
		const std::size_t size = text_.size();
		std::size_t index = position_;

		while (index < size) {
			const char character = text_[index];

			if (character == ' ' || character == '\t') {
				index = skip_spaces(text_, index);
			} else if (character == '\n') {
				has_crossed = true;
				index++;
			} else if (character == '#') {
				index = find_line_end(index);
			} else if (character == '/' && index + 1 < size && text_[index + 1] == '*') {
				position_ = index;

				if (!skip_block_comment()) {
					return false;
				}

				index = position_;
			} else {
				break;
			}
		}

		position_ = index;

		if (has_crossed_line_break != nullptr) {
			*has_crossed_line_break = has_crossed;
		}

		return true;
	}

	// MARK: Strings

	/*
	The message for a `\u` at `offset` (the `u`) that is not followed by `{`, one to six lowercase hexadecimal digits, and `}`.
	*/
	constexpr std::string describe_bad_unicode_escape(std::size_t offset) const {
		const std::string_view rest = text_.substr(offset, 12);
		const auto four_digits = [](std::string_view text, std::size_t start) -> std::optional<std::uint32_t> {
			if (start + 4 > text.size()) {
				return std::nullopt;
			}

			std::uint32_t code = 0;

			for (std::size_t index = start; index < start + 4; index++) {
				if (!is_hex_digit(text[index])) {
					return std::nullopt;
				}

				code = (code * 16) + static_cast<std::uint32_t>(hex_digit_value(text[index]));
			}

			return code;
		};

		// JSON's four-digit form. JSON writes a character above U+FFFF as two of them, a surrogate pair, which is one `\u{…}` escape here.
		if (const std::optional<std::uint32_t> code = four_digits(rest, 1)) {
			const std::string form = concat("The four-digit \\", rest.substr(0, 5), " form is not an escape");
			const bool has_low = rest.size() >= 7 && rest[5] == '\\' && rest[6] == 'u';
			const std::optional<std::uint32_t> low = has_low ? four_digits(rest, 7) : std::nullopt;

			if (*code >= 0xD800 && *code <= 0xDBFF && low && *low >= 0xDC00 && *low <= 0xDFFF) {
				const std::uint32_t scalar = 0x10000 + ((*code - 0xD800) * 0x400) + (*low - 0xDC00);
				return concat("The four-digit \\", rest.substr(0, 5), "\\u", rest.substr(7, 4), " form is not an escape. Write \\u{", hex_lower(scalar), "}");
			}

			if (*code >= 0xD800 && *code <= 0xDFFF) {
				return concat(form, ", and a lone surrogate is not a Unicode scalar value. Write the character it is half of as one \\u{…} escape");
			}

			if (*code == 0x0D) {
				return concat(form, ", and a carriage return (U+000D) cannot be represented");
			}

			return concat(form, ". Write \\u{", hex_lower(*code), "}");
		}

		if (rest.starts_with("u{")) {
			std::size_t index = 2;

			while (index < rest.size() && is_hex_digit(rest[index])) {
				if (rest[index] >= 'A' && rest[index] <= 'F') {
					return "A Unicode escape uses lowercase hexadecimal digits";
				}

				index++;
			}
		}

		if (rest.starts_with("u{}")) {
			return "A Unicode escape needs one to six hexadecimal digits";
		}

		if (rest.size() >= 9 && rest.starts_with("u{") && std::ranges::all_of(rest.substr(2, 7), [](char character) {
			return lowercase_hex_value(character) != -1;
		})) {
			return "A Unicode escape has at most six hexadecimal digits";
		}

		return "A Unicode escape is written \\u{…} with one to six lowercase hexadecimal digits";
	}

	/*
	Decodes the escape at `offset`, which is a backslash, onto `output`, and sets `end` to the offset after it.
	*/
	constexpr bool decode_escape(std::size_t offset, std::string& output, std::size_t& end) {
		const int character = byte_at(offset + 1);
		end = offset + 2;

		switch (character) {
			case 'n': {
				output.push_back('\n');
				return true;
			}

			case 't': {
				output.push_back('\t');
				return true;
			}

			case '\\':
			case '"': {
				output.push_back(static_cast<char>(character));
				return true;
			}

			case 'u': {
				break;
			}

			case 'r': {
				return fail(offset, "There is no \\r escape, because a carriage return cannot be represented");
			}

			case '\'': {
				return fail(offset, "A ' needs no escape inside \"...\"");
			}

			case -1:
			case '\n': {
				return fail(offset, "A backslash must be followed by an escape character. Use \\\\ for a literal backslash, or a '...' string");
			}

			default: {
				const std::size_t length = std::max<std::size_t>(utf8_sequence_length(text_, offset + 1), 1);
				return fail(offset, concat("Unknown escape “\\", text_.substr(offset + 1, length), "”. The escapes are \\\\, \\\", \\n, \\t, and \\u{…}; use a '...' string for literal backslashes"));
			}
		}

		// `\u{` followed by one to six lowercase hexadecimal digits and `}`.
		const std::size_t digits_start = offset + 3;
		std::size_t index = digits_start;
		std::uint32_t scalar = 0;

		const bool has_brace = byte_at(offset + 2) == '{';
		bool has_uppercase = false;
		if (has_brace) {
			// Uppercase digits too, so that the value of such an escape is checked before its spelling.
			while (index - digits_start < 7) {
				const int digit = byte_at(index);
				if (!is_hex_digit(digit)) {
					break;
				}
				has_uppercase = has_uppercase || (digit >= 'A' && digit <= 'F');
				scalar = (scalar * 16) + static_cast<std::uint32_t>(hex_digit_value(digit));
				index++;
			}
		}

		const std::size_t digit_count = index - digits_start;

		if (!has_brace || digit_count == 0 || digit_count > 6 || byte_at(index) != '}') {
			return fail(offset, describe_bad_unicode_escape(offset + 1));
		}

		const std::string_view hex = text_.substr(digits_start, digit_count);

		// The value is checked before the spelling, so that the uppercase and leading zeros errors never point to an escape that is not allowed either.
		if (scalar == 0x0D) {
			return fail(offset, concat("A carriage return (U+000D) cannot be represented, so \\u{", hex, "} is not allowed"));
		}

		if (scalar >= 0xD800 && scalar <= 0xDFFF) {
			return fail(offset, concat("\\u{", hex, "} is a surrogate, which is not a Unicode scalar value"));
		}

		if (scalar > 0x10FFFF) {
			return fail(offset, concat("\\u{", hex, "} is above U+10FFFF, the largest Unicode scalar value"));
		}

		if (has_uppercase) {
			return fail(offset, "A Unicode escape uses lowercase hexadecimal digits");
		}

		if (digit_count > 1 && hex[0] == '0') {
			return fail(offset, concat("A Unicode escape may not have leading zeros; write \\u{", hex_lower(scalar), "}"));
		}

		append_utf8(output, static_cast<char32_t>(scalar));
		end = index + 1;
		return true;
	}

	/*
	Decodes the escapes in text[start, end) onto `output`.
	*/
	constexpr bool unescape(std::size_t start, std::size_t end, std::string& output) {
		std::size_t index = start;

		while (index < end) {
			std::size_t backslash = text_.substr(0, end).find('\\', index);

			if (backslash == std::string_view::npos) {
				backslash = end;
			}

			output += text_.substr(index, backslash - index);

			if (backslash == end) {
				break;
			}

			if (!decode_escape(backslash, output, index)) {
				return false;
			}
		}

		return true;
	}

	constexpr bool parse_literal_string(std::string& output) {
		const std::size_t start = position_;
		const std::size_t index = skip_string_characters(text_, start + 1, '\'');

		if (index >= text_.size() || text_[index] != '\'') {
			return fail(start, "Unterminated string. A '...' string must end on the line it starts on; use a block string (''') for multiple lines");
		}

		output.assign(text_.substr(start + 1, index - start - 1));
		position_ = index + 1;
		return true;
	}

	constexpr bool parse_escaped_string(std::string& output) {
		const std::size_t start = position_;
		std::size_t index = start + 1;
		std::size_t chunk_start = index;
		output.clear();

		for (;;) {
			// The common case, a run of plain characters, is skipped in one step.
			index = skip_string_characters(text_, index, '"');

			const int character = byte_at(index);

			if (character == -1 || character == '\n') {
				return fail(start, "Unterminated string. A \"...\" string must end on the line it starts on; use a block string (\"\"\") for multiple lines");
			}

			output += text_.substr(chunk_start, index - chunk_start);

			if (character == '"') {
				break;
			}

			if (!decode_escape(index, output, index)) {
				return false;
			}

			chunk_start = index;
		}

		position_ = index + 1;
		return true;
	}

	constexpr bool is_blank(std::size_t start, std::size_t end) const noexcept {
		return skip_spaces(text_, start) >= end;
	}

	/*
	The hint for an unterminated block string whose content line ends with the delimiter, as TOML allows. Adding a closing line after it would keep the delimiter as content.
	*/
	constexpr std::string describe_inline_closing_delimiter(std::size_t content_start, char quote, std::size_t delimiter_length) const {
		if (delimiter_length > max_diagnosed_length) {
			return {};
		}

		std::size_t line = locate(text_, content_start).line;

		for (std::size_t line_start = content_start; line_start < text_.size();) {
			const std::size_t line_end = find_line_end(line_start);
			const std::size_t start = skip_spaces(text_, line_start);
			std::size_t end = line_end;

			while (end > start && is_space(text_[end - 1])) {
				end--;
			}

			std::size_t run = 0;

			while (run < end - start && text_[end - 1 - run] == quote) {
				run++;
			}

			if (run >= delimiter_length && run < end - start) {
				return concat(". Its closing delimiter must start a line, so move the ", std::string(delimiter_length, quote), " at the end of line ", decimal_string(line), " to a new line");
			}

			line_start = line_end + 1;
			line++;
		}

		return {};
	}

	/*
	A block string opens with a run of three or more quotes and a line break, and closes at the first line whose first content after spaces and tabs is a run of exactly that many quotes. The closing line's indentation is removed from every content line except a blank one, which holds only spaces and tabs and becomes an empty line. Blank lines at the start and the end are dropped.
	*/
	constexpr bool parse_block_string(char quote, std::string& output) {
		const std::size_t start = position_;
		std::size_t delimiter_length = 0;

		while (start + delimiter_length < text_.size() && text_[start + delimiter_length] == quote) {
			delimiter_length++;
		}

		position_ = start + delimiter_length;

		if (is_at_end()) {
			return fail(start, "Unterminated block string");
		}

		if (peek() != '\n') {
			return fail(position_, "A block string's opening delimiter must be followed directly by a line break, and its content starts on the next line");
		}

		// Find the closing line, and the first and last lines that are not blank.
		const std::size_t content_start = position_ + 1;
		std::size_t line_start = content_start;
		std::size_t first_line = std::string_view::npos;
		std::size_t last_line_end = 0;
		std::size_t indentation_start = 0;
		std::size_t indentation_end = 0;

		for (;;) {
			if (line_start >= text_.size()) {
				return fail(start, concat("Unterminated block string", describe_inline_closing_delimiter(content_start, quote, delimiter_length)));
			}

			const std::size_t line_end = find_line_end(line_start);
			const std::size_t content = skip_spaces(text_, line_start);
			std::size_t run = 0;

			while (content + run < text_.size() && text_[content + run] == quote) {
				run++;
			}

			if (run == delimiter_length) {
				indentation_start = line_start;
				indentation_end = content;
				position_ = content + delimiter_length;
				break;
			}

			if (content < line_end) {
				if (first_line == std::string_view::npos) {
					first_line = line_start;
				}

				last_line_end = line_end;
			}

			line_start = line_end + 1;
		}

		output.clear();

		if (first_line == std::string_view::npos) {
			return true;
		}

		const std::string_view indentation = text_.substr(indentation_start, indentation_end - indentation_start);
		std::size_t output_length = 0;

		// Every kept line that is not blank must start with the indentation. Dropped leading and trailing lines were already found to be blank.
		for (std::size_t line = first_line; line <= last_line_end;) {
			const std::size_t line_end = find_line_end(line);
			const bool blank = is_blank(line, line_end);

			if (!blank && !text_.substr(line, line_end - line).starts_with(indentation)) {
				return fail(line, "This line does not start with the indentation of its block string's closing delimiter. Every line except a blank one must start with exactly the same spaces and tabs");
			}

			output_length += (line > first_line ? 1U : 0U) + (blank ? 0 : line_end - line - indentation.size());

			line = line_end + 1;
		}

		output.reserve(output_length);

		for (std::size_t line = first_line; line <= last_line_end;) {
			const std::size_t line_end = find_line_end(line);

			if (line > first_line) {
				output.push_back('\n');
			}

			if (!is_blank(line, line_end)) {
				const std::size_t line_content = line + indentation.size();

				if (quote == '\'') {
					output += text_.substr(line_content, line_end - line_content);
				} else if (!unescape(line_content, line_end, output)) {
					return false;
				}
			}

			if (line_end >= last_line_end) {
				break;
			}

			line = line_end + 1;
		}

		return true;
	}

	[[gnu::noinline]] constexpr bool parse_string(std::string& output) {
		const char quote = text_[position_];

		if (peek(1) == quote && peek(2) == quote) {
			return parse_block_string(quote, output);
		}

		return quote == '\'' ? parse_literal_string(output) : parse_escaped_string(output);
	}

	// MARK: Numbers, dates, instants, and durations

	constexpr bool parse_integer(std::string_view token, std::size_t start, std::int64_t& output) {
		const bool is_negative = token[0] == '-';
		std::size_t index = is_negative ? 1 : 0;
		int radix = token.size() >= 2 && token[0] == '0' ? radix_of(token[1]) : 0;

		if (radix == 0) {
			radix = 10;
		} else {
			index = 2;
		}

		const std::uint64_t limit = is_negative ? std::uint64_t{1} << 63U : (std::uint64_t{1} << 63U) - 1;
		std::uint64_t total = 0;
		if (!read_integer_magnitude(token.substr(index), radix, total) || total > limit) {
			return fail(start, concat("The integer ", abbreviate(token), " is outside the 64-bit range (-9223372036854775808 to 9223372036854775807)"));
		}

		output = to_signed(total, is_negative);
		return true;
	}

	/*
	Reads every part of a duration in order, so each error names the part it is about, and then checks the value: a whole number of nanoseconds, no `-` before zero, and the int64 range.
	*/
	constexpr bool parse_duration(std::string_view token, std::size_t start, soml::duration& output) {
		const auto failure = [&](std::string_view reason) {
			return fail(start, concat("Invalid duration ", abbreviate(token), ": ", reason));
		};

		const bool is_negative = token[0] == '-';
		std::size_t index = is_negative ? 1 : 0;
		int previous_rank = -1;
		std::uint64_t total = 0;
		bool is_out_of_range = false;
		// The last part's fraction and its unit. Only the last part may have one.
		std::size_t fraction_start = 0;
		std::size_t fraction_length = 0;
		const duration_unit* fraction_unit = nullptr;

		while (index < token.size()) {
			const std::size_t digits_end = skip_integer_part(token, index);

			if (digits_end == index) {
				if (token[index] == '-' && index + 1 < token.size() && is_digit(token[index + 1])) {
					return failure("only the whole duration takes a sign, as in -1h30m");
				}

				return failure(token[index] == '+' ? std::string{"a “+” sign is not allowed"} : concat("expected a number at “", abbreviate(token.substr(index), 10), "”"));
			}

			const int next = digits_end < token.size() ? token[digits_end] : -1;

			if (token[index] == '0' && (is_digit(next) || next == '_')) {
				return failure("leading zeros are not allowed");
			}

			if (next == '_') {
				return failure("an underscore must be between two digits");
			}

			fraction_length = 0;
			std::size_t unit_start = digits_end;

			if (next == '.') {
				unit_start = skip_digits(token, digits_end + 1, 10);

				if (unit_start == digits_end + 1) {
					return failure("a “.” must be followed by a digit");
				}

				if (unit_start < token.size() && token[unit_start] == '_') {
					return failure("an underscore must be between two digits");
				}

				fraction_start = digits_end + 1;
				fraction_length = unit_start - fraction_start;
			}

			// Note: Not using std::ranges::find_if_not because it needed suffix/offset adaptation without a consistent gain for these short unit names.
			std::size_t unit_end = unit_start;

			while (unit_end < token.size() && is_letter(token[unit_end])) {
				unit_end++;
			}

			const std::string_view unit_name = token.substr(unit_start, unit_end - unit_start);
			const duration_unit* unit = duration_unit::find(unit_name);

			if (unit == nullptr) {
				return failure(describe_unit(unit_name, token));
			}

			const int rank = static_cast<int>(unit - duration_units.data());

			if (rank <= previous_rank) {
				return failure("the units are in the order h, m, s, ms, us, ns, and each appears at most once");
			}

			if (next == '.' && unit_end < token.size() && is_digit(token[unit_end])) {
				return failure("only the last part may have a fraction");
			}

			std::uint64_t whole = 0;
			std::uint64_t part = 0;

			if (!is_out_of_range && (!read_integer_magnitude(token.substr(index, digits_end - index), 10, whole) || __builtin_mul_overflow(whole, unit->nanoseconds, &part) || !add(total, total, part))) {
				is_out_of_range = true;
			}

			fraction_unit = unit;
			index = unit_end;
			previous_rank = rank;
		}

		if (fraction_length > 0) {
			// The fraction's digits without underscores and trailing zeros, as F / 10^k.
			std::uint64_t numerator = 0;
			std::size_t digit_count = 0;
			std::size_t pending_zeros = 0;

			for (std::size_t digit = fraction_start; digit < fraction_start + fraction_length; digit++) {
				if (token[digit] == '_') {
					continue;
				}

				if (token[digit] == '0') {
					pending_zeros++;
					continue;
				}

				// More than 13 significant digits is finer than a nanosecond in any unit.
				if (digit_count + pending_zeros + 1 > 13) {
					return failure("it is not a whole number of nanoseconds");
				}

				numerator *= duration_powers[pending_zeros];
				digit_count += pending_zeros;
				pending_zeros = 0;

				numerator = (numerator * 10) + static_cast<std::uint64_t>(token[digit] - '0');
				digit_count++;
			}

			if (digit_count > 0) {
				// F × multiplier × 10^power / 10^k must be whole. F has no factor of 10 at its end, and no multiplier has a factor of 5, so that needs k ≤ power + 2.
				const auto power = static_cast<std::size_t>(fraction_unit->power);

				if (digit_count > power + 2) {
					return failure("it is not a whole number of nanoseconds");
				}

				const std::uint64_t scaled = numerator * fraction_unit->multiplier;
				std::uint64_t fraction_nanoseconds = 0;

				if (digit_count <= power) {
					fraction_nanoseconds = scaled * duration_powers[power - digit_count];
				} else {
					const std::uint64_t divisor = duration_powers[digit_count - power];

					if (scaled % divisor != 0) {
						return failure("it is not a whole number of nanoseconds");
					}

					fraction_nanoseconds = scaled / divisor;
				}

				if (!add(total, total, fraction_nanoseconds)) {
					is_out_of_range = true;
				}
			}
		}

		if (is_negative && total == 0 && !is_out_of_range) {
			return failure("“-” is not allowed before zero, because zero has one spelling: 0s");
		}

		if (is_out_of_range || total > (is_negative ? std::uint64_t{1} << 63U : (std::uint64_t{1} << 63U) - 1)) {
			return failure("it is outside the 64-bit range of nanoseconds, about 292 years either way");
		}

		output = soml::duration{to_signed(total, is_negative)};
		return true;
	}

	/*
	A date and a time with a space between them, as TOML and Python write a date and time, where an instant has a "T". Following the example must not turn a local time into UTC silently, so a time without an offset is described as what it is.
	*/
	static constexpr std::string describe_space_separated_instant(std::string_view date, std::string_view time, bool is_whole_value) {
		constexpr std::string_view separator = "The date and time separator in an instant is an uppercase “T”, not a space";

		if (date.size() + 1 + time.size() > max_diagnosed_length) {
			return concat(separator, ". ", instant_format);
		}

		const std::string instant = concat(date, "T", time);

		if (is_valid_value(instant)) {
			return concat(separator, ", as in ", abbreviate(instant));
		}

		if (is_whole_value && is_local_date_time(instant) && is_valid_value(concat(instant, "Z"))) {
			return concat(separator, ", and an instant needs the offset it was meant in, as in ", abbreviate(instant), "Z for UTC. A date and time without an offset is a local time, which is not an instant, so write it as a string, as in '", abbreviate(concat(date, " ", time)), "'");
		}

		return concat(separator, ". ", instant_format);
	}

	/*
	A message for a token that starts with a date but is not in the shape of an instant, as the reference implementation describes it.
	*/
	constexpr std::string describe_bad_instant(std::string_view token, std::size_t start) const {
		if (token.size() > max_diagnosed_length) {
			return concat("Invalid instant “", abbreviate(token), "”. ", instant_format);
		}

		// A time after spaces or tabs, and whether the value ends after it, or after the token without one. More of the instant may follow, as in `14:00:00,5Z` or `14:00:00[Europe/Oslo]`, so it may have an offset.
		const std::size_t token_end = start + token.size();
		const std::size_t time_start = skip_spaces(text_, token_end);
		std::size_t time_end = token_end;

		if (time_start > token_end && is_digit(byte_at(time_start))) {
			time_end = time_start + 1;

			while (time_end < text_.size() && is_number_character(text_[time_end])) {
				time_end++;
			}
		}

		const bool is_whole_value = is_value_end_at(time_end) && !(byte_at(time_end) == ',' && is_digit(byte_at(time_end + 1)));

		if (token.size() == 10) {
			const std::string_view time = time_end > time_start ? text_.substr(time_start, time_end - time_start) : std::string_view{};

			// A date alone is valid; only a following time reaches the instant separator guidance.
			return describe_space_separated_instant(token, time, is_whole_value);
		}

		if (token[10] == 't') {
			return "The date and time separator in an instant is an uppercase “T”";
		}

		if (token.ends_with('z')) {
			return "The UTC offset in an instant is an uppercase “Z”";
		}

		if (token.size() >= 5 && (token[token.size() - 5] == '+' || token[token.size() - 5] == '-') && has_digits(token, token.size() - 4, 4)) {
			return "An instant's offset is written with a colon, as in +07:00";
		}

		if (!is_local_date_time(token)) {
			return concat("Invalid instant “", abbreviate(token), "”. ", instant_format);
		}

		// An offset on its own after spaces, as in `2026-09-19T14:00:00 Z`, which belongs directly after the time. The instant has the offset it was meant in, so it is not a local time to write as a string.
		if (const std::size_t offset_start = skip_spaces(text_, token_end); offset_start > token_end) {
			std::size_t offset_length = 0;

			if (byte_at(offset_start) == 'Z') {
				offset_length = 1;
			} else if ((byte_at(offset_start) == '+' || byte_at(offset_start) == '-') && has_digits(text_, offset_start + 1, 2) && byte_at(offset_start + 3) == ':' && has_digits(text_, offset_start + 4, 2)) {
				offset_length = 6;
			}

			if (offset_length > 0 && is_value_end_at(offset_start + offset_length)) {
				const std::string instant = concat(token, text_.substr(offset_start, offset_length));
				constexpr std::string_view reason = "An instant's offset follows its time directly, without a space";
				return is_valid_value(instant) ? concat(reason, ", as in ", abbreviate(instant)) : std::string{reason};
			}
		}

		return is_whole_value ? concat("An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, as in '", abbreviate(token), "', or add the offset it was meant in") : std::string{"An instant needs an offset: Z or ±HH:MM"};
	}

	/*
	A complete date preserves Gregorian components directly, without time or offset conversion.
	*/
	constexpr bool parse_date(std::string_view token, std::size_t start, soml::date& output) {
		std::int64_t last_day = 0;
		const std::string_view reason = read_date(token, output, last_day);

		if (reason == "day") {
			return fail(start, concat("Invalid date ", token, ": the day must be 01 to ", decimal_string(static_cast<std::uint64_t>(last_day)), " in that month"));
		}

		if (!reason.empty()) {
			return fail(start, concat("Invalid date ", token, ": ", reason));
		}

		return true;
	}

	constexpr bool parse_instant(std::string_view token, std::size_t start, soml::instant& output) {
		if (token.size() > max_diagnosed_length || !has_instant_time_and_offset(token)) {
			return fail(start, describe_bad_instant(token, start));
		}

		std::int64_t last_day = 0;
		const std::string_view reason = read_instant(token, output, last_day);

		if (reason == "day") {
			return fail(start, concat("Invalid instant ", abbreviate(token), ": the day must be 01 to ", decimal_string(static_cast<std::uint64_t>(last_day)), " in that month"));
		}

		if (!reason.empty()) {
			return fail(start, concat("Invalid instant ", abbreviate(token), ": ", reason));
		}

		return true;
	}

	/*
	A comma used as a thousands separator, as in `[1,000]`, makes the group after it a separate item, which fails only when it has a leading zero, as the token at `start` does. Writing that item in octal, as the leading zero message suggests, would parse to the wrong items.
	*/
	constexpr std::optional<std::string> describe_thousands_separator(std::string_view token, std::size_t start) const {
		if (token.size() != 3 || token[0] != '0' || !is_digit(token[1]) || !is_digit(token[2]) || byte_at(start - 1) != ',') {
			return std::nullopt;
		}

		std::size_t group_end = start - 1;
		std::size_t number_start = digits_start(text_, group_end);

		// Earlier groups of three digits, as in `12,345,000`.
		while (group_end - number_start == 3 && byte_at(number_start - 1) == ',' && is_digit(byte_at(number_start - 2))) {
			group_end = number_start - 1;
			number_start = digits_start(text_, group_end);
		}

		const int before = number_start == 0 ? -1 : byte_at(number_start - 1);

		// The first group has one to three digits, which are not the end of a float, of a number with underscores, or of a number with a radix prefix.
		if (number_start == group_end || before == '.' || before == '_' || number_start + 3 < group_end || is_letter(before)) {
			return std::nullopt;
		}

		// The sign belongs to the number, as in `-1,000`.
		if (before == '-') {
			number_start--;
		}

		std::size_t number_end = start + 3;

		while (number_end + 4 <= text_.size() && text_[number_end] == ',' && has_digits(text_, number_end + 1, 3) && !is_digit(byte_at(number_end + 4))) {
			number_end += 4;
		}

		const std::string_view number = text_.substr(number_start, number_end - number_start);
		std::string reason = concat("Leading zeros are not allowed in a decimal number. A comma separates items, so ", abbreviate(number), " is not one number");
		if (number.size() > max_diagnosed_length) {
			return reason;
		}

		std::string with_underscores{number};
		std::ranges::replace(with_underscores, ',', '_');
		std::string without_commas{number};
		std::erase(without_commas, ',');

		return is_valid_value(with_underscores) ? concat(reason, ". Write it as ", abbreviate(with_underscores), " or ", abbreviate(without_commas)) : std::move(reason);
	}

	/*
	The common case, a token that is a short decimal int or float such as `8080`, `-3`, `30.5`, or `1e3`, without the general path's classification. Anything else, including every error, returns `false` and is left to the general path.
	*/
	constexpr bool parse_plain_number(value& output) {
		const std::string_view token = text_.substr(position_);
		std::size_t index = token[0] == '-' ? 1 : 0;
		const std::size_t integer_start = index;
		std::uint64_t mantissa = 0;

		while (index < token.size() && is_digit(token[index])) {
			mantissa = (mantissa * 10) + static_cast<std::uint64_t>(token[index] - '0');
			index++;

			if (index - integer_start > 19) {
				return false;
			}
		}

		const std::size_t integer_length = index - integer_start;

		// No digits, or a leading zero followed by more digits, which the general path reports as an error or reads as an instant.
		if (integer_length == 0 || (integer_length > 1 && token[integer_start] == '0')) {
			return false;
		}

		std::size_t fraction_length = 0;

		if (index < token.size() && token[index] == '.') {
			const std::size_t fraction_start = index + 1;
			index = fraction_start;

			while (index < token.size() && is_digit(token[index])) {
				mantissa = (mantissa * 10) + static_cast<std::uint64_t>(token[index] - '0');
				index++;

				// 15 digits and the `.`, so that the mantissa is exact in a double.
				if (index - integer_start > 16) {
					return false;
				}
			}

			fraction_length = index - fraction_start;

			if (fraction_length == 0) {
				return false;
			}
		}

		int power = -static_cast<int>(fraction_length);
		const bool has_exponent = index < token.size() && token[index] == 'e';

		if (has_exponent) {
			index++;
			const bool is_negative_exponent = index < token.size() && token[index] == '-';
			index += is_negative_exponent ? 1U : 0U;
			const std::size_t exponent_start = index;
			int exponent = 0;

			while (index < token.size() && is_digit(token[index])) {
				exponent = (exponent * 10) + (token[index] - '0');
				index++;

				if (exponent > 22 || (token[exponent_start] == '0' && index > exponent_start + 1)) {
					return false;
				}
			}

			if (index == exponent_start || (is_negative_exponent && exponent == 0)) {
				return false;
			}

			power += is_negative_exponent ? -exponent : exponent;
		}

		// More of the token, such as an underscore or a unit, needs the general path.
		if (index < token.size() && !is_value_end(byte_of(token[index]))) {
			return false;
		}

		const bool is_negative = token[0] == '-';

		if (fraction_length > 0 || has_exponent) {
			// At most 15 digits and a power of at most 22, so Clinger's fast path is exact.
			if (integer_length + fraction_length > 15 || power < -22 || power > 22 || !rounds_to_nearest()) {
				return false;
			}

			const double number = power >= 0 ? static_cast<double>(mantissa) * powers_of_ten[static_cast<std::size_t>(power)] : static_cast<double>(mantissa) / powers_of_ten[static_cast<std::size_t>(-power)];
			// Negative zero is the same value as zero.
			output = number == 0.0 ? 0.0 : (is_negative ? -number : number);
		} else {
			// `-0` is an error, which the general path reports.
			if (mantissa == 0 && is_negative) {
				return false;
			}

			const std::uint64_t limit = (std::uint64_t{1} << 63U) - (is_negative ? 0U : 1U);
			if (mantissa > limit) {
				return false;
			}
			output = to_signed(mantissa, is_negative);
		}

		position_ += index;
		return true;
	}

	/*
	A number, a date, an instant, or a duration: one maximal run of number characters, classified as a whole.
	*/
	[[gnu::noinline]] constexpr bool parse_number_like(value& output, bool& is_number) {
		if (parse_plain_number(output)) {
			is_number = true;
			return true;
		}

		const std::size_t start = position_;
		std::size_t end = start + 1;

		while (end < text_.size() && is_number_character(text_[end])) {
			end++;
		}

		const std::string_view token = text_.substr(start, end - start);
		position_ = end;

		if (has_date(token)) {
			if (token.size() == 10 && !has_time_after_space(text_, end)) {
				soml::date date;
				if (!parse_date(token, start, date)) {
					return false;
				}
				output = date;
				return true;
			}
			soml::instant instant;

			if (!parse_instant(token, start, instant)) {
				return false;
			}

			output = instant;
			return true;
		}

		switch (classify_number(token)) {
			case number_kind::decimal:
			case number_kind::radix: {
				if (token == "-0") {
					return fail(start, "“-0” is not allowed, because zero has one spelling: 0");
				}

				std::int64_t integer = 0;

				if (!parse_integer(token, start, integer)) {
					return false;
				}

				output = integer;
				is_number = true;
				return true;
			}

			case number_kind::floating: {
				double number = 0.0;

				switch (parse_float(token, number)) {
					case float_status::ok: {
						break;
					}

					case float_status::too_large: {
						return fail(start, concat(abbreviate(token), " is too large to be a finite float. Use ", token[0] == '-' ? "-infinity" : "infinity", " if you mean it"));
					}

					case float_status::too_small: {
						return fail(start, concat(abbreviate(token), " is too small to be told apart from zero. Write 0.0 if you mean zero"));
					}
				}

				output = number;
				is_number = true;
				return true;
			}

			case number_kind::none: {
				break;
			}
		}

		if (is_duration_like(token)) {
			soml::duration length{};

			if (!parse_duration(token, start, length)) {
				return false;
			}

			output = length;
			return true;
		}

		if (std::optional<std::string> message = describe_thousands_separator(token, start)) {
			return fail(start, std::move(*message));
		}

		return fail(start, describe_bad_number(token, unquoted_text(start)));
	}

	// MARK: Structure

	constexpr bool is_keyword(std::string_view word) noexcept {
		if (!text_.substr(position_).starts_with(word) || is_bare_key_character(peek(word.size()))) {
			return false;
		}

		position_ += word.size();
		return true;
	}

	/*
	A TOML table header, as in `[server]` or `[[servers]]` on a line of its own, reads as an array that holds a word, or as a key that starts with "[". Where a value is expected, `is_value`, it is only a table header when its bracket opens the document, because a table cannot be written as an item of an array inside it. Returns its message, or an empty string.
	*/
	constexpr std::string describe_table_header(bool is_value) const {
		const std::size_t line_start = line_start_of(position_);

		if (is_value && skip_spaces(text_, line_start) != document_start_) {
			return {};
		}

		const std::size_t end = std::min(find_line_end(position_), line_start + max_diagnosed_length);
		std::size_t index = skip_spaces(text_, line_start);

		if (index >= end || text_[index] != '[') {
			return {};
		}

		index += index + 1 < end && text_[index + 1] == '[' ? 2U : 1U;

		if (index >= end || !(is_letter(text_[index]) || text_[index] == '_')) {
			return {};
		}

		const std::size_t name_start = index;

		while (index < end && (is_bare_key_character(text_[index]))) {
			index++;
		}

		const std::string_view name = text_.substr(name_start, index - name_start);

		if (index >= end || text_[index] != ']') {
			return {};
		}

		index += index + 1 < end && text_[index + 1] == ']' ? 2U : 1U;

		while (index < end && is_space(text_[index])) {
			index++;
		}

		return index == end ? concat("There are no table headers. Write the table as an object, as in ", abbreviate(name), ": {…}") : std::string{};
	}

	/*
	Whether a YAML literal block scalar indicator, as in `key: |` or `key: |-`, is at the current position, at the end of its line. A folded block scalar, `>`, joins its lines, which a block string does not, so it is not one.
	*/
	constexpr bool is_block_scalar_indicator() const noexcept {
		if (peek() != '|') {
			return false;
		}

		const int chomping = peek(1);
		const int next = byte_at(skip_spaces(text_, chomping == '-' || chomping == '+' ? position_ + 2 : position_ + 1));
		return next == '\n' || next == -1;
	}

	[[gnu::noinline]] constexpr bool fail_unexpected_value() {
		const std::size_t start = position_;

		if (is_at_end()) {
			return fail(start, "Expected a value, but reached the end of the document");
		}

		const int character = peek();

		if (character == '+') {
			return fail(start, "A “+” sign is not allowed. A number without a sign is positive");
		}

		// A `.` that no digit follows begins a string, such as `.env` or `./foo`, rather than a number.
		if (character == '.') {
			return fail(start, is_digit(peek(1)) ? std::string{"A number cannot begin with “.”; write a digit before it, as in 0.5"} : describe_unknown_word(".", unquoted_text(start)));
		}

		const std::size_t end = detail::skip_bare_key(text_, start);

		if (end > start) {
			const std::string_view word = text_.substr(start, end - start);
			// A key where a value should be, as when the value of an entry is left out and the next line has a key, or as in `[a: 1]`, is reported as a key. A word after the `:` of a member and a space, as in `msg: Error: file not found`, is that member's value instead, an unquoted string.
			const std::size_t spaces_start = skip_blanks_back(start);
			const bool is_member_value = spaces_start < start && byte_at(spaces_start - 1) == ':';

			if (!is_member_value && byte_at(end) == ':') {
				return fail(start, concat("Expected a value, but found the key ", abbreviate(word)));
			}

			if (std::string message = describe_table_header(true); !message.empty()) {
				return fail(start, std::move(message));
			}

			return fail(start, describe_unknown_word(word, unquoted_text(start)));
		}

		return fail(start, concat("Expected a value, but found ", describe_here(), is_block_scalar_indicator() ? std::string_view{". Write a multiline string as a block string, between ''' lines"} : slash_comment_hint()));
	}

	/*
	`key = bare-key / literal / escaped`.
	*/
	[[gnu::noinline]] constexpr bool parse_key(std::string& output) {
		const std::size_t start = position_;
		const int character = peek();

		if (character == '\'' || character == '"') {
			if (peek(1) == character && peek(2) == character) {
				return fail(start, "A block string cannot be a key");
			}

			// A key is a string, so it may hold anything a string can, including a line feed written as an escape.
			return character == '\'' ? parse_literal_string(output) : parse_escaped_string(output);
		}

		const std::size_t end = detail::skip_bare_key(text_, start);

		if (end == start) {
			if (character == '[') {
				if (std::string message = describe_table_header(false); !message.empty()) {
					return fail(start, std::move(message));
				}
			}

			if (is_at_end()) {
				return fail(start, "Expected a key");
			}

			return fail(start, concat("Expected a key, but found ", describe_here(), key_quoting_hint_here(), slash_comment_hint()));
		}

		output.assign(text_.substr(start, end - start));
		position_ = end;
		return true;
	}

	/*
	A `.` directly after a key, as in `example.com: 1`. A key is never a path, so the `.` is meant either as part of the key or as nesting. The example of quoting and nesting is only given for a bare key whose dots each have a word on both sides and that a `:` follows, so that both are valid.
	*/
	[[gnu::noinline]] constexpr bool fail_dot_in_key(std::size_t key_start) {
		std::size_t end = position_;

		while (end < key_start + max_diagnosed_length && (is_bare_key_character(byte_at(end)) || byte_at(end) == '.')) {
			end++;
		}

		const std::string_view key = text_.substr(key_start, end - key_start);
		const bool has_empty_word = key.starts_with('.') || key.ends_with('.') || key.contains("..");

		if (byte_at(end) != ':' || !is_bare_key_character(byte_at(key_start)) || has_empty_word) {
			return fail(position_, "A key cannot contain “.” unless it is quoted. Quote the whole key, or use braces to nest, as in a: {b: …}");
		}

		std::string nested;
		std::size_t word_count = 0;

		for (std::size_t word_start = 0; word_start <= key.size();) {
			const std::size_t dot = std::min(key.find('.', word_start), key.size());

			if (word_count > 0) {
				nested += ": {";
			}

			nested += key.substr(word_start, dot - word_start);
			word_count++;
			word_start = dot + 1;
		}

		nested += ": …";
		nested.append(word_count - 1, '}');
		return fail(position_, concat("A bare key cannot contain “.”. Quote it, as in '", abbreviate(key), "', or use braces to nest, as in ", abbreviate(nested)));
	}

	[[gnu::noinline]] constexpr bool fail_missing_colon(std::size_t key_start) {
		const std::size_t next = skip_spaces(text_, position_);

		if (next > position_ && byte_at(next) == ':') {
			return fail(position_, "Whitespace is not allowed between a key and its “:”");
		}

		// A block comment followed by the `:` was meant to come before it.
		if (byte_at(next) == '/' && byte_at(next + 1) == '*') {
			const std::size_t comment_end = text_.find("*/", next + 2);

			if (comment_end != std::string_view::npos && byte_at(skip_spaces(text_, comment_end + 2)) == ':') {
				return fail(next, "A comment is not allowed between a key and its “:”");
			}
		}

		// A key with a space in it, such as `the name: 1`. The hint is only given for plain words, because quoting a quoted key would change what it means, and for a `:` that whitespace or the end follows, because quoting the words before the `:` in `server localhost:8080` would give a valid document with another meaning.
		if (next > position_ && is_bare_key_character(byte_at(next))) {
			const std::size_t line_end = find_line_end(next);
			const std::size_t colon = text_.substr(0, line_end).find(':', next);

			if (colon != std::string_view::npos) {
				const int after_colon = byte_at(colon + 1);

				if (after_colon == -1 || after_colon == '\n' || is_space(after_colon)) {
					const std::string_view key = text_.substr(key_start, skip_blanks_back(colon) - key_start);
					const bool is_words_with_spaces = std::ranges::all_of(key, [](char character) {
						return is_space(character) || is_bare_key_character(character);
					});

					if (is_words_with_spaces) {
						return fail(key_start, concat("A bare key cannot contain spaces. Quote it, as in '", abbreviate(key), "'"));
					}
				}
			}
		}

		if (is_at_end() || peek() == '\n') {
			return fail(position_, "Expected “:” after the key");
		}

		// A character directly after a bare key is most likely meant to be part of it. A quoted key is quoted already.
		const char previous = text_[position_ - 1];
		const bool has_hint = next == position_ && previous != '\'' && previous != '"' && quotable_key_character_length(next) != 0;
		position_ = next;
		return fail(next, concat("Expected “:” after the key, but found ", describe_here(), has_hint ? key_quoting_hint : std::string_view{}));
	}

	/*
	The number of spaces and tabs before `offset` on its line, or `std::nullopt` when something else comes before it.
	*/
	constexpr std::optional<std::size_t> indentation_at(std::size_t offset) const noexcept {
		const std::size_t line_start = line_start_of(offset);
		return skip_spaces(text_, line_start) >= offset ? std::optional{offset - line_start} : std::nullopt;
	}

	/*
	A member whose value was left out, as in `a:` followed by `'b': 1` on the next line, reads the next key as its value, which fails. That failure is reported as what it is, at the key, and so is a YAML block sequence. Returns whether the failure is reported.
	*/
	[[gnu::noinline]] constexpr bool diagnose_missing_value(std::size_t start, std::string_view parent, std::size_t key_start, std::string_view comment_hint) {
		const int first = byte_at(start);

		if (first == '-' && is_space(byte_at(start + 1))) {
			fail(start, "Expected a value, but found a “-” list. An array is written in brackets, as in [80, 443]");
			return true;
		}
		if (first != '\'' && first != '"' && !is_bare_key_character(first)) {
			return false;
		}

		error_state saved = std::move(error_);
		std::string key;
		position_ = start;
		const bool is_key = parse_key(key) && peek() == ':';
		error_ = std::move(saved);

		// Whitespace or the end follows the `:` of a bare key. A digit follows the `:` in an instant such as `2026-09-19T25:00:00Z`, whose own error is more precise. A quoted key cannot be part of a value, so anything may follow its `:`.
		const int next = peek(1);

		if (!is_key || !(first == '\'' || first == '"' || next == -1 || next == '\n' || is_space(next))) {
			return false;
		}

		std::string hint{comment_hint};

		if (comment_hint.empty()) {
			// A key indented under the entry whose value is missing, as YAML nests an object.
			const std::optional<std::size_t> key_indentation = indentation_at(key_start);
			const std::optional<std::size_t> indentation = indentation_at(start);

			if (key_indentation && indentation && *indentation > *key_indentation && parent.size() <= max_diagnosed_length && key.size() <= max_diagnosed_length) {
				hint = concat(". Indentation does not nest objects, so write ", abbreviate(format_key(parent), 200), ": {", abbreviate(format_key(key), 200), ": …}");
			}
		}

		fail(start, concat("Expected a value, but found the key ", describe_key(key), hint));
		return true;
	}

	/*
	Whether a key that starts at `key_start` is a bare key followed directly by `:` and more of a key, up to a `:` that whitespace or the end follows, as in `a:b: 1`. Returns where that `:` is.
	*/
	constexpr std::size_t find_key_with_colon(std::size_t key_start) const noexcept {
		for (std::size_t index = key_start; index < key_start + max_diagnosed_length; index++) {
			const int character = byte_at(index);

			if (character == ':') {
				const int next = byte_at(index + 1);

				if (next == -1 || next == '\n' || is_space(next)) {
					return index;
				}
			} else if (!is_bare_key_character(character)) {
				return std::string_view::npos;
			}
		}

		return std::string_view::npos;
	}

	/*
	The hint for a `#` directly after a `:`, as in `color: #FFF`, which starts a comment rather than a value. A `#` followed by a space, or by another `#`, starts an ordinary comment.
	*/
	constexpr std::string describe_comment_as_value(std::size_t colon) const {
		const std::size_t hash = skip_spaces(text_, colon + 1);

		if (byte_at(hash) != '#') {
			return {};
		}

		const int after = byte_at(hash + 1);

		if (after == -1 || std::string_view{"\t\n #,]}"}.contains(static_cast<char>(after))) {
			return {};
		}

		std::size_t end = hash + 2;
		const std::size_t limit = std::min(text_.size(), hash + max_diagnosed_length);

		while (end < limit && !std::string_view{"\t\n ,]}"}.contains(text_[end])) {
			end++;
		}

		return concat(". “#” starts a comment, so a value that starts with “#” must be quoted", quoting_example(text_.substr(hash, end - hash)));
	}

	/*
	A value that failed to parse, in an entry whose key starts at `key_start` and whose `:` is at `colon`, is reported as what the entry was meant to be when that is clear. Otherwise, the original error stands.
	*/
	[[gnu::noinline]] constexpr void diagnose_bad_value(std::string_view key, std::size_t key_start, std::size_t colon) {
		// A key that contains a `:`, as in `12:30: 'lunch'`, ends at the first `:`, and the rest is read as the value.
		if (is_bare_key_character(byte_at(colon + 1))) {
			if (const std::size_t end = find_key_with_colon(key_start); end != std::string_view::npos) {
				fail(key_start, concat("A key that contains “:” must be quoted, as in '", abbreviate(text_.substr(key_start, end - key_start)), "'"));
				return;
			}
		}

		const std::size_t error_offset = error_.offset;
		position_ = colon + 1;
		bool is_value_on_next_line = false;

		// The trivia was skipped once already, so this cannot fail.
		static_cast<void>(skip_trivia(&is_value_on_next_line));

		const std::size_t value_start = position_;
		const std::string comment_hint = describe_comment_as_value(colon);

		if (is_value_on_next_line && diagnose_missing_value(value_start, key, key_start, comment_hint)) {
			return;
		}

		// A value that was left out at the end of the document or of an object.
		const int first = byte_at(value_start);

		if (!comment_hint.empty() && error_offset == value_start && (first == '}' || first == -1)) {
			fail(value_start, concat(error_.message, comment_hint));
		}
	}

	/*
	`entry = key ":" ws value`. The member goes on the member stack, and its object is built when the object closes.
	*/
	constexpr bool parse_entry(std::size_t depth) {
		const std::size_t key_start = position_;
		std::string key;

		if (!parse_key(key)) {
			return false;
		}

		if (peek() == '.') {
			return fail_dot_in_key(key_start);
		}

		if (peek() != ':') {
			return fail_missing_colon(key_start);
		}

		const std::size_t colon = position_;
		position_++;

		if (!skip_trivia()) {
			return false;
		}

		value item;

		if (!parse_value(depth + 1, item)) {
			diagnose_bad_value(key, key_start, colon);
			return false;
		}

		members_.emplace_back(std::move(key), std::move(item));
		key_offsets_.push_back(key_start);
		return true;
	}

	/*
	The index of the first member, in source order, whose key repeats an earlier one, among the members since `base`, or `npos`. That is the duplicate that a reader that checks each member as it reads reports first.
	*/
	constexpr std::size_t find_duplicate(std::size_t base) {
		const std::size_t count = members_.size() - base;

		if (count < 2) {
			return std::string_view::npos;
		}

		if (count <= linear_duplicate_limit) {
			const std::size_t duplicate = find_duplicate_key(std::span{members_}.subspan(base, count));

			return duplicate == std::string_view::npos ? duplicate : base + duplicate;
		}

		// Sorted by key and then by position, so equal keys are next to each other in source order, and the second of each pair is a duplicate.
		order_.resize(count);

		std::ranges::iota(order_, base);

		std::ranges::sort(order_, [&](std::size_t left, std::size_t right) {
			const int comparison = members_[left].key.compare(members_[right].key);
			return comparison < 0 || (comparison == 0 && left < right);
		});

		std::size_t first = std::string_view::npos;

		for (std::size_t index = 1; index < count; index++) {
			if (members_[order_[index - 1]].key == members_[order_[index]].key) {
				first = std::min(first, order_[index]);
			}
		}

		return first;
	}

	[[gnu::noinline]] constexpr bool fail_duplicate(std::size_t index) {
		return fail(key_offsets_[index], concat("Duplicate key ", describe_key(members_[index].key)));
	}

	/*
	Builds the object from the members since `base`, after checking for duplicate keys. A root object takes the member stack directly, because no enclosing object needs it afterward.
	*/
	constexpr bool finish_object(std::size_t base, std::size_t depth, value& output) {
		if (const std::size_t duplicate = find_duplicate(base); duplicate != std::string_view::npos) {
			fail_duplicate(duplicate);
			members_.resize(base);
			key_offsets_.resize(base);
			return false;
		}

		// Note: Not using move-iterator constructors because they added iterator/offset machinery without a consistent gain over this range construction.
		std::vector<member> members = depth == 1 ? std::move(members_) : std::vector<member>(std::from_range, std::span{members_}.subspan(base) | std::views::as_rvalue);
		members_.resize(base);
		key_offsets_.resize(base);
		output = object_access::make(std::move(members));
		return true;
	}

	/*
	An object that fails before it closes has members that were not checked for duplicates yet. They all come before the failure in the text, so a duplicate among them is the first error, and is reported instead, as a reader that checks each member as it reads would report it. Each open object checks its own members as the failure passes through it, from the innermost out, so the outermost duplicate wins, because its members come before the object that holds the failure. Returns `false`.
	*/
	[[gnu::noinline]] constexpr bool fail_open_object(std::size_t base) {
		if (const std::size_t duplicate = find_duplicate(base); duplicate != std::string_view::npos) {
			fail_duplicate(duplicate);
		}

		members_.resize(base);
		key_offsets_.resize(base);
		return false;
	}

	/*
	Reported at the bracket that opens the level past the limit, as by the reference implementation.
	*/
	constexpr bool fail_too_deep() {
		error_ = {.code = soml::error_code::too_deep, .message = concat("The document is nested more than ", decimal_string(max_depth), " levels deep"), .offset = position_};
		return false;
	}

	/*
	After a member or an item: a comma, a line break, both, the closing bracket, or an error. A comma must be on the line of the item before it. `start` is where the container opened, where an unterminated one is reported.
	*/
	constexpr bool skip_separator(char closing, std::size_t start) {
		const std::size_t item_end = position_;
		bool has_crossed_line_break = false;

		if (!skip_trivia(&has_crossed_line_break)) {
			return false;
		}

		const int character = peek();
		const bool is_object = closing == '}';

		if (character == -1) {
			return fail(start, is_object ? "Unterminated object: expected “}”" : "Unterminated array: expected “]”");
		}

		if (character == ',') {
			if (has_crossed_line_break) {
				return fail(position_, "A comma must be on the same line as the item before it. The line break already separates the items, so remove the comma");
			}

			position_++;
			return skip_trivia();
		}

		if (character == closing || has_crossed_line_break) {
			return true;
		}

		// Without a line break that separates, a line break in the gap is inside a block comment.
		const bool has_comment_line_break = text_.substr(item_end, position_ - item_end).contains('\n');
		return fail(position_, concat("Expected “,”, a line break, or “", std::string_view{&closing, 1}, "” after ", is_object ? "an object member" : "an array item", ", but found ", describe_here(), has_comment_line_break ? std::string_view{". A line break inside a block comment does not separate items"} : slash_comment_hint()));
	}

	/*
	The members go on the shared member stack while the object is open, and into an exact-size vector when a nested object closes, after the duplicate check. A root object keeps the stack's storage.
	*/
	constexpr bool parse_object(std::size_t depth, value& output) {
		if (depth > max_depth) {
			return fail_too_deep();
		}

		const std::size_t start = position_;
		const std::size_t base = members_.size();
		position_++;

		if (!skip_trivia()) {
			return false;
		}

		for (;;) {
			if (peek() == '}') {
				position_++;
				return finish_object(base, depth, output);
			}

			if (is_at_end()) {
				fail(start, "Unterminated object: expected “}”");
				return fail_open_object(base);
			}

			if (!parse_entry(depth) || !skip_separator('}', start)) {
				return fail_open_object(base);
			}
		}
	}

	constexpr bool parse_array(std::size_t depth, value& output) {
		if (depth > max_depth) {
			return fail_too_deep();
		}

		const std::size_t start = position_;
		const std::size_t base = items_.size();
		position_++;

		if (!skip_trivia()) {
			return false;
		}

		for (;;) {
			if (peek() == ']') {
				position_++;
				array items = depth == 1 ? std::move(items_) : array(std::from_range, std::span{items_}.subspan(base) | std::views::as_rvalue);
				items_.resize(base);
				output = std::move(items);
				return true;
			}

			if (is_at_end()) {
				return fail(start, "Unterminated array: expected “]”");
			}

			value item;

			if (!parse_value(depth + 1, item)) {
				return false;
			}

			items_.push_back(std::move(item));

			if (!skip_separator(']', start)) {
				return false;
			}
		}
	}

	/*
	A character that cannot follow the value that starts at `start`. `is_number` is whether the value is an int or a float written with digits.
	*/
	[[gnu::noinline]] constexpr bool fail_after_value(std::size_t start, bool is_number) {
		const int character = peek();
		const int second = peek(1);

		// Go writes microseconds as `µs`, with the micro sign, U+00B5, or the Greek letter mu, U+03BC.
		if (is_number && ((character == 0xC2 && second == 0xB5) || (character == 0xCE && second == 0xBC)) && peek(2) == 's') {
			const std::string_view number = text_.substr(start, position_ - start);
			return fail(position_, concat("The unit for microseconds is written us", number.size() <= max_diagnosed_length && is_valid_value(concat(number, "us")) ? concat(", as in ", abbreviate(number), "us") : std::string{}));
		}

		// A `''` inside a '...' string, as SQL and YAML escape a quote, ends the string.
		if (character == '\'' && byte_at(start) == '\'') {
			return fail(position_, "There is no '' escape in a '...' string. Write a string that contains ' as \"...\", as in \"it's\"");
		}

		return fail(position_, concat("Unexpected ", describe_here(), " after a value"));
	}

	/*
	A number followed by a space and a word, such as `512 MiB` or `10 seconds`, which is an error anyway, is reported as a unit after a space. A word followed by more than spaces, a separator, or a comment is left to the general errors, because it may be a key, as in `a: 1 b: 2`, or a sentence, and a keyword after a number is a missing comma. Returns `false` when it reports one.
	*/
	[[gnu::noinline]] constexpr bool check_unit_after_space(std::size_t start) {
		const std::size_t unit_start = skip_spaces(text_, position_);
		std::size_t unit_end = unit_start;

		while (unit_end < text_.size() && is_letter(text_[unit_end])) {
			unit_end++;
		}

		const std::string_view unit = text_.substr(unit_start, unit_end - unit_start);
		if (unit.empty() || !is_value_end_at(skip_spaces(text_, unit_end)) || unit == "true" || unit == "false" || unit == "null" || unit == "infinity") {
			return true;
		}

		const std::string_view number = text_.substr(start, position_ - start);
		const std::string number_example = abbreviate(number);
		const bool is_unit = duration_unit::find(unit) != nullptr;
		const std::string duration = is_unit && number.size() <= max_diagnosed_length && is_valid_value(concat(number, unit)) ? concat(number_example, unit) : std::string{"10s"};
		return fail(unit_start, concat("A unit cannot follow a number after a space. Write a duration without the space, as in ", duration, ", and anything else, such as a size, as a string, as in '", abbreviate(concat(number_example, " ", abbreviate(unit))), "'"));
	}

	constexpr bool parse_value(std::size_t depth, value& output) {
		const int character = peek();

		if (character == '{') {
			return parse_object(depth, output);
		}

		if (character == '[') {
			return parse_array(depth, output);
		}

		return parse_scalar(output);
	}

	/*
	A value that is not an object or an array. It is kept out of the recursive `parse_value`, so that its temporaries do not make every level's stack frame larger.
	*/
	[[gnu::noinline]] constexpr bool parse_scalar(value& output) {
		const int character = peek();
		const std::size_t start = position_;
		// An int or a float written with digits.
		bool is_number = false;

		if (character == '\'' || character == '"') {
			std::string string;

			if (!parse_string(string)) {
				return false;
			}

			output = std::move(string);
		} else if (character == 't' && is_keyword("true")) {
			output = true;
		} else if (character == 'f' && is_keyword("false")) {
			output = false;
		} else if (character == 'n' && is_keyword("null")) {
			output = nullptr;
		} else if (character == 'i' && is_keyword("infinity")) {
			output = std::numeric_limits<double>::infinity();
		} else if (character == '-' && is_keyword("-infinity")) {
			output = -std::numeric_limits<double>::infinity();
		} else if (is_digit(character) || character == '-') {
			if (!parse_number_like(output, is_number)) {
				return false;
			}
		} else {
			return fail_unexpected_value();
		}

		const int next = peek();

		if (next != -1 && !is_value_end(next)) {
			return fail_after_value(start, is_number);
		}

		if (is_number && is_space(next) && !check_unit_after_space(start)) {
			return false;
		}

		return true;
	}

	/*
	`bare-object = entry ( entry-sep entry )*`, where a separator is one or more line breaks.
	*/
	constexpr bool parse_bare_object(value& output) {
		const std::size_t start = position_;

		if (!parse_entry(1)) {
			// A whole document that is one scalar, such as `5` or an instant, fails as an entry. That failure is reported as what it is.
			error_state saved = std::move(error_);
			position_ = start;
			members_.clear();
			key_offsets_.clear();
			items_.clear();
			value bare;
			const bool is_bare_value = parse_value(1, bare) && skip_trivia() && is_at_end();
			error_ = std::move(saved);

			if (is_bare_value) {
				return fail(start, "A bare value is not a document. A document is an object or an array, so write it as `key: value` or `[value]`");
			}

			return false;
		}

		for (;;) {
			bool has_crossed_line_break = false;

			if (!skip_trivia(&has_crossed_line_break)) {
				return fail_open_object(0);
			}

			if (is_at_end()) {
				return finish_object(0, 1, output);
			}

			// A comma is explained wherever it is, also at the start of a line or after the last entry, where it would otherwise be a missing key.
			if (peek() == ',') {
				fail(position_, "Top-level entries are separated by line breaks, not commas. Use braces for a one-line object");
				return fail_open_object(0);
			}

			if (!has_crossed_line_break) {
				fail(position_, concat("Expected a line break before the next entry, but found ", describe_here(), slash_comment_hint()));
				return fail_open_object(0);
			}

			if (!parse_entry(1)) {
				return fail_open_object(0);
			}
		}
	}

	/*
	`document = ws ( object / array / bare-object ) ws EOF`. The form is decided by the first character after whitespace and comments, so no lookahead is needed.
	*/
	constexpr bool parse_document_into(value& output) {
		if (!check_characters() || !skip_trivia()) {
			return false;
		}

		document_start_ = position_;

		if (is_at_end()) {
			return fail(position_, "A document must contain an object or an array, but this one is empty");
		}

		const int character = peek();
		const bool success = character == '{' || character == '[' ? parse_value(1, output) : parse_bare_object(output);

		if (!success || !skip_trivia()) {
			return false;
		}

		if (!is_at_end()) {
			return fail(position_, concat("Unexpected ", describe_here(), " after the end of the document"));
		}

		return true;
	}
};

/*
Whether `text` is one valid value, so that a message only suggests a fix that works.
*/
constexpr bool is_valid_value(std::string_view text) {
	return parser{text}.is_one_value();
}

} // namespace soml::detail

namespace soml {

// MARK: Object

constexpr object::object() noexcept = default;
constexpr object::object(const object& other) = default;
constexpr object::object(object&& other) noexcept = default;
// Assignment copies or moves the source out first, so that an object can be assigned a part of itself.
constexpr object& object::operator=(const object& other) {
	if (this != &other) {
		std::vector<member> copy = other.members_;
		members_ = std::move(copy);
	}

	return *this;
}

constexpr object& object::operator=(object&& other) noexcept {
	if (this != &other) {
		std::vector<member> moved = std::move(other.members_);
		members_ = std::move(moved);
	}

	return *this;
}
constexpr object::~object() = default;

constexpr object::object(std::initializer_list<member> members) : object(std::vector<member>{members}) {}

constexpr object::object(std::vector<member> members) {
	// Strictly ordered keys and small unique batches can reuse their storage without sorting indexes.
	if (std::ranges::adjacent_find(members, std::greater_equal{}, &member::key) == members.end() || (members.size() <= detail::linear_duplicate_limit && detail::find_duplicate_key(members) == std::string_view::npos)) {
		members_ = std::move(members);
		return;
	}

	std::vector<std::size_t> order(members.size());

	std::ranges::iota(order, 0UZ);

	// Sorting indexes leaves the original order intact. Equal keys are sorted by position, so their first and last occurrences are known without moving values.
	std::ranges::sort(order, [&](std::size_t left, std::size_t right) {
		const int comparison = members[left].key.compare(members[right].key);
		return comparison < 0 || (comparison == 0 && left < right);
	});

	std::size_t unique_count = 0;

	for (std::size_t start = 0; start < order.size();) {
		std::size_t end = start + 1;

		while (end < order.size() && members[order[start]].key == members[order[end]].key) {
			end++;
		}

		const std::size_t first = order[start];

		if (end > start + 1) {
			members[first].value = std::move(members[order[end - 1]].value);
		}

		order[unique_count++] = first;
		start = end;
	}

	if (unique_count == members.size()) {
		members_ = std::move(members);
		return;
	}

	order.resize(unique_count);
	std::ranges::sort(order);

	// Each retained position is at or after its destination, so compaction can reuse the original allocation.
	for (std::size_t index = 0; index < unique_count; index++) {
		if (index != order[index]) {
			members[index] = std::move(members[order[index]]);
		}
	}
	members.resize(unique_count);
	members_ = std::move(members);
}

constexpr std::size_t object::size() const noexcept {
	return members_.size();
}

constexpr bool object::empty() const noexcept {
	return members_.empty();
}

constexpr void object::reserve(std::size_t capacity) {
	members_.reserve(capacity);
}

constexpr void object::clear() noexcept {
	members_.clear();
}

constexpr std::span<const member> object::members() const noexcept {
	return members_;
}

template<class Self>
constexpr auto object::find(this Self&& self, std::string_view key) noexcept {
	const auto found = std::ranges::find(self.object::members_, key, &member::key);
	return found == self.object::members_.end() ? nullptr : &found->value;
}

constexpr bool object::contains(std::string_view key) const noexcept {
	return find(key) != nullptr;
}

constexpr value& object::operator[](std::string_view key) {
	if (value* existing = find(key)) {
		return *existing;
	}

	return members_.emplace_back(std::string{key}, value{}).value;
}

// The value is taken by value, so that it can come from this object, as in `members.insert_or_assign("b", *members.find("a"))`, which adding a member could otherwise invalidate.
constexpr value& object::insert_or_assign(std::string_view key, soml::value value) {
	if (soml::value* existing = find(key)) {
		*existing = std::move(value);
		return *existing;
	}

	return members_.emplace_back(std::string{key}, std::move(value)).value;
}

constexpr bool object::erase(std::string_view key) {
	// Note: Not using std::erase_if because key can view a member's text that compaction would change; find the member before erasing.
	const auto found = std::ranges::find(members_, key, &member::key);

	if (found == members_.end()) {
		return false;
	}

	members_.erase(found);
	return true;
}

constexpr bool operator==(const object& left, const object& right) {
	if (left.size() != right.size()) {
		return false;
	}

	// Matching key order needs neither lookups nor sorting, including when a value differs.
	std::size_t matching = 0;
	for (; matching < left.size(); matching++) {
		if (left.members_[matching].key != right.members_[matching].key) {
			break;
		}
		if (left.members_[matching].value != right.members_[matching].value) {
			return false;
		}
	}
	if (matching == left.size()) {
		return true;
	}

	// Member order is not part of a value. Compare only the unmatched suffix: lookups for a small one, sorting for a large one.
	const auto left_remaining = left.members().subspan(matching);
	const auto right_remaining = right.members().subspan(matching);
	if (left_remaining.size() <= detail::linear_duplicate_limit) {
		return std::ranges::all_of(left_remaining, [&](const member& item) {
			const auto other = std::ranges::find(right_remaining, item.key, &member::key);
			return other != right_remaining.end() && other->value == item.value;
		});
	}

	std::vector<const member*> sorted;
	sorted.reserve(left_remaining.size() + right_remaining.size());

	for (const member& item : left_remaining) {
		sorted.push_back(&item);
	}
	for (const member& item : right_remaining) {
		sorted.push_back(&item);
	}
	const auto storage = std::span{sorted};
	const auto left_sorted = storage.first(left_remaining.size());
	const auto right_sorted = storage.last(right_remaining.size());

	if (!std::ranges::is_sorted(left_sorted, {}, &member::key)) {
		std::ranges::sort(left_sorted, {}, &member::key);
	}
	if (!std::ranges::is_sorted(right_sorted, {}, &member::key)) {
		std::ranges::sort(right_sorted, {}, &member::key);
	}

	for (std::size_t index = 0; index < left_sorted.size(); index++) {
		if (left_sorted[index]->key != right_sorted[index]->key || left_sorted[index]->value != right_sorted[index]->value) {
			return false;
		}
	}

	return true;
}

// MARK: Value

template<class Self>
constexpr auto value::find(this Self&& self, std::string_view key) noexcept {
	auto* members = self.value::template get_if<object>();
	return members == nullptr ? nullptr : members->find(key);
}

constexpr view value::operator[](std::string_view key) const noexcept {
	return find(key);
}

constexpr view value::operator[](std::size_t index) const noexcept {
	const array* items = get_if<array>();
	return items != nullptr && index < items->size() ? view{&(*items)[index]} : view{};
}

constexpr std::size_t value::size() const noexcept {
	// Note: Not using std::visit because it would throw on a valueless variant and terminate this noexcept function; get_if preserves the zero-size result.
	if (const array* items = get_if<array>()) {
		return items->size();
	}

	if (const object* members = get_if<object>()) {
		return members->size();
	}

	return 0;
}

constexpr bool operator==(const value& left, const value& right) {
	// `0.0 == -0.0` holds for doubles, which is the spec's equality, and NaN is not a SOML value.
	return left.storage_ == right.storage_;
}

template<class Duration>
	requires detail::chrono_representation<typename Duration::rep>
constexpr std::optional<std::chrono::sys_time<Duration>> instant::to_sys_time() const noexcept {
	if (!is_valid()) {
		return std::nullopt;
	}

	if constexpr (std::chrono::treat_as_floating_point_v<typename Duration::rep>) {
		// Combine the signed parts before floating conversion, so a fraction just before the epoch does not disappear through cancellation.
		const detail::int128 total = (static_cast<detail::int128>(seconds) * 1'000'000'000) + nanoseconds;
		return std::chrono::sys_time<Duration>{std::chrono::duration_cast<Duration>(std::chrono::duration<long double, std::nano>{static_cast<long double>(total)})};
	} else {
		// The count is nanoseconds × num / den, with the ratio in lowest terms, so it is whole exactly when that product is a multiple of den.
		// Note: Not using std::chrono::duration_cast because it truncates fractional ticks and does not check intermediate multiplication overflow.
		using ratio = std::ratio_divide<std::nano, typename Duration::period>;
		const detail::int128 total = (static_cast<detail::int128>(seconds) * 1'000'000'000) + nanoseconds;
		detail::int128 scaled = 0;

		if (__builtin_mul_overflow(total, static_cast<detail::int128>(ratio::num), &scaled) || scaled % ratio::den != 0) {
			return std::nullopt;
		}

		const detail::int128 count = scaled / ratio::den;

		if (!detail::fits_integer<typename Duration::rep>(count)) {
			return std::nullopt;
		}

		return std::chrono::sys_time<Duration>{Duration{static_cast<typename Duration::rep>(count)}};
	}
}

// MARK: Errors

constexpr std::string error::description() const {
	// Note: Not using std::format_to because it is not constexpr in the Apple Clang 21 and GCC 16 standard libraries.
	std::string output;
	// Two size_t positions and their labels fit within 64 bytes.
	output.reserve(path.size() + message.size() + (path.empty() ? 0 : 2) + (line > 0 ? 64 : 0));

	if (!path.empty()) {
		detail::append_error_text(output, path);
		output += ": ";
	}

	detail::append_error_text(output, message);

	if (line > 0) {
		output += " at line ";
		detail::append_integer(output, line);
		output += ", column ";
		detail::append_integer(output, column);
	}

	return output;
}

constexpr std::string error::code_frame(std::string_view text) const {
	constexpr std::size_t context_lines = 2;

	if (line == 0 || offset > text.size()) {
		return {};
	}

	// The error line and up to two lines before it.
	std::array<std::size_t, context_lines + 1> line_starts{};
	std::size_t line_count = 0;
	line_starts[line_count++] = text.substr(0, offset).rfind('\n') + 1;

	while (line_count < line_starts.size() && line_starts[line_count - 1] > 0) {
		const std::size_t previous_end = line_starts[line_count - 1] - 1;
		line_starts[line_count++] = text.substr(0, previous_end).rfind('\n') + 1;
	}

	const auto context = std::span{line_starts}.first(line_count);
	// Note: Not using indexed reverse_view because it regressed common short multi-line frames on Apple Clang 21.
	std::ranges::reverse(context);
	const std::size_t first_line = line - (line_count - 1);
	const std::string last_number = detail::decimal_string(line);
	std::string output;
	for (std::size_t index = 0; index < line_count; index++) {
		const std::size_t number = first_line + index;
		const bool is_error_line = number == line;
		const std::size_t start = line_starts[index];

		// A long line is cut to 100 characters around the caret. Count only far enough beyond it to select the window and detect a trailing ellipsis.
		constexpr std::size_t window = 100;
		std::size_t line_length = 0;
		std::size_t caret = 0;
		for (std::size_t character = start; character < text.size() && text[character] != '\n'; character += std::max<std::size_t>(detail::utf8_sequence_length(text, character), 1)) {
			line_length++;
			if (is_error_line && character < offset) {
				caret++;
			}
			if ((!is_error_line || character >= offset) && line_length > caret + window) {
				break;
			}
		}
		// Note: Not using std::clamp because it needed extra center-to-start arithmetic without a consistent performance gain.
		const std::size_t window_start = line_length <= window ? 0 : std::min(caret > window / 2 ? caret - (window / 2) : 0, line_length - window);
		const std::size_t window_end = std::min(line_length, window_start + window);

		// Invalid UTF-8, controls, and Unicode layout separators are shown as U+FFFD. Tabs are kept, so the caret lines up whatever the tab width is.
		std::string line_text = window_start > 0 ? "…" : "";
		std::string padding = window_start > 0 ? " " : "";
		line_text.reserve((window_end - window_start) + (window_start > 0 ? 3 : 0) + (window_end < line_length ? 3 : 0));
		padding.reserve(std::min(caret, window_end) - window_start + (window_start > 0 ? 1 : 0));
		std::size_t position = 0;

		for (std::size_t character = start; position < window_end; position++) {
			const std::size_t valid_length = detail::utf8_sequence_length(text, character);
			const std::size_t length = std::max<std::size_t>(valid_length, 1);
			if (position < window_start) {
				character += length;
				continue;
			}
			const char32_t scalar = detail::decode_utf8(text, character, valid_length);

			if (position < caret) {
				padding.push_back(scalar == '\t' ? '\t' : ' ');
			}

			const bool is_unprintable = valid_length == 0 || (scalar < 0x20 && scalar != '\t') || scalar == 0x7F || detail::is_diagnostic_control(scalar);

			if (is_unprintable) {
				line_text += "�";
			} else {
				line_text += text.substr(character, length);
			}

			character += length;
		}

		if (window_end < line_length) {
			line_text += "…";
		}

		const std::string number_text = detail::decimal_string(number);

		if (!output.empty()) {
			output.push_back('\n');
		}

		output += is_error_line ? "> " : "  ";
		output.append(last_number.size() - number_text.size(), ' ');
		output += number_text;
		output += " |";

		if (!line_text.empty()) {
			output.push_back(' ');
			output += line_text;
		}

		if (is_error_line) {
			output += "\n  ";
			output.append(last_number.size(), ' ');
			output += " | ";
			output += padding;
			output.push_back('^');
		}
	}

	return output;
}

} // namespace soml

namespace soml::detail {

/*
Puts a path component in front of an error's path, as the error returns through each container.
*/
constexpr error inside(error failure, std::string component) {
	if (!failure.path.empty() && failure.path.front() != '[') {
		component.push_back('.');
	}

	// Note: Not using rvalue string concatenation because it regressed deeply nested paths on Apple Clang 21 and GCC 16.
	failure.path.insert(0, component);
	return failure;
}

inline constexpr std::size_t max_path_key_length = 200;

constexpr std::string path_key(std::string_view key) {
	return is_bare_key(key) ? abbreviate(key, max_path_key_length) : concat("[", json_quote(abbreviate(key, max_path_key_length)), "]");
}

constexpr std::string path_index(std::size_t index) {
	std::string output = "[";
	append_integer(output, index);
	output.push_back(']');
	return output;
}

// MARK: Writer

/*
Writes a value in multiline or compact layout. Everything the reader checks is checked again, and a failure names the path of the value it is about.
*/
template<bool OneLine = false>
class writer {
public:
	constexpr writer(std::string& output, bool is_canonical, std::string_view line_prefix = {}) noexcept :
		output_(output),
		is_canonical_(is_canonical),
		line_prefix_(line_prefix) {}

	constexpr bool write_document(const value& root) {
		if (const object* members = root.get_if<object>()) {
			return write_document(*members);
		}

		if (const array* items = root.get_if<array>()) {
			return write_document(*items);
		}

		return fail(error_code::invalid_value, concat("A document must be an object or an array, but this is ", soml::to_string(root.type())));
	}

	constexpr bool write_document(const object& members) {
		// A non-empty top-level object is written without braces, and its entries start at column 0, but it is still the first level.
		if (!members.empty()) {
			return write_members(members, 0, 1);
		}

		return write_root_container(nullptr, &members);
	}

	constexpr bool write_document(const array& items) {
		return write_root_container(&items, nullptr);
	}

	constexpr bool write_fragment(const value& item, std::size_t level) {
		return write_value(item, 0, level);
	}

	constexpr bool write_key(std::string_view key) {
		if (is_bare_key(key)) {
			output_ += key;
			return true;
		}

		return write_string(key);
	}

	constexpr soml::error take_error() {
		return std::move(error_);
	}

private:
	std::string& output_;
	bool is_canonical_;
	std::string_view line_prefix_;
	soml::error error_;

	constexpr bool write_root_container(const array* items, const object* members) {
		if (!write_container(items, members, 0, 1)) {
			return false;
		}

		output_.push_back('\n');
		return true;
	}

	[[gnu::noinline]] constexpr bool fail(error_code code, std::string message) {
		error_ = make_error(code, std::move(message));
		return false;
	}

	[[gnu::noinline]] constexpr bool fail_duplicate(std::string_view key) {
		return fail(error_code::invalid_value, concat("An object has the key ", describe_key(key), " more than once"));
	}

	/*
	Adds a path component in front of the error's path, as the failure returns through each container.
	*/
	[[gnu::noinline]] constexpr bool fail_inside(std::string component) {
		error_ = inside(std::move(error_), std::move(component));
		return false;
	}

	/*
	Writes a string, or fails when SOML cannot hold it.
	*/
	constexpr bool write_string(std::string_view text) {
		const string_kind kind = classify_string(text);

		if (kind == string_kind::carriage_return) {
			return fail(error_code::invalid_value, "A string holds a carriage return (U+000D), which SOML cannot represent");
		}

		if (kind == string_kind::invalid_utf8) {
			return fail(error_code::invalid_value, "A string is not valid UTF-8");
		}

		append_string(output_, text, kind);
		return true;
	}

	constexpr void indent(std::size_t depth) {
		if constexpr (!OneLine) {
			output_.append(depth, '\t');
		}
	}

	constexpr void newline() {
		if constexpr (!OneLine) {
			output_.push_back('\n');
			output_ += line_prefix_;
		}
	}

	constexpr void begin_entry(std::size_t index, std::size_t indentation) {
		if constexpr (OneLine) {
			if (index > 0) {
				output_ += ", ";
			}
		} else {
			indent(indentation);
		}
	}

	/*
	Writes the members of an object in the chosen layout, sorted by key in canonical form and in their order otherwise. Multiline entries use `indentation`. Containers in the members are at nesting level `level + 1`. A duplicate key is refused before any member is written.
	*/
	constexpr bool write_members(const object& members, std::size_t indentation, std::size_t level) {
		const std::span<const member> items = members.members();
		std::vector<const member*> sorted;
		// Canonical form needs sorted keys, as does duplicate detection in a large object. Strictly increasing keys need neither sorting nor scratch storage.
		const bool needs_sorting = ((is_canonical_ && items.size() > 1) || items.size() > linear_duplicate_limit) && std::ranges::adjacent_find(items, std::greater_equal{}, &member::key) != items.end();

		if (needs_sorting) {
			sorted.reserve(items.size());

			for (const member& item : items) {
				sorted.push_back(&item);
			}

			std::ranges::sort(sorted, {}, &member::key);

			for (std::size_t index = 1; index < sorted.size(); index++) {
				if (sorted[index - 1]->key == sorted[index]->key) {
					return fail_duplicate(sorted[index]->key);
				}
			}
		} else if (!is_canonical_ && items.size() <= linear_duplicate_limit) {
			if (const std::size_t duplicate = find_duplicate_key(items); duplicate != std::string_view::npos) {
				return fail_duplicate(items[duplicate].key);
			}
		}

		for (std::size_t index = 0; index < items.size(); index++) {
			const member& item = is_canonical_ && needs_sorting ? *sorted[index] : items[index];

			begin_entry(index, indentation);

			if (!write_key(item.key)) {
				return fail_inside(path_key(item.key));
			}

			output_ += ": ";

			if (!write_value(item.value, indentation, level + 1)) {
				return fail_inside(path_key(item.key));
			}

			newline();
		}

		return true;
	}

	/*
	Writes a scalar. It is kept out of the recursive `write_value`, so that the strings it builds do not make every level's stack frame larger.
	*/
	[[gnu::noinline]] constexpr bool write_scalar(const value& item) {
		switch (item.type()) {
			case type::null: {
				output_ += "null";
				return true;
			}

			case type::boolean: {
				output_ += *item.get_if<bool>() ? "true" : "false";
				return true;
			}

			case type::integer: {
				append_integer(output_, *item.get_if<std::int64_t>());
				return true;
			}

			case type::floating: {
				const double number = *item.get_if<double>();

				if (is_nan(number)) {
					return fail(error_code::invalid_value, "NaN is not a SOML value");
				}

				append_float(output_, number);
				return true;
			}

			case type::string: {
				return write_string(*item.get_if<std::string>());
			}

			case type::date: {
				const auto date = *item.get_if<soml::date>();
				if (!valid_date(date)) {
					return fail(error_code::invalid_value, "A date must exist in the proleptic Gregorian calendar in years 0001 to 9999");
				}
				append_date(output_, date);
				return true;
			}

			case type::instant: {
				const soml::instant instant = *item.get_if<soml::instant>();

				if (!instant.is_valid()) {
					return fail(error_code::invalid_value, "An instant is outside the years 0001 to 9999, or its nanoseconds are outside 0 to 999,999,999");
				}

				append_instant(output_, instant);
				return true;
			}

			case type::duration: {
				append_duration(output_, *item.get_if<soml::duration>());
				return true;
			}

			case type::array:
			case type::object: {
				break;
			}
		}

		return fail(error_code::invalid_value, "A value has an unknown type");
	}

	/*
	Writes a value after a key or at an item's indentation. `level` is the nesting level the value has when it is a container.
	*/
	constexpr bool write_value(const value& item, std::size_t indentation, std::size_t level) {
		const array* items = item.get_if<array>();
		const object* members = item.get_if<object>();

		if (items == nullptr && members == nullptr) {
			return write_scalar(item);
		}

		return write_container(items, members, indentation, level);
	}

	constexpr bool write_container(const array* items, const object* members, std::size_t indentation, std::size_t level) {
		if (level > max_depth) {
			return fail_too_deep();
		}

		output_.push_back(members != nullptr ? '{' : '[');

		if (members != nullptr ? !members->empty() : !items->empty()) {
			newline();

			if (members != nullptr) {
				if (!write_members(*members, indentation + 1, level)) {
					return false;
				}
			} else {
				for (std::size_t index = 0; index < items->size(); index++) {
					begin_entry(index, indentation + 1);

					if (!write_value((*items)[index], indentation + 1, level + 1)) {
						return fail_inside(path_index(index));
					}

					newline();
				}
			}

			indent(indentation);
		}

		output_.push_back(members != nullptr ? '}' : ']');
		return true;
	}

	constexpr bool fail_too_deep() {
		return fail(error_code::too_deep, concat("The value is nested more than ", decimal_string(max_depth), " levels deep, which is the most a SOML reader accepts"));
	}
};

template<class T>
	requires (std::same_as<T, value> || std::same_as<T, array> || std::same_as<T, object>)
constexpr std::expected<std::string, error> write_text(const T& document, bool is_canonical) {
	std::string output;
	writer writer{output, is_canonical};

	if (!writer.write_document(document)) {
		return std::unexpected(writer.take_error());
	}

	return output;
}

/*
Renders before appending, preserving the destination on invalid values and allocation failure, even when it is a string inside the document.
*/
template<class Root>
	requires (std::same_as<Root, value> || std::same_as<Root, array> || std::same_as<Root, object>)
constexpr std::expected<void, error> write_text(const Root& document, std::string& output, bool is_canonical) {
	auto rendered = write_text(document, is_canonical);
	if (!rendered) {
		return std::unexpected(std::move(rendered.error()));
	}
	// Note: Not using rvalue concatenation because it did not improve complete writes and made buffer ownership less direct.
	if (output.empty()) {
		output = std::move(*rendered);
	} else {
		output += *rendered;
	}
	return {};
}

} // namespace soml::detail

namespace soml {

// MARK: Reading and writing

constexpr std::expected<value, error> parse(std::string_view text) {
	return detail::parser{text}.parse_document();
}

constexpr std::expected<value, error> parse(const std::vector<char>& text) {
	return parse(std::string_view{text});
}

constexpr std::expected<value, error> parse(std::u8string_view text) {
	if consteval {
		std::string copy{std::from_range, text};
		return parse(std::string_view{copy});
	} else {
		// `char` may alias any object, so viewing UTF-8 code units as `char` is defined.
		return parse(std::string_view{reinterpret_cast<const char*>(text.data()), text.size()}); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
	}
}

inline std::expected<value, error> parse(std::span<const std::byte> bytes) {
	return parse(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()}); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

constexpr std::expected<std::string, error> write(const value& document) {
	return detail::write_text(document, false);
}

constexpr std::expected<std::string, error> write_canonical(const value& document) {
	return detail::write_text(document, true);
}

constexpr std::expected<void, error> write(const value& document, std::string& output) {
	return detail::write_text(document, output, false);
}

constexpr std::expected<void, error> write_canonical(const value& document, std::string& output) {
	return detail::write_text(document, output, true);
}

template<class Root>
	requires (std::same_as<Root, array> || std::same_as<Root, object>)
constexpr std::expected<std::string, error> write(const Root& document) {
	return detail::write_text(document, false);
}

template<class Root>
	requires (std::same_as<Root, array> || std::same_as<Root, object>)
constexpr std::expected<std::string, error> write_canonical(const Root& document) {
	return detail::write_text(document, true);
}

template<class Root>
	requires (std::same_as<Root, array> || std::same_as<Root, object>)
constexpr std::expected<void, error> write(const Root& document, std::string& output) {
	return detail::write_text(document, output, false);
}

template<class Root>
	requires (std::same_as<Root, array> || std::same_as<Root, object>)
constexpr std::expected<void, error> write_canonical(const Root& document, std::string& output) {
	return detail::write_text(document, output, true);
}

constexpr bool is_bare_key(std::string_view key) noexcept {
	return !key.empty() && detail::skip_bare_key(key, 0) == key.size();
}

} // namespace soml

// MARK: Source layout

namespace soml {
namespace detail {

/*
Byte ranges in a validated source document. Object children carry their key's range separately from the value range, so the same nodes serve formatting, editing, and decoding diagnostics.
*/
struct source_node {
	std::size_t start = 0;
	std::size_t end = 0;
	std::size_t key_start = std::string_view::npos;
	std::size_t key_end = 0;
	soml::type kind = soml::type::null;
	bool bare = false;
	bool block = false;
	std::vector<source_node> children;

	/*
	The start of the complete entry, including an object's key when present.
	*/
	constexpr std::size_t entry_start() const noexcept {
		return key_start == std::string_view::npos ? start : key_start;
	}

	/*
	Finds a diagnostic path in the parsed structure without parsing its quoted keys. An unresolved or ambiguous abbreviated path has no source node.
	*/
	constexpr const source_node* find(const value& parsed, std::string_view path) const {
		const source_node* current = this;
		const value* item = &parsed;

		while (!path.empty()) {
			const object* members = item->get_if<object>();
			const array* items = item->get_if<array>();
			const source_node* next = nullptr;
			const value* next_value = nullptr;
			std::size_t component_length = 0;

			if (items != nullptr) {
				if (!path.starts_with('[')) {
					return nullptr;
				}
				std::size_t index = 0;
				const auto [parsed_end, code] = std::from_chars(path.data() + 1, path.data() + path.size(), index);
				if (code != std::errc{} || parsed_end == path.data() + path.size() || *parsed_end != ']' || index >= items->size()) {
					return nullptr;
				}
				component_length = static_cast<std::size_t>(parsed_end - path.data()) + 1;
				next = &current->children[index];
				next_value = &(*items)[index];
			} else if (members != nullptr) {
				for (std::size_t index = 0; index < current->children.size(); index++) {
					const std::string& key = members->members()[index].key;
					if (!path.starts_with('[') && (key.empty() || path.front() != key.front())) {
						continue;
					}
					const bool is_bare = is_bare_key(key);
					if (path.starts_with('[') == is_bare) {
						continue;
					}
					std::string formatted;
					std::string_view component = key;
					if (key.size() > max_path_key_length || !is_bare) {
						formatted = path_key(key);
						component = formatted;
					}
					if (!path.starts_with(component)) {
						continue;
					}
					const std::string_view remaining = path.substr(component.size());
					if (!remaining.empty() && remaining.front() != '.' && remaining.front() != '[') {
						continue;
					}
					if (next != nullptr) {
						return nullptr;
					}
					next = &current->children[index];
					next_value = &members->members()[index].value;
					component_length = component.size();
				}
			}

			if (next == nullptr) {
				return nullptr;
			}
			path.remove_prefix(component_length);
			if (!path.empty() && path.front() != '.' && path.front() != '[') {
				return nullptr;
			}
			if (path.starts_with('.')) {
				path.remove_prefix(1);
			}
			current = next;
			item = next_value;
		}

		return current;
	}
};

struct source_comment {
	std::size_t start;
	std::size_t end;
	bool block;
};

struct source_tree {
	source_node root;
	std::vector<source_comment> comments;
};

/*
Records source ranges after the ordinary parser has validated the document. The parsed value supplies the structure, so this scanner neither decodes scalars nor duplicates grammar validation.
*/
class source_scanner {
public:
	constexpr explicit source_scanner(std::string_view text) : text_(text) {}

	constexpr source_tree scan(const value& parsed) {
		trivia();
		const bool bare = position_ == text_.size() || (text_[position_] != '{' && text_[position_] != '[');
		tree_.root = node(parsed, bare);
		trivia();
		return std::move(tree_);
	}

private:
	std::string_view text_;
	std::size_t position_ = 0;
	source_tree tree_;

	constexpr void trivia() {
		while (position_ < text_.size()) {
			const char character = text_[position_];
			if (character == ' ' || character == '\t') {
				position_ = skip_spaces(text_, position_);
			} else if (character == '\n' || character == ',') {
				position_++;
			} else if (character == '#') {
				const std::size_t start = position_;
				position_ = std::min(text_.find('\n', position_), text_.size());
				tree_.comments.push_back({start, position_, false});
			} else if (character == '/' && text_[position_ + 1] == '*') {
				const std::size_t start = position_;
				position_ = text_.find("*/", position_ + 2) + 2;
				tree_.comments.push_back({start, position_, true});
			} else {
				break;
			}
		}
	}

	constexpr void quoted(bool block) {
		const char quote = text_[position_];
		if (block) {
			std::size_t delimiter_length = 0;
			while (text_[position_ + delimiter_length] == quote) {
				delimiter_length++;
			}
			position_ = text_.find('\n', position_) + 1;
			while (true) {
				position_ = skip_spaces(text_, position_);
				std::size_t run = 0;
				while (position_ + run < text_.size() && text_[position_ + run] == quote) {
					run++;
				}
				if (run == delimiter_length) {
					position_ += delimiter_length;
					return;
				}
				position_ = text_.find('\n', position_) + 1;
			}
		}
		if (quote == '\'') {
			position_ = text_.find(quote, position_ + 1) + 1;
			return;
		}
		position_ = skip_string_characters(text_, position_ + 1, quote);
		while (text_[position_] != quote) {
			position_ = skip_string_characters(text_, position_ + 2, quote);
		}
		position_++;
	}

	constexpr source_node node(const value& parsed, bool bare = false) {
		source_node result;
		result.start = position_;
		result.kind = parsed.type();
		result.bare = bare;
		if (parsed.is_object() || parsed.is_array()) {
			if (!bare) {
				position_++;
			}
			trivia();
			if (const auto* members = parsed.get_if<object>()) {
				result.children.reserve(members->size());
				for (const auto& member : *members) {
					const std::size_t key_start = position_;
					if (text_[position_] == '\'' || text_[position_] == '"') {
						quoted(false);
					} else {
						position_ += member.key.size();
					}
					const std::size_t key_end = position_++;
					trivia();
					source_node child = node(member.value);
					child.key_start = key_start;
					child.key_end = key_end;
					result.children.push_back(std::move(child));
					trivia();
				}
			} else {
				const auto& items = *parsed.get_if<array>();
				result.children.reserve(items.size());
				for (const auto& item : items) {
					result.children.push_back(node(item));
					trivia();
				}
			}
			if (!bare) {
				position_++;
			}
		} else if (parsed.is_string()) {
			result.block = text_.substr(position_, 3) == "'''" || text_.substr(position_, 3) == "\"\"\"";
			quoted(result.block);
		} else {
			// Note: Not using find_first_of with the delimiter set because it regressed long-number and date formatting with libstdc++.
			while (position_ < text_.size() && !is_value_end(byte_of(text_[position_]))) {
				position_++;
			}
		}
		result.end = position_;
		return result;
	}
};

constexpr source_tree scan_source(std::string_view text, const value& parsed) {
	return source_scanner{text}.scan(parsed);
}

/*
Recovers a failed decoding's source position. Kept out of successful decoding calls, which need neither a source tree nor per-value location storage.
*/
[[gnu::noinline]] constexpr void locate_decode_error(error& failure, std::string_view text, const value& parsed) {
	const auto tree = scan_source(text, parsed);
	if (const source_node* node = tree.root.find(parsed, failure.path)) {
		failure.offset = failure.code == error_code::unknown_key && node->key_start != std::string_view::npos ? node->key_start : node->start;
		const auto position = locate(text, failure.offset);
		failure.line = position.line;
		failure.column = position.column;
	}
}

/*
Appends a block string with its nonblank lines at a new tab indentation, preserving their indentation relative to the closing delimiter and leaving blank content lines unchanged.
*/
inline void append_reindented_block_string(std::string& output, std::string_view text, std::size_t indentation) {
	const std::size_t closing_start = text.rfind('\n') + 1;
	const std::size_t old_indentation = skip_spaces(text, closing_start) - closing_start;
	for (std::size_t start = 0; start < text.size();) {
		const std::size_t end = std::min(text.find('\n', start), text.size());
		const std::string_view line = text.substr(start, end - start);
		// Validated nonblank lines already have the closing delimiter's whitespace prefix.
		if (start > 0 && skip_spaces(line, std::min(old_indentation, line.size())) < line.size()) {
			output.append(indentation, '\t');
			output += line.substr(old_indentation);
		} else {
			output += line;
		}
		if (end < text.size()) {
			output += '\n';
		}
		start = end + 1;
	}
}

class format_writer {
public:
	format_writer(std::string_view text, const source_tree& tree) : text_(text), tree_(tree) {
		output_.reserve(text.size() + 1);
	}

	std::string write() {
		const auto& root = tree_.root;
		const std::span<const source_node> items = root.bare ? std::span<const source_node>{root.children} : std::span<const source_node>{&root, 1};
		list(items, 0, text_.size(), 0, false);
		output_ += '\n';
		return std::move(output_);
	}

private:
	std::string_view text_;
	const source_tree& tree_;
	std::size_t next_line_break_ = 0;
	std::size_t comment_index_ = 0;
	std::string output_;

	// Queries follow the source traversal, so their start offsets never move backwards. The first query initializes the cache.
	bool same_line(std::size_t start, std::size_t end) {
		if (next_line_break_ <= start) {
			next_line_break_ = text_.find('\n', start);
		}
		return next_line_break_ >= end;
	}

	bool blank_line(std::size_t start, std::size_t end) const {
		const auto gap = text_.substr(start, end - start);
		for (std::size_t index = gap.find('\n'); index != std::string_view::npos; index = gap.find('\n', index)) {
			index = skip_spaces(gap, index + 1);
			if (index < gap.size() && gap[index] == '\n') {
				return true;
			}
		}
		return false;
	}

	void new_line(std::size_t level, bool blank = false) {
		if (!output_.empty()) {
			output_ += blank ? "\n\n" : "\n";
			output_.append(level, '\t');
		}
	}

	const source_comment* peek_comment(std::size_t end) const {
		return comment_index_ < tree_.comments.size() && tree_.comments[comment_index_].end <= end ? &tree_.comments[comment_index_] : nullptr;
	}

	void comment(const source_comment& item) {
		comment_index_++;
		const auto text = text_.substr(item.start, item.end - item.start);
		bool previous_empty = false;
		for (std::size_t start = 0; start < text.size();) {
			const std::size_t end = std::min(text.find('\n', start), text.size());
			auto line = text.substr(start, end - start);
			const auto last = line.find_last_not_of(" \t");
			line = line.substr(0, last + 1);
			if (!line.empty() || !previous_empty) {
				if (start > 0) {
					output_ += '\n';
				}
				output_ += line;
			}
			previous_empty = line.empty();
			start = end + 1;
		}
	}

	std::size_t find_comma(std::size_t start, std::size_t end) const {
		for (std::size_t index = skip_spaces(text_, start); index < end; index = skip_spaces(text_, index + 1)) {
			if (text_[index] == ',') {
				return index;
			}
			if (text_[index] == '#') {
				index = std::min(text_.find('\n', index), end);
			} else if (text_[index] == '/' && text_[index + 1] == '*') {
				index = text_.find("*/", index + 2) + 1;
			}
		}
		return std::string_view::npos;
	}

	void list(std::span<const source_node> items, std::size_t start, std::size_t end, std::size_t level, bool braced) {
		std::size_t previous_end = start;
		std::size_t item_end = std::string_view::npos;
		bool can_trail = braced;
		bool has_written = false;
		bool on_same_line = false;
		for (std::size_t position = 0; position <= items.size(); position++) {
			const bool has_item = position < items.size();
			const std::size_t next_start = has_item ? items[position].entry_start() : end;
			const std::size_t comma = braced ? find_comma(previous_end, next_start) : std::string_view::npos;
			const std::size_t last_break = text_.substr(previous_end, next_start - previous_end).rfind('\n');
			const std::size_t item_line_start = last_break == std::string_view::npos ? previous_end : previous_end + last_break + 1;
			while (const auto* next = peek_comment(next_start)) {
				const bool after_comma = comma != std::string_view::npos && next->start > comma;
				const bool before_item = has_item && next->block && (comma == std::string_view::npos || after_comma) && next->end >= item_line_start;
				const std::size_t line_end = after_comma && previous_end == item_end ? comma + 1 : previous_end;
				const bool trailing = can_trail && !on_same_line && !before_item && same_line(line_end, next->start);
				if (trailing || on_same_line) {
					output_ += ' ';
				} else {
					new_line(level, has_written && blank_line(previous_end, next->start));
				}
				comment(*next);
				has_written = has_written || !trailing;
				const auto* following = peek_comment(next_start);
				const std::size_t following_start = following == nullptr ? next_start : following->start;
				on_same_line = next->block && !trailing && (has_item || following != nullptr) && same_line(next->end, following_start);
				previous_end = next->end;
			}
			if (!has_item) {
				return;
			}
			if (on_same_line) {
				output_ += ' ';
			} else {
				new_line(level, has_written && blank_line(previous_end, next_start));
			}
			item(items[position], level);
			previous_end = items[position].end;
			item_end = previous_end;
			can_trail = true;
			has_written = true;
			on_same_line = false;
		}
	}

	bool separator(std::size_t start, std::size_t end, std::size_t level) {
		const bool on_same_line = same_line(start, end);
		if (on_same_line) {
			output_ += ' ';
		} else {
			new_line(level);
		}
		return on_same_line;
	}

	void item(const source_node& node, std::size_t level) {
		if (node.key_start == std::string_view::npos) {
			value(node, level);
			return;
		}
		// Note: Not using append(view, position, count) because it regressed small-object formatting with libc++.
		output_ += text_.substr(node.key_start, node.key_end - node.key_start);
		output_ += ':';
		if (peek_comment(node.start) == nullptr) {
			if (node.block) {
				new_line(level + 1);
				value(node, level + 1);
			} else {
				output_ += ' ';
				value(node, level);
			}
			return;
		}
		std::size_t previous_end = node.key_end + 1;
		std::size_t value_level = level;
		while (const auto* next = peek_comment(node.start)) {
			if (!separator(previous_end, next->start, level + 1)) {
				value_level = level + 1;
			}
			comment(*next);
			previous_end = next->end;
		}
		if (node.block) {
			new_line(level + 1);
			value_level = level + 1;
		} else {
			if (!separator(previous_end, node.start, level + 1)) {
				value_level = level + 1;
			}
		}
		value(node, value_level);
	}

	void one_line(const source_node& node, std::size_t level, char opening, char closing) {
		output_ += opening;
		std::size_t previous_end = node.start + 1;
		bool has_token = false;
		for (std::size_t position = 0; position <= node.children.size(); position++) {
			const bool has_item = position < node.children.size();
			const std::size_t next_start = has_item ? node.children[position].entry_start() : node.end - 1;
			const std::size_t comma = has_item ? find_comma(previous_end, next_start) : std::string_view::npos;
			bool wrote_comma = false;
			while (const auto* next = peek_comment(next_start)) {
				if (!wrote_comma && comma != std::string_view::npos && comma < next->start) {
					output_ += ',';
					wrote_comma = true;
				}
				if (has_token) {
					output_ += ' ';
				}
				comment(*next);
				has_token = true;
			}
			if (!has_item) {
				break;
			}
			if (comma != std::string_view::npos && !wrote_comma) {
				output_ += ", ";
			} else if (has_token) {
				output_ += ' ';
			}
			item(node.children[position], level);
			previous_end = node.children[position].end;
			has_token = true;
		}
		output_ += closing;
	}

	void value(const source_node& node, std::size_t level) {
		if (node.kind != type::object && node.kind != type::array) {
			const auto text = text_.substr(node.start, node.end - node.start);
			if (node.block) {
				append_reindented_block_string(output_, text, level);
			} else {
				output_ += text;
			}
			return;
		}
		const char opening = node.kind == type::object ? '{' : '[';
		const char closing = node.kind == type::object ? '}' : ']';
		if (node.children.empty() && peek_comment(node.end) == nullptr) {
			output_ += opening;
			output_ += closing;
		} else if (same_line(node.start, node.end)) {
			one_line(node, level, opening, closing);
		} else {
			output_ += opening;
			list(node.children, node.start + 1, node.end - 1, level + 1, true);
			new_line(level);
			output_ += closing;
		}
	}
};

} // namespace detail

/**
Formats a document while keeping comments, member order, literal spellings, and an unbraced root. Uses tabs, trims trailing whitespace and redundant blank lines, and ends with one LF. Inline containers stay inline; multiline containers get one item per line. Invalid input returns `parse`'s error.
*/
[[nodiscard]] inline std::expected<std::string, error> format(std::string_view text) {
	detail::source_tree tree;
	{
		auto parsed = parse(text);
		if (!parsed) {
			return std::unexpected(std::move(parsed.error()));
		}
		tree = detail::scan_source(text, *parsed);
	}
	return detail::format_writer{text, tree}.write();
}

} // namespace soml

// MARK: Editing

namespace soml {

/**
One object key or array index in an edit path. Keys are owned, so a path can be kept for later edits.
*/
using path_segment = std::variant<std::string, std::size_t>;

/**
Changes one value while preserving surrounding text, comments, and literal spellings. Adds missing object members and intermediate objects; an index at an array's size appends. Empty paths and paths through the wrong type fail.

`std::nullopt` removes a member or item; removing a missing value leaves the text unchanged. Compare the result with the input to detect a change. New values use the writer's spelling and keep a one-line parent on one line.
*/
[[nodiscard]] inline std::expected<std::string, error> edit(std::string_view text, std::span<const path_segment> path, const std::optional<value>& replacement);

/**
Edits using an inline path, as in `soml::edit(text, {"servers", std::size_t{0}, "port"}, 8080)`.
*/
[[nodiscard]] inline std::expected<std::string, error> edit(std::string_view text, std::initializer_list<path_segment> path, const std::optional<value>& replacement) {
	return edit(text, std::span<const path_segment>{path.begin(), path.size()}, replacement);
}

} // namespace soml

namespace soml::detail {

/*
Edits a single source range. Comment ownership follows the specification, using the source tree's sorted comment spans. Only the path being changed is traversed.
*/
class source_editor {
public:
	source_editor(std::string_view text, const source_tree& tree) : text_(text), tree_(tree) {}

	std::expected<std::string, error> run(const value& document, std::span<const path_segment> path, const std::optional<value>& replacement) {
		const source_node* container = &tree_.root;
		const value* current = &document;

		for (std::size_t position = 0; position < path.size(); position++) {
			std::size_t index = 0;
			const value* child = nullptr;
			const std::string* key = std::get_if<std::string>(&path[position]);

			if (const object* members = current->get_if<object>()) {
				if (key == nullptr) {
					return std::unexpected(make_error(error_code::type_mismatch, "An object needs a key, not an array index"));
				}

				// Note: Not using std::ranges::find plus iterator distance because it regressed common edits; this loop finds both the member and its index.
				for (const member& item : *members) {
					if (item.key == *key) {
						child = &item.value;
						break;
					}

					index++;
				}
			} else if (const array* items = current->get_if<array>()) {
				if (key != nullptr) {
					return std::unexpected(make_error(error_code::type_mismatch, "An array needs an index, not an object key"));
				}

				index = std::get<std::size_t>(path[position]);

				if (index < items->size()) {
					child = &(*items)[index];
				} else if (replacement && index != items->size()) {
					return std::unexpected(make_error(error_code::out_of_range, "An array item can only be added at its end"));
				}
			} else {
				return std::unexpected(make_error(error_code::type_mismatch, "An edit path cannot pass through a scalar"));
			}

			if (child == nullptr) {
				if (!replacement) {
					return std::string{text_};
				}

				if (position + 1 == path.size()) {
					return insert(*container, key, *replacement, position + 2);
				}

				if (!std::ranges::all_of(path.subspan(position + 1), [](const path_segment& segment) {
					return std::holds_alternative<std::string>(segment);
				})) {
					return std::unexpected(make_error(error_code::out_of_range, "A missing value is created as an object and cannot have an array index"));
				}

				value nested = *replacement;

				for (std::size_t next = path.size(); next > position + 1; next--) {
					object parent;
					parent.insert_or_assign(std::get<std::string>(path[next - 1]), std::move(nested));
					nested = std::move(parent);
				}

				return insert(*container, key, nested, position + 2);
			}

			const source_node& node = container->children[index];

			if (position + 1 == path.size()) {
				if (!replacement) {
					return remove(*container, node, index);
				}

				std::size_t start = node.start;
				std::string prefix;
				std::string_view indent = indentation(start);

				if (node.key_start != std::string_view::npos && node.block && skip_whitespace(node.key_end + 1) == start) {
					start = node.key_end + 1;
					prefix = " ";
					indent = indentation(node.key_start);
				}

				auto rendered = render(*replacement, position + 2, indent, is_one_line(*container));

				if (!rendered) {
					return std::unexpected(std::move(rendered.error()));
				}

				return splice(start, node.end, {prefix, *rendered});
			}

			inside_one_line_ = is_one_line(*container);
			container = &node;
			current = child;
		}

		std::unreachable();
	}

private:
	std::string_view text_;
	const source_tree& tree_;
	bool inside_one_line_ = false;
	mutable std::size_t next_line_break_ = 0;

	char at(std::size_t offset) const {
		return offset < text_.size() ? text_[offset] : '\0';
	}

	std::size_t spaces(std::size_t offset) const {
		return skip_spaces(text_, offset);
	}

	std::size_t spaces_back(std::size_t offset) const {
		while (offset > 0 && (at(offset - 1) == ' ' || at(offset - 1) == '\t')) {
			offset--;
		}

		return offset;
	}

	std::size_t skip_whitespace(std::size_t offset) const {
		offset = spaces(offset);
		while (at(offset) == '\n') {
			offset = spaces(offset + 1);
		}

		return offset;
	}

	const source_comment* comment_at(std::size_t offset) const {
		// Note: Not using equal_range because it also searches for an upper bound; this lookup only needs the first matching comment.
		const auto found = std::ranges::lower_bound(tree_.comments, offset, {}, &source_comment::start);
		return found != tree_.comments.end() && found->start == offset ? &*found : nullptr;
	}

	std::size_t line_trivia(std::size_t offset) const {
		for (;;) {
			offset = spaces(offset);
			const source_comment* comment = comment_at(offset);

			if (comment == nullptr) {
				return offset;
			}

			offset = comment->end;
		}
	}

	std::size_t trivia(std::size_t offset) const {
		offset = line_trivia(offset);

		while (at(offset) == '\n') {
			offset = line_trivia(offset + 1);
		}

		return offset;
	}

	std::optional<std::size_t> line_end(std::size_t offset) const {
		offset = line_trivia(offset);

		if (offset == text_.size() || at(offset) == '\n') {
			return offset;
		}

		return std::nullopt;
	}

	std::size_t line_start(std::size_t offset) const {
		return offset == 0 ? 0 : text_.rfind('\n', offset - 1) + 1;
	}

	std::size_t owned_start(std::size_t offset) const {
		for (;;) {
			const std::size_t end = spaces_back(offset);
			const auto found = std::ranges::lower_bound(tree_.comments, end, {}, &source_comment::end);

			if (found == tree_.comments.end() || found->end != end || !found->block) {
				return offset;
			}

			offset = found->start;
		}
	}

	std::size_t owned_end(std::size_t offset) const {
		for (;;) {
			const source_comment* comment = comment_at(spaces(offset));

			if (comment == nullptr) {
				return offset;
			}

			offset = comment->end;
		}
	}

	std::string_view indentation(std::size_t offset) const {
		std::size_t start = line_start(offset);

		for (;;) {
			const auto found = std::ranges::lower_bound(tree_.comments, start, {}, &source_comment::start);

			if (found == tree_.comments.begin() || (found - 1)->end <= start) {
				break;
			}

			start = line_start((found - 1)->start);
		}

		return text_.substr(start, spaces(start) - start);
	}

	bool is_one_line(const source_node& container) const {
		if (inside_one_line_) {
			return true;
		}
		if (container.bare || spaces(container.start + 1) >= container.end - 1) {
			return false;
		}
		// The edit path only moves into descendants, so container starts never move backwards.
		if (next_line_break_ <= container.start) {
			next_line_break_ = text_.find('\n', container.start);
		}
		return next_line_break_ >= container.end;
	}

	std::string splice(std::size_t start, std::size_t end, std::initializer_list<std::string_view> replacement) const {
		std::size_t size = text_.size() - (end - start);
		for (const auto part : replacement) {
			size += part.size();
		}
		std::string result;
		result.reserve(size);
		result.append(text_.substr(0, start));
		for (const auto part : replacement) {
			result.append(part);
		}
		result.append(text_.substr(end));
		return result;
	}

	/*
	The normal writer validates and renders the new value in the parent's layout.
	*/
	static std::expected<std::string, error> render(const value& item, std::size_t depth, std::string_view indent, bool one_line) {
		std::string output;

		const auto serialize = [&](auto serializer) -> std::expected<std::string, error> {
			// Also enforces the combined depth of the original document and the new value.
			if (!serializer.write_fragment(item, depth)) {
				return std::unexpected(serializer.take_error());
			}
			return std::move(output);
		};
		return one_line ? serialize(writer<true>{output, false, indent}) : serialize(writer{output, false, indent});
	}

	std::expected<std::string, error> insert(const source_node& container, const std::string* key, const value& item, std::size_t depth) const {
		std::string prefix;

		if (key != nullptr) {
			// Validate new keys with the same writer as ordinary object members.
			writer serializer{prefix, false};
			if (!serializer.write_key(*key)) {
				return std::unexpected(inside(serializer.take_error(), path_key(*key)));
			}

			prefix += ": ";
		}

		const bool one_line = is_one_line(container);
		std::size_t start;
		std::size_t end;
		std::string before;
		std::string after;
		std::string owned_indent;
		std::string_view indent;

		if (container.children.empty()) {
			const std::size_t closing = container.end - 1;

			if (one_line) {
				start = spaces_back(closing);
				end = closing;
				before = start > container.start + 1 ? " " : "";
			} else {
				const std::string_view outer = indentation(container.start);
				owned_indent = outer;
				owned_indent += '\t';
				indent = owned_indent;
				const std::size_t line = line_start(closing);

				if (line > container.start && spaces(line) == closing) {
					start = line;
					end = line;
					before = indent;
					after = "\n";
				} else {
					start = spaces_back(closing);
					end = closing;
					before = concat("\n", indent);
					after = "\n";
					after += outer;
				}
			}
		} else {
			const source_node& previous = container.children.back();
			const std::size_t previous_end = owned_end(previous.end);
			const std::size_t next = trivia(previous_end);
			const bool comma = at(next) == ',';
			const std::size_t after_comma = comma ? next + 1 : previous_end;

			if (const auto line = line_end(after_comma)) {
				start = *line;
				end = start;
				indent = indentation(previous.entry_start());
				before = concat("\n", indent);
			} else {
				start = comma ? after_comma : previous_end;
				end = start;
				indent = indentation(after_comma);
				before = comma ? " " : ", ";
				after = comma ? "," : "";
			}
		}

		auto rendered = render(item, depth, indent, one_line);

		if (!rendered) {
			return std::unexpected(std::move(rendered.error()));
		}

		return splice(start, end, {before, prefix, *rendered, after});
	}

	bool blank_line(std::size_t start) const {
		return at(spaces(start)) == '\n';
	}

	std::size_t skip_blank_lines(std::size_t offset) const {
		while (offset < text_.size()) {
			const std::size_t end = spaces(offset);
			if (at(end) != '\n') {
				break;
			}
			offset = end + 1;
		}

		return offset;
	}

	std::string remove(const source_node& container, const source_node& node, std::size_t index) const {
		const std::size_t node_start = node.entry_start();

		if (container.bare && container.children.size() == 1) {
			return splice(node_start, node.end, {"{}"});
		}

		const std::size_t owned = owned_start(node_start);
		const std::size_t end_owned = owned_end(node.end);
		const std::size_t next = spaces(end_owned);
		const bool comma = at(next) == ',';
		const std::size_t before = spaces_back(owned);
		const bool take_before = index + 1 == container.children.size() && at(trivia(node.end)) != ',' && before > 0 && at(before - 1) == ',';
		std::size_t start = take_before ? before - 1 : owned;
		std::size_t end = comma ? line_end(next + 1).value_or(next + 1) : end_owned;
		bool whole_lines = false;

		if (const auto line = line_end(end); line && spaces(line_start(start)) == start) {
			start = line_start(start);
			end = std::min(*line + 1, text_.size());
			whole_lines = true;
		} else {
			end = spaces(end);

			if (end == text_.size() || at(end) == '\n' || at(end) == '}' || at(end) == ']') {
				start = spaces_back(start);
			}

			if (take_before && comment_at(end) != nullptr) {
				end = spaces_back(end);
			}
		}

		if (whole_lines) {
			const bool blank_before = start > 0 && blank_line(line_start(start - 1));
			const bool blank_after = end < text_.size() && blank_line(end);
			const std::size_t following = skip_blank_lines(end);
			const bool closing = following == text_.size() || (!container.bare && spaces(following) == container.end - 1);
			const auto opening_line = container.bare ? std::optional<std::size_t>{0} : line_end(container.start + 1);
			const std::size_t content = opening_line ? skip_blank_lines(*opening_line + (container.bare ? 0 : 1)) : std::string_view::npos;

			if (blank_before && (blank_after || closing)) {
				start = line_start(start - 1);
			} else if (blank_after && start == content) {
				end = text_.find('\n', end) + 1;
			}
		}

		if (container.children.size() == 1 && !container.bare) {
			const auto first = std::ranges::upper_bound(tree_.comments, container.start, {}, &source_comment::start);
			const auto last = std::ranges::lower_bound(first, tree_.comments.end(), container.end, {}, &source_comment::start);
			const bool comment_left = std::ranges::any_of(first, last, [&](const source_comment& comment) {
				return comment.start < start || comment.start >= end;
			});

			if (!comment_left) {
				return splice(container.start + 1, container.end - 1, {});
			}
		}

		return splice(start, end, {});
	}
};

} // namespace soml::detail

namespace soml {

inline std::expected<std::string, error> edit(std::string_view text, std::span<const path_segment> path, const std::optional<value>& replacement) {
	auto document = parse(text);

	if (!document) {
		return std::unexpected(std::move(document.error()));
	}
	if (path.empty()) {
		return std::unexpected(detail::make_error(error_code::invalid_value, "An edit path must not be empty"));
	}
	if (replacement && path.size() > max_depth) {
		return std::unexpected(detail::make_error(error_code::too_deep, "The edit path exceeds the nesting limit"));
	}

	const auto tree = detail::scan_source(text, *document);
	return detail::source_editor{text, tree}.run(*document, path, replacement);
}

} // namespace soml

// MARK: Typed mapping

namespace soml {

#if defined(__cpp_impl_reflection)

/**
An annotation that gives a struct member another key, for decoding and encoding with static reflection:

```
struct server {
	[[=soml::key("max-connections")]] int max_connections = 100;
};
```
*/
struct key_annotation {
	const char* name;
	std::size_t size;
};

/**
Gives a reflected struct member this exact key.
*/
[[nodiscard]] consteval key_annotation key(std::string_view name) {
	return {std::define_static_string(name), name.size()};
}

/**
An annotation for a struct whose member names become keys with `-` instead of `_`, as SOML documents usually spell them, so `max_connections` is read from `max-connections`:

```
struct [[=soml::kebab_case]] server {
	int max_connections = 100;
};
```
*/
struct kebab_case_annotation {
	friend constexpr bool operator==(const kebab_case_annotation&, const kebab_case_annotation&) = default;
};

inline constexpr kebab_case_annotation kebab_case{};

#endif

} // namespace soml

namespace soml::detail {

template<class T>
inline constexpr bool is_std_array = false;

template<class T, std::size_t Size>
inline constexpr bool is_std_array<std::array<T, Size>> = true;

template<class T>
concept has_fields = !std::same_as<std::remove_cvref_t<decltype(soml::fields<T>)>, std::nullptr_t>;

template<class T>
concept has_enum_names = std::is_enum_v<T> && !std::same_as<std::remove_cvref_t<decltype(soml::enum_names<T>)>, std::nullptr_t>;


inline constexpr bool has_reflection =
#if defined(__cpp_impl_reflection)
	true;
#else
	false;
#endif

template<class T>
concept is_sequence = std::ranges::range<T> && !std::same_as<T, std::string> && !std::same_as<T, array> && !is_std_array<T> && requires(T& container, typename T::value_type item) {
	container.push_back(std::move(item));
};

template<class T>
concept is_string_map = std::ranges::range<T> && std::constructible_from<typename T::key_type, std::string> && std::convertible_to<const typename T::key_type&, std::string_view> && requires(T& map, typename T::key_type key, typename T::mapped_type item) {
	{ map.insert_or_assign(std::move(key), std::move(item)).second } -> std::convertible_to<bool>;
};

#if defined(__cpp_impl_reflection)
// A struct with a base class is left out, because its members do not include the base's members.
template<class T>
concept is_reflectable_struct = std::is_class_v<T> && std::is_aggregate_v<T> && !is_std_array<T> && !is_sequence<T> && !is_string_map<T> && std::meta::bases_of(^^T, std::meta::access_context::unchecked()).empty();
#else
template<class T>
concept is_reflectable_struct = false;
#endif

/*
Whether each mapped or reflected field can receive its decoded value. Only assignment is checked, so recursive structs remain supported.
*/
template<class Struct>
consteval bool has_assignable_fields() {
	// Note: Not using std::assignable_from because it requires assignment to return a reference; decoding supports void-returning move assignment.
	if constexpr (has_fields<Struct>) {
		return std::apply([](const auto&... field) {
			return (std::is_move_assignable_v<std::remove_reference_t<decltype(std::declval<Struct&>().*(field.pointer))>> && ...);
		}, soml::fields<Struct>);
	} else {
#if defined(__cpp_impl_reflection)
		constexpr auto context = std::meta::access_context::current();
		bool assignable = true;
		template for (constexpr std::meta::info member : std::define_static_array(std::meta::nonstatic_data_members_of(^^Struct, context))) {
			assignable = assignable && std::is_move_assignable_v<typename [:std::meta::type_of(member):]>;
		}
		return assignable;
#else
		return false;
#endif
	}
}

/*
The article and name of a type for a message, as in `an int`.
*/
constexpr std::string_view describe_type(soml::type kind) noexcept {
	static constexpr std::array<std::string_view, 10> names{"null", "a bool", "an int", "a float", "a string", "a date", "an instant", "a duration", "an array", "an object"};
	const auto index = std::to_underlying(kind);
	return index < names.size() ? names[index] : "an unknown value";
}

/*
A value for a message: a scalar with its canonical spelling, as in `the string 'abc'`, and a collection by its type.
*/
constexpr std::string describe_value(const value& found) {
	// Note: Not using std::get because it retains additional checked-access code here; get_if already follows the selected alternative.
	if (found.is_null() || found.is_array() || found.is_object()) {
		return std::string{describe_type(found.type())};
	}

	std::string text = concat("the ", soml::to_string(found.type()), " ");

	switch (found.type()) {
		case soml::type::null:
		case soml::type::array:
		case soml::type::object: {
			break;
		}

		case soml::type::boolean: {
			text += *found.get_if<bool>() ? "true" : "false";
			break;
		}

		case soml::type::integer: {
			append_integer(text, *found.get_if<std::int64_t>());
			break;
		}

		case soml::type::floating: {
			if (is_nan(*found.get_if<double>())) {
				return "NaN";
			}

			append_float(text, *found.get_if<double>());
			break;
		}

		case soml::type::string: {
			const std::string string = abbreviate(*found.get_if<std::string>());
			const string_kind kind = classify_string(string);

			if (kind == string_kind::literal || kind == string_kind::escaped) {
				append_string(text, string, kind);
			} else {
				text += json_quote(string);
			}

			break;
		}

		case soml::type::date: {
			if (!valid_date(*found.get_if<soml::date>())) {
				return "an invalid date";
			}
			append_date(text, *found.get_if<soml::date>());
			break;
		}

		case soml::type::instant: {
			if (!found.get_if<soml::instant>()->is_valid()) {
				return "an invalid instant";
			}

			append_instant(text, *found.get_if<soml::instant>());
			break;
		}

		case soml::type::duration: {
			append_duration(text, *found.get_if<soml::duration>());
			break;
		}
	}

	return text;
}

constexpr std::unexpected<error> mismatch(const value& found, std::string_view expected) {
	return std::unexpected(make_error(error_code::type_mismatch, concat("Expected ", expected, ", but found ", describe_value(found))));
}

/*
The number of edits that turn one key into another: insertions, deletions, substitutions, and swaps of two neighbors.
*/
constexpr std::size_t edit_distance(std::string_view first, std::string_view second) {
	if (first == second) {
		return 0;
	}
	if (first.size() < second.size()) {
		std::swap(first, second);
	}
	if (second.empty()) {
		return first.size();
	}

	// Equal outer text contributes no edits; remove it before allocating the dynamic-programming rows.
	const auto common_prefix = std::ranges::mismatch(first, second);
	const std::size_t prefix_size = static_cast<std::size_t>(common_prefix.in1 - first.begin());
	first.remove_prefix(prefix_size);
	second.remove_prefix(prefix_size);
	while (!second.empty() && first.back() == second.back()) {
		first.remove_suffix(1);
		second.remove_suffix(1);
	}
	if (second.empty()) {
		return first.size();
	}

	// Only three rows are needed, with columns for the shorter key and one allocation for all rows.
	const std::size_t columns = second.size() + 1;
	std::vector<std::size_t> storage(3 * columns);
	auto before_previous = std::span{storage}.subspan(0, columns);
	auto previous = std::span{storage}.subspan(columns, columns);
	auto current = std::span{storage}.subspan(2 * columns, columns);

	for (std::size_t column = 0; column <= second.size(); column++) {
		previous[column] = column;
	}

	for (std::size_t row = 1; row <= first.size(); row++) {
		current[0] = row;

		for (std::size_t column = 1; column <= second.size(); column++) {
			const std::size_t cost = first[row - 1] == second[column - 1] ? 0 : 1;
			current[column] = std::min({previous[column] + 1, current[column - 1] + 1, previous[column - 1] + cost});

			if (row > 1 && column > 1 && first[row - 1] == second[column - 2] && first[row - 2] == second[column - 1]) {
				current[column] = std::min(current[column], before_previous[column - 2] + 1);
			}
		}

		// Note: Not using std::ranges::rotate because it needed a row array and regressed short keys with Apple Clang 21.
		std::swap(before_previous, previous);
		std::swap(previous, current);
	}

	return previous[second.size()];
}

constexpr std::unexpected<error> out_of_range(std::string message) {
	return std::unexpected(make_error(error_code::out_of_range, std::move(message)));
}

#if defined(__cpp_impl_reflection)
/*
The key of a struct member: its `soml::key` annotation, or its name, with `-` for `_` when the struct has the `soml::kebab_case` annotation.
*/
consteval std::string_view member_key(std::meta::info member) {
	// Note: Not using a range-for because libstdc++'s debug mode makes it invalid over the vector of reflections in a consteval function; use indexing.
	const std::vector<std::meta::info> keys = std::meta::annotations_of_with_type(member, ^^soml::key_annotation);

	if (!keys.empty()) {
		const auto annotation = std::meta::extract<soml::key_annotation>(keys[0]);
		return {annotation.name, annotation.size};
	}

	std::string name{std::meta::identifier_of(member)};

	if (!std::meta::annotations_of_with_type(std::meta::parent_of(member), ^^soml::kebab_case_annotation).empty()) {
		std::ranges::replace(name, '_', '-');
	}

	return std::define_static_string(name);
}
#endif

/*
Calls `visit(key, member)` for each field of a struct, in declaration order, until it returns `false`. Returns whether every call returned `true`.
*/
template<class Struct, class Visitor>
constexpr bool visit_fields(Struct& target, Visitor&& visit) {
	// Note: Not using std::invoke because its callable adaptation brought no consistent decoding benefit on the tested libraries.
	using plain = std::remove_const_t<Struct>;

	if constexpr (has_fields<plain>) {
		return std::apply([&](const auto&... field) {
			return (visit(field.key, target.*(field.pointer)) && ...);
		}, soml::fields<plain>);
	} else {
#if defined(__cpp_impl_reflection)
		constexpr auto context = std::meta::access_context::current();
		bool is_complete = true;

		template for (constexpr std::meta::info member : std::define_static_array(std::meta::nonstatic_data_members_of(^^plain, context))) {
			if (is_complete) {
				is_complete = visit(std::string_view{member_key(member)}, target.[:member:]);
			}
		}

		return is_complete;
#else
		static_assert(sizeof(Struct) == 0, "This struct needs `soml::fields`, because the compiler has no static reflection");
		return false;
#endif
	}
}

/*
Encodes fixed-size and dynamic sequences in iteration order, adding the failing item's index to conversion errors.
*/
template<class Sequence>
constexpr std::expected<value, error> sequence_to_value(Sequence&& source) {
	using item_type = typename std::remove_cvref_t<Sequence>::value_type;
	array items;

	if constexpr (std::ranges::sized_range<Sequence>) {
		items.reserve(std::ranges::size(source));
	}

	std::size_t index = 0;

	for (auto&& item : source) {
		std::expected<value, error> converted = soml::to_value<item_type>(std::forward_like<Sequence>(item));

		if (!converted) {
			return std::unexpected(inside(std::move(converted.error()), path_index(index)));
		}

		items.push_back(std::move(*converted));
		index++;
	}

	return value{std::move(items)};
}

} // namespace soml::detail

namespace soml {

/**
Whether `soml::from_value` can read a `T`. `soml::decode` additionally requires a movable or copyable result.
*/
template<class T>
concept decodable = requires(const value& source, const decode_options& options) {
	{ converter<T>::from_value(source, options) } -> std::same_as<std::expected<T, error>>;
};

/**
Whether `soml::encode` and `soml::to_value` can write a `T`.
*/
template<class T>
concept encodable = requires(const T& source) {
	{ converter<T>::to_value(source) } -> std::same_as<std::expected<value, error>>;
};

template<class T>
struct converter {};

template<>
struct converter<value> {
	template<class Source>
	static constexpr std::expected<value, error> from_value(Source&& source, const decode_options&)
		requires std::same_as<std::remove_cvref_t<Source>, value>
	{
		return std::forward<Source>(source);
	}

	template<class Source>
	static constexpr std::expected<value, error> to_value(Source&& source)
		requires std::same_as<std::remove_cvref_t<Source>, value>
	{
		return std::forward<Source>(source);
	}
};

template<class T>
	requires (std::same_as<T, array> || std::same_as<T, object> || std::same_as<T, bool> || std::same_as<T, std::string>)
struct converter<T> {
	template<class Source>
	static constexpr std::expected<T, error> from_value(Source&& source, const decode_options&)
		requires std::same_as<std::remove_cvref_t<Source>, value>
	{
		if (auto* stored = source.template get_if<T>()) {
			return std::forward_like<Source>(*stored);
		}
		constexpr auto kind = value{T{}}.type();
		return detail::mismatch(source, detail::describe_type(kind));
	}

	template<class Source>
	static constexpr std::expected<value, error> to_value(Source&& source)
		requires std::same_as<std::remove_cvref_t<Source>, T>
	{
		// Note: Not using in-place expected construction because rvalue-string timings were mixed on Apple Clang 21 despite improving GCC 16.
		return value{std::forward<Source>(source)};
	}
};

/**
SOML null, represented by `std::nullptr_t`.
*/
template<>
struct converter<std::nullptr_t> {
	static constexpr std::expected<std::nullptr_t, error> from_value(const value& source, const decode_options&) {
		if (!source.is_null()) {
			return detail::mismatch(source, "null");
		}

		return nullptr;
	}

	static constexpr std::expected<value, error> to_value(std::nullptr_t) {
		return value{nullptr};
	}
};

/**
Every integer type, range-checked both ways.
*/
template<std::integral Integer>
	requires (!std::same_as<Integer, bool> && !detail::is_character<Integer> && sizeof(Integer) <= sizeof(std::int64_t))
struct converter<Integer> {
	// The optional API needs the same range check without constructing a discarded diagnostic.
	static constexpr std::optional<Integer> try_from_value(const value& source) noexcept {
		const std::int64_t* integer = source.get_if<std::int64_t>();
		return integer != nullptr && std::in_range<Integer>(*integer) ? std::optional<Integer>{static_cast<Integer>(*integer)} : std::nullopt;
	}

	static constexpr std::expected<Integer, error> from_value(const value& source, const decode_options&) {
		const std::int64_t* integer = source.get_if<std::int64_t>();

		if (integer == nullptr) {
			return detail::mismatch(source, "an int");
		}

		const bool fits = std::in_range<Integer>(*integer);

		if (!fits) {
			std::string message = "The int ";
			detail::append_integer(message, *integer);
			message += " is outside the range of the type, ";
			detail::append_integer(message, static_cast<std::int64_t>(std::numeric_limits<Integer>::min()));
			message += " to ";
			detail::append_integer(message, static_cast<std::uint64_t>(std::numeric_limits<Integer>::max()));
			return detail::out_of_range(std::move(message));
		}

		return static_cast<Integer>(*integer);
	}

	static constexpr std::expected<value, error> to_value(Integer source) {
		if (!std::in_range<std::int64_t>(source)) {
			// Only an unsigned integer can be outside the range.
			return detail::out_of_range(detail::concat("The integer ", detail::decimal_string(static_cast<std::uint64_t>(source)), " is outside the 64-bit range of a SOML int"));
		}

		return value{static_cast<std::int64_t>(source)};
	}
};

/**
`float`, `double`, and `long double`. A floating-point value is only read from a SOML float, never from an int, because they are different types. Conversion to `float` uses ordinary C++ narrowing and may follow the current rounding mode.

Writing a `float` stores its shortest round-trip decimal as a SOML binary64 value, so `0.1f` writes as `0.1`. Constructing a `value` directly from a `float` instead preserves its exact widened value. A wider `long double` is narrowed to binary64; finite values outside its finite range, or nonzero values that round to zero, are refused.
*/
template<std::floating_point Float>
struct converter<Float> {
	// The optional API needs the same range check without constructing a discarded diagnostic.
	static constexpr std::optional<Float> try_from_value(const value& source) noexcept {
		const double* number = source.get_if<double>();

		if (number == nullptr) {
			return std::nullopt;
		}

		if constexpr (std::numeric_limits<Float>::max_exponent < std::numeric_limits<double>::max_exponent) {
			// A finite value rounds to the largest `Float` up to halfway to the next power of two, where it would round to infinity.
			constexpr double limit = [] {
				double half_step = 1.0;
				for (int exponent = 0; exponent < std::numeric_limits<Float>::max_exponent - std::numeric_limits<Float>::digits - 1; exponent++) {
					half_step *= 2.0;
				}
				return static_cast<double>(std::numeric_limits<Float>::max()) + half_step;
			}();
			const double magnitude = *number < 0.0 ? -*number : *number;

			if (magnitude >= limit && magnitude != std::numeric_limits<double>::infinity()) {
				return std::nullopt;
			}
		}

		return static_cast<Float>(*number);
	}

	static constexpr std::expected<Float, error> from_value(const value& source, const decode_options&) {
		if (!source.is_float()) {
			return detail::mismatch(source, source.is_int() ? "a float, written with a fraction or an exponent, as in 3.0" : "a float");
		}

		const std::optional<Float> result = try_from_value(source);

		if (!result) {
			return detail::out_of_range("The float is too large for the type");
		}

		return *result;
	}

	static constexpr std::expected<value, error> to_value(Float source) {
		if constexpr (std::numeric_limits<Float>::max_exponent > std::numeric_limits<double>::max_exponent) {
			const Float magnitude = source < 0 ? -source : source;

			if (magnitude > static_cast<Float>(std::numeric_limits<double>::max()) && magnitude != std::numeric_limits<Float>::infinity()) {
				return detail::out_of_range("The float is too large for a SOML binary64 value");
			}
		}

		const double number = static_cast<double>(source);

		if (detail::is_nan(number)) {
			return std::unexpected(detail::make_error(error_code::invalid_value, "NaN is not a SOML value"));
		}

		if (number == 0.0 && source != 0) {
			return detail::out_of_range("The float is too small for a SOML binary64 value");
		}

		if constexpr (std::same_as<Float, float>) {
			return value{shortest_float(source)};
		} else {
			return value{number};
		}
	}

private:
	/*
	Stores the shortest decimal that reads back as the same float, rounded to binary64. This is a typed-encoding choice; direct value construction still widens exactly.
	*/
	static constexpr double shortest_float(float source) {
		const auto bits = std::bit_cast<std::uint32_t>(source);
		const std::uint32_t magnitude = bits & 0x7FFF'FFFFU;

		if (magnitude == 0 || magnitude == 0x7F80'0000U) {
			return static_cast<double>(source);
		}

		if !consteval {
			if (detail::rounds_to_nearest()) {
				std::array<char, std::numeric_limits<float>::max_digits10 + 6> buffer;
				const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), source, std::chars_format::scientific);

				if (result) {
					// The standard converter accepts scientific exponents directly, including their plus signs and leading zeros.
					double number = 0;
					static_cast<void>(std::from_chars(buffer.data(), result.ptr, number, std::chars_format::scientific));
					return number;
				}
			}
		}

		detail::decimal digits;
		digits.assign_shortest<23, -127, 8>(magnitude);
		const double number = *digits.to_double();
		return source < 0 ? -number : number;
	}
};

/**
A string view can be written, but not read, because the string it would view is in a value that `soml::decode` does not keep. Use `value::as<std::string_view>()` to view a string in a value.
*/
template<>
struct converter<std::string_view> {
	static constexpr std::expected<value, error> to_value(std::string_view source) {
		return value{source};
	}
};

template<>
struct converter<date> {
	static constexpr std::expected<date, error> from_value(const value& source, const decode_options&) {
		if (const date* calendar = source.get_if<date>()) {
			return *calendar;
		}
		return detail::mismatch(source, "a date");
	}

	static constexpr std::expected<value, error> to_value(date source) {
		if (!detail::valid_date(source)) {
			return detail::out_of_range("The date must exist in the proleptic Gregorian calendar in years 0001 to 9999");
		}
		return value{source};
	}
};

template<>
struct converter<instant> {
	static constexpr std::expected<instant, error> from_value(const value& source, const decode_options&) {
		if (const instant* time = source.get_if<instant>()) {
			return *time;
		}

		return detail::mismatch(source, "an instant");
	}

	static constexpr std::expected<value, error> to_value(instant source) {
		if (!source.is_valid()) {
			return detail::out_of_range("The instant is outside the years 0001 to 9999, or its nanoseconds are outside 0 to 999,999,999");
		}

		return value{source};
	}
};

/**
Any `std::chrono::duration` with a floating-point count or a built-in integer count of at most 64 bits. Reading one with an integer count needs the duration to be a whole number of its unit, so `1.5s` is an error for `std::chrono::seconds`, and it must fit the count. Reading a floating-point count may lose precision. Writing rounds to the nearest nanosecond with ties to even, then checks the signed 64-bit nanosecond range. Floating-point counts are first scaled using `long double` arithmetic, which may lose precision. Custom periods whose nanosecond conversion ratios overflow `std::intmax_t` are unsupported.
*/
template<detail::chrono_representation Representation, class Period>
struct converter<std::chrono::duration<Representation, Period>> {
	using duration_type = std::chrono::duration<Representation, Period>;

	static constexpr std::optional<duration_type> try_from_value(const value& source) {
		const soml::duration* length = source.get_if<soml::duration>();
		if (length == nullptr) {
			return std::nullopt;
		}
		const auto result = convert(*length);
		return result ? std::optional{*result} : std::nullopt;
	}

	static constexpr std::expected<duration_type, error> from_value(const value& source, const decode_options&) {
		const soml::duration* length = source.get_if<soml::duration>();

		if (length == nullptr) {
			return detail::mismatch(source, "a duration");
		}

		return convert(*length).transform_error([](std::string_view message) {
			return detail::make_error(error_code::out_of_range, std::string{message});
		});
	}

private:
	// A failed optional conversion needs no owning error message.
	static constexpr std::expected<duration_type, std::string_view> convert(soml::duration length) {
		if constexpr (std::chrono::treat_as_floating_point_v<Representation>) {
			return std::chrono::duration_cast<duration_type>(length);
		} else {
			// The count in the target unit is nanoseconds × num / den, where the ratio is in lowest terms, so it is whole exactly when the nanoseconds are a multiple of den.
			using ratio = std::ratio_divide<std::nano, Period>;
			const std::int64_t nanoseconds = length.count();

			if (nanoseconds % ratio::den != 0) {
				return std::unexpected("The duration is not a whole number of the type's unit");
			}

			const detail::int128 count = static_cast<detail::int128>(nanoseconds / ratio::den) * ratio::num;
			if (!detail::fits_integer<Representation>(count)) {
				return std::unexpected("The duration is outside the range of the type");
			}

			return duration_type{static_cast<Representation>(count)};
		}
	}

public:
	static constexpr std::expected<value, error> to_value(duration_type source) {
		if constexpr (std::chrono::treat_as_floating_point_v<Representation>) {
			const auto nanoseconds = std::chrono::duration<long double, std::nano>{source}.count();

			if (!(nanoseconds >= -9'223'372'036'854'775'809.0L && nanoseconds <= 9'223'372'036'854'775'808.0L)) {
				return detail::out_of_range("The duration is outside the 64-bit range of nanoseconds, about 292 years either way");
			}

			// Note: Not using std::chrono::round because it computes in the input type's precision, such as float. Take the whole part first because adding 0.5 can itself round where long double is double; choose the even count at exact ties and narrow only after rounding.
			detail::int128 whole = static_cast<detail::int128>(nanoseconds);
			const long double fraction = nanoseconds - static_cast<long double>(whole);
			const long double magnitude = fraction < 0 ? -fraction : fraction;

			if (magnitude > 0.5L || (magnitude == 0.5L && whole % 2 != 0)) {
				whole += fraction > 0 ? 1 : -1;
			}

			if (!detail::fits_integer<std::int64_t>(whole)) {
				return detail::out_of_range("The duration is outside the 64-bit range of nanoseconds, about 292 years either way");
			}

			return value{soml::duration{static_cast<std::int64_t>(whole)}};
		} else {
			using ratio = std::ratio_divide<Period, std::nano>;
			// Widen before scaling so rounding remains defined for large counts and periods.
			const detail::int128 scaled = static_cast<detail::int128>(source.count()) * ratio::num;
			detail::int128 count = scaled / ratio::den;
			const detail::int128 remainder = scaled % ratio::den;
			const detail::int128 twice_remainder = (remainder < 0 ? -remainder : remainder) * 2;

			if (twice_remainder > ratio::den || (twice_remainder == ratio::den && count % 2 != 0)) {
				count += remainder > 0 ? 1 : -1;
			}

			if (!detail::fits_integer<std::int64_t>(count)) {
				return detail::out_of_range("The duration is outside the 64-bit range of nanoseconds, about 292 years either way");
			}

			return value{soml::duration{static_cast<std::int64_t>(count)}};
		}
	}
};

/**
A `std::chrono::sys_time`, such as `std::chrono::sys_seconds`, with a floating-point count or a built-in integer count of at most 64 bits. Reading an integer count requires an exact conversion; reading a floating-point count may lose precision. Writing requires an integer count and floors subnanosecond times to a nanosecond, as `soml::instant` does, then checks the SOML instant range. Custom periods whose nanosecond conversion ratios overflow `std::intmax_t` are unsupported.
*/
template<class Duration>
	requires detail::chrono_representation<typename Duration::rep>
struct converter<std::chrono::sys_time<Duration>> {
	static constexpr std::optional<std::chrono::sys_time<Duration>> try_from_value(const value& source) noexcept {
		const instant* time = source.get_if<instant>();
		return time == nullptr ? std::nullopt : time->template to_sys_time<Duration>();
	}

	static constexpr std::expected<std::chrono::sys_time<Duration>, error> from_value(const value& source, const decode_options&) {
		const instant* time = source.get_if<instant>();

		if (time == nullptr) {
			return detail::mismatch(source, "an instant");
		}

		if (const std::optional<std::chrono::sys_time<Duration>> result = time->template to_sys_time<Duration>()) {
			return *result;
		}

		return detail::out_of_range("The instant is outside the range of the type, or is not a whole number of its unit");
	}

	static constexpr std::expected<value, error> to_value(std::chrono::sys_time<Duration> source)
		requires (!std::chrono::treat_as_floating_point_v<typename Duration::rep>)
	{
		return converter<instant>::to_value(instant{source});
	}
};

/**
`std::nullopt` for null, and for a struct member that the document leaves out. Reading requires a move-constructible contained type.
*/
template<class T>
struct converter<std::optional<T>> {
	// Note: Not using std::make_optional because it added no clarity or consistent performance benefit; direct constructors keep nested engagement explicit.
	static constexpr std::optional<std::optional<T>> try_from_value(const value& source)
		requires decodable<T> && std::is_move_constructible_v<T>
	{
		if (source.is_null()) {
			return std::optional<std::optional<T>>{std::in_place, std::nullopt};
		}
		return source.template as<T>().transform([](T&& item) {
			return std::optional<T>{std::move(item)};
		});
	}

	template<class Source>
	static constexpr std::expected<std::optional<T>, error> from_value(Source&& source, const decode_options& options)
		requires std::same_as<std::remove_cvref_t<Source>, value> && decodable<T> && std::is_move_constructible_v<T>
	{
		if (source.is_null()) {
			return std::optional<T>{};
		}

		return soml::from_value<T>(std::forward<Source>(source), options).transform([](T&& item) {
			return std::optional<T>{std::move(item)};
		});
	}

	template<class Source>
	static constexpr std::expected<value, error> to_value(Source&& source)
		requires std::same_as<std::remove_cvref_t<Source>, std::optional<T>> && encodable<T>
	{
		if (!source) {
			return value{};
		}

		return soml::to_value(std::forward_like<Source>(*source));
	}
};

/**
`std::vector` and other sequences with `push_back`, from an array. Reading requires a default-constructible, move-constructible sequence with move-constructible elements; writing requires iteration over a const sequence.
*/
template<detail::is_sequence Sequence>
struct converter<Sequence> {
	using item_type = typename Sequence::value_type;

	template<class Source>
	static constexpr std::expected<Sequence, error> from_value(Source&& source, const decode_options& options)
		requires std::same_as<std::remove_cvref_t<Source>, value> && decodable<item_type> && std::default_initializable<Sequence> && std::is_move_constructible_v<Sequence> && std::is_move_constructible_v<item_type>
	{
		auto* items = source.template get_if<array>();

		if (items == nullptr) {
			return detail::mismatch(source, "an array");
		}

		Sequence result;

		if constexpr (requires { result.reserve(items->size()); }) {
			result.reserve(items->size());
		}

		for (std::size_t index = 0; index < items->size(); index++) {
			std::expected<item_type, error> item = soml::from_value<item_type>(std::forward_like<Source>((*items)[index]), options);

			if (!item) {
				return std::unexpected(detail::inside(std::move(item.error()), detail::path_index(index)));
			}

			result.push_back(std::move(*item));
		}

		return result;
	}

	template<class Source>
	static constexpr std::expected<value, error> to_value(Source&& source)
		requires std::same_as<std::remove_cvref_t<Source>, Sequence> && encodable<item_type> && std::ranges::input_range<const Sequence>
	{
		return detail::sequence_to_value(std::forward<Source>(source));
	}
};

/**
A `std::array`, from an array of exactly that many items. Reading requires default-constructible, move-assignable elements and a move-constructible array.
*/
template<class T, std::size_t Size>
struct converter<std::array<T, Size>> {
	template<class Source>
	static constexpr std::expected<std::array<T, Size>, error> from_value(Source&& source, const decode_options& options)
		requires std::same_as<std::remove_cvref_t<Source>, value> && decodable<T> && std::default_initializable<T> && std::is_move_assignable_v<T> && std::is_move_constructible_v<std::array<T, Size>>
	{
		auto* items = source.template get_if<array>();

		if (items == nullptr) {
			return detail::mismatch(source, "an array");
		}

		if (items->size() != Size) {
			return std::unexpected(detail::make_error(error_code::type_mismatch, detail::concat("Expected an array of ", detail::decimal_string(Size), " items, but found ", detail::decimal_string(items->size()))));
		}

		std::array<T, Size> result{};

		for (std::size_t index = 0; index < Size; index++) {
			std::expected<T, error> item = soml::from_value<T>(std::forward_like<Source>((*items)[index]), options);

			if (!item) {
				return std::unexpected(detail::inside(std::move(item.error()), detail::path_index(index)));
			}

			result[index] = std::move(*item);
		}

		return result;
	}

	template<class Source>
	static constexpr std::expected<value, error> to_value(Source&& source)
		requires std::same_as<std::remove_cvref_t<Source>, std::array<T, Size>> && encodable<T>
	{
		return detail::sequence_to_value(std::forward<Source>(source));
	}
};

/**
`std::map`, `std::unordered_map`, `std::flat_map`, and other maps with string keys, from an object. Reading requires a default-constructible, move-constructible map with move-constructible, move-assignable mapped values and fails if the map treats distinct keys as equivalent. A map is written in iteration order; writing requires iteration over a const map. Use `soml::encode_canonical` when the bytes matter.
*/
template<detail::is_string_map Map>
struct converter<Map> {
	using item_type = typename Map::mapped_type;

	// The keys must own their text, because the document they are read from is gone once decoding returns.
	template<class Source>
	static constexpr std::expected<Map, error> from_value(Source&& source, const decode_options& options)
		requires std::same_as<std::remove_cvref_t<Source>, value> && decodable<item_type> && std::same_as<typename Map::key_type, std::string> && std::default_initializable<Map> && std::is_move_constructible_v<Map> && std::is_move_constructible_v<item_type> && std::is_move_assignable_v<item_type>
	{
		auto* members = source.template get_if<object>();

		if (members == nullptr) {
			return detail::mismatch(source, "an object");
		}

		Map result;

		if constexpr (requires { result.reserve(members->size()); }) {
			if (!members->empty()) {
				result.reserve(members->size());
			}
		}

		for (auto& [key, item] : *members) {
			std::expected<item_type, error> converted = soml::from_value<item_type>(std::forward_like<Source>(item), options);

			if (!converted) {
				return std::unexpected(detail::inside(std::move(converted.error()), detail::path_key(key)));
			}

			// Note: Not using try_emplace because it is not required by the generic map contract; use insert_or_assign.
			if (!result.insert_or_assign(typename Map::key_type{key}, std::move(*converted)).second) {
				return std::unexpected(detail::inside(detail::make_error(error_code::out_of_range, "The target map treats two object keys as the same key"), detail::path_key(key)));
			}
		}

		return result;
	}

	template<class Source>
	static constexpr std::expected<value, error> to_value(Source&& source)
		requires std::same_as<std::remove_cvref_t<Source>, Map> && encodable<item_type> && std::ranges::input_range<const Map>
	{
		object members;

		if constexpr (std::ranges::sized_range<const Map>) {
			members.reserve(std::ranges::size(source));
		}

		for (auto&& [key, item] : source) {
			const std::string_view name = key;
			std::expected<value, error> converted = soml::to_value(std::forward_like<Source>(item));

			if (!converted) {
				return std::unexpected(detail::inside(std::move(converted.error()), detail::path_key(name)));
			}

			detail::object_access::members(members).emplace_back(std::string{name}, std::move(*converted));
		}

		return value{std::move(members)};
	}
};

/**
An enum, as the name of its value: from static reflection, or from `soml::enum_names`.
*/
template<class Enum>
	requires std::is_enum_v<Enum> && (detail::has_enum_names<Enum> || detail::has_reflection)
struct converter<Enum> {
	static constexpr std::optional<Enum> try_from_value(const value& source) noexcept {
		const std::string* name = source.get_if<std::string>();
		return name == nullptr ? std::nullopt : from_name(*name);
	}

	static constexpr std::expected<Enum, error> from_value(const value& source, const decode_options&) {
		const std::string* name = source.get_if<std::string>();

		if (name == nullptr) {
			return detail::mismatch(source, "a string");
		}

		if (const std::optional<Enum> item = from_name(*name)) {
			return *item;
		}

		return std::unexpected(detail::make_error(error_code::out_of_range, detail::concat("Unknown value ", detail::json_quote(detail::abbreviate(*name)), ". Expected one of: ", names_list())));
	}

	static constexpr std::expected<value, error> to_value(Enum source) {
		const std::optional<std::string_view> name = name_of(source);

		if (!name) {
			return detail::out_of_range("The enum value has no name");
		}

		return value{*name};
	}

private:
	/*
	Explicit and reflected enum metadata share the same pair-based lookup and diagnostic code.
	*/
	static constexpr const auto& names() noexcept {
		if constexpr (detail::has_enum_names<Enum>) {
			return (soml::enum_names<Enum>);
		} else {
#if defined(__cpp_impl_reflection)
			static constexpr auto reflected = [] consteval {
				constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^Enum));
				std::array<std::pair<Enum, std::string_view>, enumerators.size()> entries{};
				[[maybe_unused]] std::size_t index = 0;
				template for (constexpr std::meta::info enumerator : std::define_static_array(std::meta::enumerators_of(^^Enum))) {
					entries[index++] = {[:enumerator:], std::meta::identifier_of(enumerator)};
				}
				return entries;
			}();
			return (reflected);
#else
			static_assert(sizeof(Enum) == 0, "This enum needs `soml::enum_names`, because the compiler has no static reflection");
#endif
		}
	}

	/*
	The name of an enum value, or `std::nullopt` when it has no name.
	*/
	static constexpr std::optional<std::string_view> name_of(Enum item) noexcept {
		const auto found = std::ranges::find_if(names(), [item](const auto& entry) {
			return entry.first == item;
		});
		return found == std::ranges::end(names()) ? std::nullopt : std::optional<std::string_view>{found->second};
	}

	static constexpr std::optional<Enum> from_name(std::string_view name) noexcept {
		const auto found = std::ranges::find_if(names(), [name](const auto& entry) {
			return entry.second == name;
		});
		return found == std::ranges::end(names()) ? std::nullopt : std::optional<Enum>{found->first};
	}

	static constexpr std::string names_list() {
		std::size_t size = 0;
		// Note: Not using std::ranges::distance(names()) because it adds another traversal for unsized metadata; count in the required length pass.
		std::size_t count = 0;
		for (const auto& [item, name] : names()) {
			size += std::string_view{name}.size();
			count++;
		}
		std::string output;
		output.reserve(size + (count == 0 ? 0 : (count - 1) * 2));
		for (const auto& [item, name] : names()) {
			if (!output.empty()) {
				output += ", ";
			}
			output += name;
		}
		return output;
	}
};

/**
A struct, as an object with a member for each field: from `soml::fields`, or from static reflection for an aggregate. Reading requires a default-constructible, move-constructible struct with move-assignable fields. A member that the document leaves out keeps the field's default value.
*/
template<class Struct>
	requires detail::has_fields<Struct> || detail::is_reflectable_struct<Struct>
struct converter<Struct> {
	template<class Source>
	static constexpr std::expected<Struct, error> from_value(Source&& source, const decode_options& options)
		requires std::same_as<std::remove_cvref_t<Source>, value> && std::default_initializable<Struct> && std::is_move_constructible_v<Struct> && (detail::has_assignable_fields<Struct>())
	{
		auto* members = source.template get_if<object>();

		if (members == nullptr) {
			return detail::mismatch(source, "an object");
		}

		// Value-initialized, so that a field the document leaves out keeps its default member initializer.
		Struct result{};

		// Each member of the document looks up its field among the struct's fields, which is a short list known at compile time.
		for (auto& [key, item] : *members) {
			std::optional<error> failure;
			bool is_known = false;

			detail::visit_fields(result, [&](std::string_view field_key, auto& field) {
				if (field_key != key) {
					return true;
				}

				is_known = true;
				using field_type = std::remove_cvref_t<decltype(field)>;
				static_assert(decodable<field_type>, "soml cannot read a field of this struct. Specialize `soml::converter` for the field's type");
				std::expected<field_type, error> converted = soml::from_value<field_type>(std::forward_like<Source>(item), options);

				if (converted) {
					field = std::move(*converted);
				} else {
					failure = detail::inside(std::move(converted.error()), detail::path_key(key));
				}

				return false;
			});

			if (failure) {
				return std::unexpected(std::move(*failure));
			}

			if (!is_known && !options.ignore_unknown_keys) {
				// The closest field, as for a misspelled key, when it is at most two edits away and shares at least one character.
				std::string_view suggestion;
				std::size_t best = 3;

				detail::visit_fields(result, [&](std::string_view field_key, const auto&) {
					// Suggestions are best effort for keys up to the diagnostic path limit. Skip lengths that cannot beat the closest match, and bound quadratic edit-distance work on untrusted keys.
					// Note: Not using std::minmax because it made this longer without a consistent diagnostic speedup on the tested libraries.
					const std::size_t length_difference = key.size() > field_key.size() ? key.size() - field_key.size() : field_key.size() - key.size();

					if (length_difference >= best || key.size() > detail::max_path_key_length || field_key.size() > detail::max_path_key_length) {
						return true;
					}

					const std::size_t distance = detail::edit_distance(key, field_key);

					if (distance < best && distance < std::max(key.size(), field_key.size())) {
						best = distance;
						suggestion = field_key;
					}

					return true;
				});

				const std::string hint = suggestion.empty() ? std::string{} : detail::concat(". Did you mean ", detail::describe_key(suggestion), "?");
				return std::unexpected(detail::inside(detail::make_error(error_code::unknown_key, detail::concat("Unknown key ", detail::describe_key(key), ", which the type has no field for", hint)), detail::path_key(key)));
			}
		}

		return result;
	}

	template<class Source>
	static constexpr std::expected<value, error> to_value(Source&& source)
		requires std::same_as<std::remove_cvref_t<Source>, Struct>
	{
		object members;
		// Count metadata without visiting the source's fields.
		constexpr auto count = [] {
			if constexpr (detail::has_fields<Struct>) {
				return std::tuple_size_v<decltype(soml::fields<Struct>)>;
			} else {
#if defined(__cpp_impl_reflection)
				return std::meta::nonstatic_data_members_of(^^Struct, std::meta::access_context::current()).size();
#else
				return 0UZ;
#endif
			}
		}();
		members.reserve(count);
		std::optional<error> failure;

		detail::visit_fields(source, [&](std::string_view key, auto& field) {
			using field_type = std::remove_cvref_t<decltype(field)>;
			static_assert(encodable<field_type>, "soml cannot write a field of this struct. Specialize `soml::converter` for the field's type");
			std::expected<value, error> converted = soml::to_value(std::forward_like<Source>(field));

			if (!converted) {
				failure = detail::inside(std::move(converted.error()), detail::path_key(key));
				return false;
			}

			detail::object_access::members(members).emplace_back(std::string{key}, std::move(*converted));
			return true;
		});

		if (failure) {
			return std::unexpected(std::move(*failure));
		}

		return value{std::move(members)};
	}
};

template<class T>
constexpr std::expected<T, error> from_value(const value& source, const decode_options& options) {
	static_assert(decodable<T>, "soml cannot read this type. Specialize `soml::converter` for it, or `soml::fields` for a struct");
	return converter<T>::from_value(source, options);
}

template<class T>
constexpr std::expected<T, error> from_value(value&& source, const decode_options& options) {
	static_assert(decodable<T>, "soml cannot read this type. Specialize `soml::converter` for it, or `soml::fields` for a struct");
	return converter<T>::from_value(std::move(source), options);
}

template<class T>
constexpr std::expected<value, error> to_value(const T& source) {
	static_assert(encodable<T>, "soml cannot write this type. Specialize `soml::converter` for it, or `soml::fields` for a struct");
	return converter<T>::to_value(source);
}

template<class T>
	requires (!std::is_lvalue_reference_v<T> && !std::is_const_v<T>)
constexpr std::expected<value, error> to_value(T&& source) {
	static_assert(encodable<T>, "soml cannot write this type. Specialize `soml::converter` for it, or `soml::fields` for a struct");
	return converter<T>::to_value(std::move(source));
}

template<class T>
	requires std::is_move_constructible_v<std::expected<T, error>>
constexpr std::expected<T, error> decode(std::string_view text, const decode_options& options) {
	if constexpr (std::same_as<T, value>) {
		return parse(text);
	} else {
		return parse(text).and_then([&](value&& document) {
			auto result = from_value<T>(std::move(document), options);
			if (!result) {
				// Recover the original structure after conversions may have moved strings or whole containers from it.
				detail::locate_decode_error(result.error(), text, *parse(text));
			}
			return result;
		});
	}
}

namespace detail {

template<class T>
constexpr std::expected<std::string, error> encode_text(T&& source, bool is_canonical) {
	using plain = std::remove_cvref_t<T>;
	if constexpr (std::same_as<plain, value> || std::same_as<plain, array> || std::same_as<plain, object>) {
		return write_text(source, is_canonical);
	} else {
		return soml::to_value(std::forward<T>(source)).and_then([&](const value& document) {
			return write_text(document, is_canonical);
		});
	}
}

} // namespace detail

template<class T>
constexpr std::expected<std::string, error> encode(const T& source) {
	return detail::encode_text(source, false);
}

template<class T>
	requires (!std::is_lvalue_reference_v<T> && !std::is_const_v<T>)
constexpr std::expected<std::string, error> encode(T&& source) {
	return detail::encode_text(std::move(source), false);
}

template<class T>
constexpr std::expected<std::string, error> encode_canonical(const T& source) {
	return detail::encode_text(source, true);
}

template<class T>
	requires (!std::is_lvalue_reference_v<T> && !std::is_const_v<T>)
constexpr std::expected<std::string, error> encode_canonical(T&& source) {
	return detail::encode_text(std::move(source), true);
}

template<class T>
constexpr std::optional<T> value::as() const {
	if constexpr (std::same_as<T, std::string_view>) {
		const std::string* string = get_if<std::string>();
		return string == nullptr ? std::nullopt : std::optional<std::string_view>{*string};
	} else if constexpr (std::same_as<T, std::nullptr_t> || std::same_as<T, bool> || std::same_as<T, std::int64_t> || std::same_as<T, double> || std::same_as<T, std::string> || std::same_as<T, soml::date> || std::same_as<T, soml::instant> || std::same_as<T, soml::duration> || std::same_as<T, soml::array> || std::same_as<T, soml::object>) {
		// The value's own types skip the converter, whose error message is built only to be dropped here.
		const T* item = get_if<T>();
		return item == nullptr ? std::nullopt : std::optional<T>{*item};
	} else if constexpr (decodable<T> && requires { { converter<T>::try_from_value(*this) } -> std::same_as<std::optional<T>>; }) {
		return converter<T>::try_from_value(*this);
	} else {
		// Note: Not using expected::transform(...).value_or(...) because the pipeline adds an observable move of T.
		std::expected<T, error> result = from_value<T>(*this);
		return result ? std::optional<T>{std::move(*result)} : std::nullopt;
	}
}

constexpr std::string_view view::value_or(const char* fallback) const {
	return as<std::string_view>().value_or(fallback);
}

} // namespace soml

// MARK: Formatting

/**
Formats an error as its description, such as `Duplicate key name at line 3, column 1`.
*/
template<>
struct std::formatter<soml::error> : std::formatter<std::string_view> {
	template<class Context>
	auto format(const soml::error& error, Context& context) const {
		return std::formatter<std::string_view>::format(error.description(), context);
	}
};

/**
Formats an instant in canonical form, such as `2026-09-19T14:00:00Z`.
*/
template<>
struct std::formatter<soml::instant> : std::formatter<std::string_view> {
	template<class Context>
	auto format(const soml::instant& instant, Context& context) const {
		std::string text;

		if (instant.is_valid()) {
			soml::detail::append_instant(text, instant);
		} else {
			text = "invalid instant";
		}

		return std::formatter<std::string_view>::format(text, context);
	}
};

/**
Formats a type with the spec's name, such as `int`.
*/
template<>
struct std::formatter<soml::type> : std::formatter<std::string_view> {
	template<class Context>
	auto format(soml::type kind, Context& context) const {
		return std::formatter<std::string_view>::format(soml::to_string(kind), context);
	}
};

template<>
struct std::formatter<soml::error_code> : std::formatter<std::string_view> {
	template<class Context>
	auto format(soml::error_code code, Context& context) const {
		return std::formatter<std::string_view>::format(soml::to_string(code), context);
	}
};

/*
MIT License

Copyright (c) Sindre Sorhus <sindresorhus@gmail.com> (https://sindresorhus.com)

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#endif // SOML_HPP
