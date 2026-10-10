#include <soml.hpp>

#include <cmath>
#include <cstdio>
#include <source_location>

namespace {

int failures = 0;

void check(bool condition, const std::source_location location = std::source_location::current()) {
	if (!condition) {
		std::fprintf(stderr, "%s:%u: check failed\n", location.file_name(), location.line());
		failures++;
	}
}

constexpr bool float_examples() {
	return *soml::to_value(0.1F)->get_if<double>() == 0.1
		&& *soml::to_value(-0.1F)->get_if<double>() == -0.1
		&& *soml::to_value(std::numeric_limits<float>::denorm_min())->get_if<double>() == 1e-45
		&& *soml::to_value(std::numeric_limits<float>::max())->get_if<double>() == 3.4028235e38
		&& std::bit_cast<std::uint64_t>(*soml::to_value(-0.0F)->get_if<double>()) == (std::uint64_t{1} << 63U)
		&& *soml::to_value(std::numeric_limits<float>::infinity())->get_if<double>() == std::numeric_limits<double>::infinity()
		&& soml::to_value(std::numeric_limits<float>::quiet_NaN()).error().code == soml::error_code::invalid_value
		&& *soml::value{0.1F}.get_if<double>() == static_cast<double>(0.1F);
}

static_assert(float_examples());
static_assert(*soml::encode(std::array{0.1F, 1.25F}) == "[\n\t0.1\n\t1.25\n]\n");
static_assert(soml::encodable<long double> && soml::decodable<long double>);

constexpr bool float_range_examples() {
	struct test_case {
		std::string_view literal;
		soml::detail::float_status status;
		double number = 0;
	};
	constexpr std::array cases{
		test_case{"1e310", soml::detail::float_status::too_large},
		test_case{"-1e310", soml::detail::float_status::too_large},
		test_case{"1e309", soml::detail::float_status::too_large},
		test_case{"1e-331", soml::detail::float_status::too_small},
		test_case{"1e-332", soml::detail::float_status::too_small},
		test_case{"-1e-332", soml::detail::float_status::too_small},
		test_case{"1.797_693_134_862_315_7e308", soml::detail::float_status::ok, std::numeric_limits<double>::max()},
		test_case{"1.797_693_134_862_315_9e308", soml::detail::float_status::too_large},
		test_case{"2.225_073_858_507_201_4e-308", soml::detail::float_status::ok, std::numeric_limits<double>::min()},
		test_case{"2.470_328_229_206_232_7e-324", soml::detail::float_status::too_small},
		test_case{"2.470_328_229_206_232_8e-324", soml::detail::float_status::ok, std::numeric_limits<double>::denorm_min()},
		test_case{"4.940_656_458_412_465_4e-324", soml::detail::float_status::ok, std::numeric_limits<double>::denorm_min()},
		test_case{"0.0_0e999_999_999_999_999", soml::detail::float_status::ok},
		test_case{"-0.0_0e-999_999_999_999_999", soml::detail::float_status::ok},
	};

	for (const test_case& item : cases) {
		double number = 1;
		if (soml::detail::classify_number(item.literal) != soml::detail::number_kind::floating || soml::detail::parse_float(item.literal, number) != item.status) {
			return false;
		}
		if (item.status == soml::detail::float_status::ok && std::bit_cast<std::uint64_t>(number) != std::bit_cast<std::uint64_t>(item.number)) {
			return false;
		}
	}

	return true;
}

static_assert(float_range_examples());

constexpr bool large_plain_integer_examples() {
	for (const std::int64_t number : std::array<std::int64_t, 7>{1'000'000'000'000'000LL, 9'999'999'999'999'999LL, 10'000'000'000'000'000LL, 99'999'999'999'999'999LL, 100'000'000'000'000'000LL, 999'999'999'999'999'999LL, std::numeric_limits<std::int64_t>::max()}) {
		for (const std::int64_t signed_number : {number, -number}) {
			const std::string literal = soml::detail::decimal_string(soml::detail::magnitude(signed_number));
			const std::string source = "[" + std::string{signed_number < 0 ? "-" : ""} + literal + "]";
			const auto parsed = soml::parse(source);
			if (!parsed || (*parsed)[0].as<std::int64_t>() != signed_number || (*parsed)[0].type() != soml::type::integer) {
				return false;
			}
		}
	}
	for (const std::string_view literal : {"999999999999999999.9", "999999999999999999e0", "999_999_999_999_999_999"}) {
		const auto parsed = soml::parse("[" + std::string{literal} + "]");
		if (!parsed || (literal.contains('_') ? (*parsed)[0].as<std::int64_t>() != 999'999'999'999'999'999LL : (*parsed)[0].as<double>() != 1e18)) {
			return false;
		}
	}
	return !soml::parse("[9223372036854775808]") && !soml::parse("[-9223372036854775809]") && !soml::parse("[0999999999999999999]");
}

static_assert(large_plain_integer_examples());

constexpr bool plain_number_boundary_examples() {
	for (const std::string_view literal : {"0", "1", "-1", "30.5", "-0.5", "123456789012345678"}) {
		const auto expected = soml::parse("[" + std::string{literal} + "]");
		if (!expected) {
			return false;
		}
		for (const std::string_view separator : {" ", "\t", "\n", ",", "# comment\n", "/* comment */"}) {
			const auto parsed = soml::parse("[" + std::string{literal} + std::string{separator} + "]");
			if (!parsed || *parsed != *expected) {
				return false;
			}
		}
		for (const std::string_view suffix : {"false", "_x", ".", "e", ":", "z"}) {
			if (soml::parse("[" + std::string{literal} + std::string{suffix} + "]")) {
				return false;
			}
		}
	}
	const auto classified = soml::parse("[1s, 1.5e2, 0xA, 2026-01-01T00:00:00Z]");
	return classified && (*classified)[0].type() == soml::type::duration && (*classified)[1].as<double>() == 150.0 && (*classified)[2].as<int>() == 10 && (*classified)[3].type() == soml::type::instant;
}

static_assert(plain_number_boundary_examples());

constexpr bool speculative_diagnostic_examples() {
	struct test_case {
		std::string_view source;
		std::string_view message;
		std::size_t offset;
		std::size_t line;
		std::size_t column;
	};
	constexpr std::array cases{
		test_case{"123", "A bare value is not a document. A document is an object or an array, so write it as `key: value` or `[value]`", 0, 1, 1},
		test_case{"1e+", "Expected “:” after the key, but found “+”. A key that contains characters other than letters, digits, “_”, and “-” must be quoted", 2, 1, 3},
		test_case{"a: #FFF", "Expected a value, but reached the end of the document. “#” starts a comment, so a value that starts with “#” must be quoted, as in '#FFF'", 7, 1, 8},
		test_case{"{a: #FFF\n}", "Expected a value, but found “}”. “#” starts a comment, so a value that starts with “#” must be quoted, as in '#FFF'", 9, 2, 1},
		test_case{"a:\n  @bad", "Expected a value, but found “@”", 5, 2, 3},
		test_case{"a:\n  25:00:00", "A time of day is a string, so it must be quoted", 5, 2, 3},
		test_case{"a:\n  'broken", "Unterminated string. A '...' string must end on the line it starts on; use a block string (''') for multiple lines", 5, 2, 3},
		test_case{"a:\n  1e+", "A “+” sign is not allowed in a number, including in an exponent", 5, 2, 3},
		test_case{"a:\n  b: 1", "Expected a value, but found the key b. Indentation does not nest objects, so write a: {b: …}", 5, 2, 3},
	};
	for (const test_case& item : cases) {
		const auto parsed = soml::parse(item.source);
		if (parsed || parsed.error().code != soml::error_code::syntax || parsed.error().message != item.message || parsed.error().offset != item.offset || parsed.error().line != item.line || parsed.error().column != item.column) {
			return false;
		}
	}
	return true;
}

static_assert(speculative_diagnostic_examples());

constexpr bool bare_key_examples(std::size_t alignments) {
	if (soml::is_bare_key("")) {
		return false;
	}
	for (std::size_t alignment = 0; alignment < alignments; alignment++) {
		for (int byte = 0; byte < 256; byte++) {
			const std::string key = std::string(alignment + 1, 'a') + static_cast<char>(byte) + std::string(24, 'a');
			if (soml::is_bare_key(key) != soml::detail::is_bare_key_character(byte)) {
				return false;
			}
		}
	}
	return true;
}

static_assert(bare_key_examples(1));

constexpr bool bigint_boundaries() {
	for (const std::uint64_t value : {0ULL, 1ULL, 999'999'999ULL, 1'000'000'000ULL, 4'294'967'295ULL, 4'294'967'296ULL, 18'446'744'073'709'551'615ULL}) {
		soml::detail::bigint integer;
		integer.assign_u64(value);
		std::array<std::uint8_t, 20> digits{};
		const int count = integer.to_decimal(digits.data());
		std::string text;
		for (int index = 0; index < count; index++) {
			text.push_back(static_cast<char>('0' + digits[static_cast<std::size_t>(index)]));
		}
		if (text != (value == 0 ? std::string{} : soml::detail::decimal_string(value))) {
			return false;
		}
		integer.assign_digits(std::span{digits}.first(static_cast<std::size_t>(count)));
		if (integer.window_right(0) != value || integer.bit_length() != std::bit_width(value)) {
			return false;
		}
	}

	for (const int shift : {0, 1, 31, 32, 33, 63, 64, 65, 1074}) {
		soml::detail::bigint integer;
		integer.assign_u64(0xFFFF'FFFF'FFFF'FFFF);
		integer.shift_left(shift);
		if (integer.bit_length() != 64 + shift || integer.window_right(shift) != 0xFFFF'FFFF'FFFF'FFFF || !integer.bit(shift) || integer.any_below(shift) || !integer.any_below(shift + 1)) {
			return false;
		}
	}

	for (const int shift : {0, 31, 32, 52, 63}) {
		soml::detail::bigint integer;
		integer.assign_u64(1);
		if (integer.window_left(shift) != (std::uint64_t{1} << shift)) {
			return false;
		}
	}

	for (const int exponent : {0, 1, 12, 13, 14, 26, 1074}) {
		soml::detail::bigint integer;
		integer.assign_u64(1);
		integer.multiply_pow5(exponent);
		bool sticky = false;
		integer.divide_pow5(exponent, sticky);
		if (sticky || integer.window_right(0) != 1 || integer.bit_length() != 1) {
			return false;
		}
	}

	soml::detail::bigint integer;
	integer.assign_u64(0xFFFF'FFFF'FFFF'FFFF);
	integer.add_small(1);
	if (integer.bit_length() != 65 || integer.window_right(64) != 1 || integer.any_below(64)) {
		return false;
	}
	integer.multiply_small(0);
	integer.add_small(7);
	bool sticky = false;
	integer.divide_pow5(1, sticky);
	integer.divide_pow5(0, sticky);
	return sticky && integer.window_right(0) == 1 && integer.divide_small(2) == 1 && integer.is_zero();
}

static_assert(bigint_boundaries());

constexpr bool bigint_reuse() {
	soml::detail::bigint integer;
	for (const std::uint64_t value : {0ULL, 1ULL, 0xFFFF'FFFFULL, 0x1'0000'0000ULL, 0xFFFF'FFFF'FFFF'FFFFULL}) {
		integer.assign_u64(0xFFFF'FFFF'FFFF'FFFF);
		integer.shift_left(1024);
		integer.assign_u64(value);
		if (integer.window_right(0) != value || integer.bit_length() != std::bit_width(value) || integer.window_right(64) != 0) {
			return false;
		}

		integer.clear();
		integer.add_small(0xFFFF'FFFF);
		integer.add_small(1);
		if (integer.window_right(0) != 0x1'0000'0000 || integer.bit_length() != 33) {
			return false;
		}
		if (integer.divide_small(2) != 0) {
			return false;
		}
		integer.add_small(0x8000'0000);
		if (integer.window_right(0) != 0x1'0000'0000) {
			return false;
		}
		integer.multiply_small(0);
		integer.add_small(7);
		integer.shift_left(33);
		if (integer.window_right(0) != (7ULL << 33) || integer.bit_length() != 36) {
			return false;
		}
	}
	return true;
}

static_assert(bigint_reuse());

constexpr bool decimal_chunk_boundaries() {
	std::array<std::uint8_t, 800> digits;
	digits.fill(9);
	soml::detail::bigint integer;
	for (const std::size_t length : {800UZ, 0UZ, 1UZ, 8UZ, 9UZ, 10UZ, 17UZ, 18UZ, 19UZ, 799UZ}) {
		integer.assign_digits(std::span{digits}.first(length));
		std::array<std::uint8_t, 800> output{};
		const int count = integer.to_decimal(output.data());
		if (count != static_cast<int>(length) || !std::ranges::equal(std::span{output}.first(length), std::span{digits}.first(length))) {
			return false;
		}
	}
	return true;
}

static_assert(decimal_chunk_boundaries());

constexpr bool decimal_mixed_chunks(std::size_t maximum_length) {
	std::array<std::uint8_t, 800> digits{};
	for (std::size_t index = 0; index < digits.size(); ++index) {
		// Include entire zero chunks, leading zeros, and nonzero chunks with internal zeros.
		if (index % 19 >= 9) {
			digits[index] = static_cast<std::uint8_t>(index % 10);
		}
	}
	soml::detail::bigint integer;
	for (std::size_t length = 0; length <= maximum_length; ++length) {
		auto expected = std::span{digits}.first(length);
		integer.assign_digits(expected);
		while (!expected.empty() && expected.front() == 0) {
			expected = expected.subspan(1);
		}
		std::array<std::uint8_t, 800> output;
		const int count = integer.to_decimal(output.data());
		if (count != static_cast<int>(expected.size()) || !std::ranges::equal(std::span{output}.first(expected.size()), expected)) {
			return false;
		}
	}
	return true;
}

static_assert(decimal_mixed_chunks(36));

constexpr bool decimal_chunk_values() {
	std::uint64_t power = 1;
	for (int exponent = 0; exponent <= 18; ++exponent) {
		for (const std::uint64_t value : {power - 1, power, power + 1}) {
			soml::detail::bigint integer;
			integer.assign_u64(value);
			std::array<std::uint8_t, 20> digits{};
			const int count = integer.to_decimal(digits.data());
			if (count > 0 && digits.front() == 0) {
				return false;
			}
			std::uint64_t reconstructed = 0;
			for (const std::uint8_t digit : std::span{digits}.first(static_cast<std::size_t>(count))) {
				if (digit > 9) {
					return false;
				}
				reconstructed = reconstructed * 10 + digit;
			}
			if (reconstructed != value) {
				return false;
			}
		}
		power *= 10;
	}
	return true;
}

static_assert(decimal_chunk_values());

constexpr bool positive_decimal_ties() {
	for (const auto& [literal, expected] : std::array{
		std::pair{std::string_view{"9007199254740993e0"}, 9007199254740992.0},
		std::pair{std::string_view{"9007199254740995e0"}, 9007199254740996.0},
		std::pair{std::string_view{"3602879701896398e1"}, 36028797018963984.0},
		std::pair{std::string_view{"3602879701896402e1"}, 36028797018964016.0},
		std::pair{std::string_view{"7205759403792796e1"}, 72057594037927968.0},
	}) {
		double result = 0;
		if (soml::detail::exact_float(literal, result) != soml::detail::float_status::ok || result != expected) {
			return false;
		}
	}
	return true;
}

static_assert(positive_decimal_ties());

void positive_decimal_exponents() {
	for (const std::string_view significand : {"1", "5", "9007199254740993", "17976931348623157", "99999999999999999"}) {
		for (int exponent = 0; exponent <= 310; ++exponent) {
			for (const bool negative : {false, true}) {
				const std::string literal = std::string{negative ? "-" : ""} + std::string{significand} + "e" + soml::detail::decimal_string(static_cast<std::uint64_t>(exponent));
				double expected = 0;
				const auto parsed = std::from_chars(literal.data(), literal.data() + literal.size(), expected);
				double result = 0;
				const auto status = soml::detail::exact_float(literal, result);
				if (parsed.ec == std::errc::result_out_of_range) {
					check(status == soml::detail::float_status::too_large);
				} else {
					check(parsed.ec == std::errc{} && parsed.ptr == literal.data() + literal.size());
					check(status == soml::detail::float_status::ok && std::bit_cast<std::uint64_t>(result) == std::bit_cast<std::uint64_t>(expected));
				}
			}
		}
	}
}

template<class Float>
void shortest_binary_powers() {
	using bits_type = std::conditional_t<std::same_as<Float, float>, std::uint32_t, std::uint64_t>;
	constexpr int significand_bits = std::numeric_limits<Float>::digits - 1;
	constexpr int exponent_offset = 1 - std::numeric_limits<Float>::max_exponent;
	constexpr int exponent_width = sizeof(Float) * 8 - significand_bits - 1;
	// Every power-of-two boundary and its adjacent values have different trailing-bit counts and rounding intervals.
	for (int exponent = std::numeric_limits<Float>::min_exponent - std::numeric_limits<Float>::digits; exponent < std::numeric_limits<Float>::max_exponent; ++exponent) {
		const Float power = std::ldexp(Float{1}, exponent);
		const bits_type bits = std::bit_cast<bits_type>(power);
		for (const bits_type candidate : {bits - 1, bits, bits + 1}) {
			if (candidate == 0) {
				continue;
			}
			std::array<char, 32> buffer;
			const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), std::bit_cast<Float>(candidate), std::chars_format::scientific);
			check(written.ec == std::errc{});
			const auto marker = std::find(buffer.data(), written.ptr, 'e');
			int decimal_exponent = 0;
			const auto parsed = std::from_chars(marker + 2, written.ptr, decimal_exponent);
			check(parsed.ec == std::errc{});
			if (marker[1] == '-') {
				decimal_exponent = -decimal_exponent;
			}
			std::string expected{buffer.data(), marker};
			std::erase(expected, '.');
			soml::detail::decimal decimal;
			decimal.assign_shortest<significand_bits, exponent_offset, exponent_width>(candidate);
			check(decimal.point == decimal_exponent + 1);
			check(std::ranges::equal(std::span{decimal.digits}.first(static_cast<std::size_t>(decimal.count)), expected, {}, [](std::uint8_t digit) {
				return static_cast<char>('0' + digit);
			}));
		}
	}
}

constexpr bool powers_of_five_examples() {
	std::uint64_t power = 1;
	for (int exponent = 0; exponent <= 27; ++exponent) {
		soml::detail::bigint integer;
		integer.assign_u64(1);
		integer.multiply_pow5(exponent);
		if (integer.window_right(0) != power || integer.bit_length() != std::bit_width(power)) {
			return false;
		}
		for (const std::uint64_t value : {power - 1, power, power + 1}) {
			for (const bool already_sticky : {false, true}) {
				integer.assign_u64(value);
				bool sticky = already_sticky;
				integer.divide_pow5(exponent, sticky);
				if (integer.window_right(0) != value / power || sticky != (already_sticky || value % power != 0)) {
					return false;
				}
			}
		}
		if (exponent < 27) {
			power *= 5;
		}
	}
	return true;
}

static_assert(powers_of_five_examples());

constexpr bool padded_float_examples() {
	struct test_case {
		std::string_view significand;
		std::string_view exponent;
		std::uint64_t expected;
	};
	constexpr std::array cases{
		test_case{"1.00000000000000011102230246251565404236316680908203125", "e0", 0x3FF0'0000'0000'0000},
		test_case{"1.00000000000000033306690738754696212708950042724609375", "e0", 0x3FF0'0000'0000'0002},
		test_case{"0.1", "e0", 0x3FB9'9999'9999'999A},
		test_case{"2.2250738585072014", "e-308", 0x0010'0000'0000'0000},
		test_case{"4.9406564584124654", "e-324", 1},
	};
	for (const auto& [significand, exponent, expected] : cases) {
		for (const bool negative : {false, true}) {
			const std::string literal = std::string{negative ? "-" : ""} + std::string{significand} + std::string(810, '0') + std::string{exponent};
			double result = 0;
			if (soml::detail::exact_float(literal, result) != soml::detail::float_status::ok || std::bit_cast<std::uint64_t>(result) != (expected | (std::uint64_t{negative} << 63))) {
				return false;
			}
		}
	}
	return true;
}

static_assert(padded_float_examples());

constexpr bool shortest_decimal_examples() {
	struct test_case {
		double value;
		std::string_view digits;
		int point;
	};
	constexpr std::array cases{
		test_case{0.125, "125", 0},
		test_case{0.5, "5", 0},
		test_case{1.0, "1", 1},
		test_case{8.0, "8", 1},
		test_case{0.1, "1", 0},
		test_case{1e23, "1", 24},
		// Both shortest candidates are equally close; choose the even final digit.
		test_case{1000000000000000.25, "10000000000000002", 16},
		test_case{1000000000000000.75, "10000000000000008", 16},
		test_case{1000000000000000128.0, "10000000000000001", 19},
		test_case{0x1.0000000000001p0, "10000000000000002", 1},
		test_case{std::numeric_limits<double>::denorm_min(), "5", -323},
		test_case{std::numeric_limits<double>::min(), "22250738585072014", -307},
		test_case{std::numeric_limits<double>::max(), "17976931348623157", 309},
	};
	for (const auto& item : cases) {
		soml::detail::decimal decimal;
		decimal.assign_shortest(std::bit_cast<std::uint64_t>(item.value));
		if (decimal.count != static_cast<int>(item.digits.size()) || decimal.point != item.point) {
			return false;
		}
		for (int index = 0; index < decimal.count; index++) {
			if (decimal.digits[static_cast<std::size_t>(index)] != item.digits[static_cast<std::size_t>(index)] - '0') {
				return false;
			}
		}
	}
	return true;
}

static_assert(shortest_decimal_examples());

template<class Float>
constexpr bool shortest_candidate_rounding() {
	constexpr bool is_float = std::same_as<Float, float>;
	constexpr Float base = static_cast<Float>(is_float ? 2000000.0 : 1000000000000000.0);
	constexpr auto prefix = is_float ? std::string_view{"2000000"} : std::string_view{"1000000000000000"};
	// Eighths exercise both rounding directions and decimal ties with even digits on either side.
	constexpr std::array expected_last_digits{1, 2, 4, 5, 6, 8, 9};
	soml::detail::decimal decimal;
	for (std::size_t index = 0; index < expected_last_digits.size(); ++index) {
		const Float number = base + static_cast<Float>(index + 1) / Float{8};
		if constexpr (is_float) {
			decimal.assign_shortest<23, -127, 8>(std::bit_cast<std::uint32_t>(number));
		} else {
			decimal.assign_shortest(std::bit_cast<std::uint64_t>(number));
		}
		if (decimal.count != static_cast<int>(prefix.size() + 1) || decimal.point != static_cast<int>(prefix.size()) || decimal.digits[prefix.size()] != expected_last_digits[index]) {
			return false;
		}
		if (!std::ranges::equal(std::span{decimal.digits}.first(prefix.size()), prefix, {}, [](std::uint8_t digit) {
			return static_cast<char>('0' + digit);
		})) {
			return false;
		}
	}
	return true;
}

static_assert(shortest_candidate_rounding<float>());
static_assert(shortest_candidate_rounding<double>());

constexpr bool shortest_asymmetric_intervals() {
	struct test_case {
		float value;
		std::string_view digits;
		int point;
	};
	// At powers of two, the nearer decimal can fall outside the narrower half of the binary rounding interval.
	constexpr std::array cases{
		test_case{0x1p-96F, "12621775", -28},
		test_case{0x1p87F, "15474251", 27},
	};
	for (const auto& [value, digits, point] : cases) {
		soml::detail::decimal decimal;
		decimal.assign_shortest<23, -127, 8>(std::bit_cast<std::uint32_t>(value));
		if (decimal.point != point || decimal.count != static_cast<int>(digits.size()) || !std::ranges::equal(std::span{decimal.digits}.first(digits.size()), digits, {}, [](std::uint8_t digit) {
			return static_cast<char>('0' + digit);
		})) {
			return false;
		}
	}
	return true;
}

static_assert(shortest_asymmetric_intervals());

constexpr bool shortest_decimal_lengths() {
	constexpr std::array<double, 17> values{1.0, 1.2, 1.23, 1.234, 1.2345, 1.23456, 1.234567, 1.2345678, 1.23456789, 1.234567891, 1.2345678912, 1.23456789123, 1.234567891234, 1.2345678912345, 1.23456789123456, 1.234567891234567, 1.2345678912345677};
	constexpr std::string_view expected = "12345678912345677";
	soml::detail::decimal decimal;
	for (std::size_t index = 0; index < values.size(); ++index) {
		decimal.assign_shortest(std::bit_cast<std::uint64_t>(values[index]));
		if (decimal.count != static_cast<int>(index + 1) || decimal.point != 1 || !std::ranges::equal(std::span{decimal.digits}.first(index + 1), expected.substr(0, index + 1), {}, [](std::uint8_t digit) {
			return static_cast<char>('0' + digit);
		})) {
			return false;
		}
	}
	return true;
}

constexpr bool shortest_float_lengths() {
	constexpr std::array<float, 9> values{1.0F, 1.2F, 1.23F, 1.234F, 1.2345F, 1.23456F, 1.234567F, 1.2345678F, 1000000064.0F};
	soml::detail::decimal decimal;
	for (std::size_t index = 0; index < values.size(); ++index) {
		const auto expected = index == values.size() - 1 ? std::string_view{"100000006"} : std::string_view{"12345678"}.substr(0, index + 1);
		decimal.assign_shortest<23, -127, 8>(std::bit_cast<std::uint32_t>(values[index]));
		if (decimal.count != static_cast<int>(index + 1) || decimal.point != (index == values.size() - 1 ? 10 : 1) || !std::ranges::equal(std::span{decimal.digits}.first(index + 1), expected, {}, [](std::uint8_t digit) {
			return static_cast<char>('0' + digit);
		})) {
			return false;
		}
	}
	return true;
}

static_assert(shortest_decimal_lengths());
static_assert(shortest_float_lengths());

constexpr bool truncated_float_example(std::size_t padding = 1000) {
	const std::string halfway = "1.00000000000000011102230246251565404236316680908203125";
	double number = 0;
	if (soml::detail::exact_float(halfway, number) != soml::detail::float_status::ok || number != 1.0) {
		return false;
	}
	// A nonzero digit beyond the decimal buffer changes an exact tie into a value above it.
	const std::string padded_halfway = halfway + std::string(padding, '0');
	if (soml::detail::exact_float(padded_halfway, number) != soml::detail::float_status::ok || number != 1.0) {
		return false;
	}
	const std::string above_halfway = padded_halfway + "1";
	return soml::detail::exact_float(above_halfway, number) == soml::detail::float_status::ok && std::bit_cast<std::uint64_t>(number) == 0x3FF0'0000'0000'0001;
}

static_assert(truncated_float_example());

template<class Float>
constexpr bool wide_examples() {
	const auto encoded = soml::to_value(Float{1.25L});
	const auto decoded = soml::from_value<Float>(soml::value{1.25});

	if (!encoded || *encoded->template get_if<double>() != 1.25 || decoded != Float{1.25L}) {
		return false;
	}

	if (!soml::to_value(static_cast<Float>(std::numeric_limits<double>::max())) || !soml::to_value(static_cast<Float>(std::numeric_limits<double>::denorm_min()))) {
		return false;
	}

	if (soml::to_value(std::numeric_limits<Float>::quiet_NaN()).error().code != soml::error_code::invalid_value) {
		return false;
	}

	if (*soml::to_value(-std::numeric_limits<Float>::infinity())->template get_if<double>() != -std::numeric_limits<double>::infinity()) {
		return false;
	}

	if constexpr (std::numeric_limits<Float>::max_exponent > std::numeric_limits<double>::max_exponent) {
		const Float too_large = static_cast<Float>(std::numeric_limits<double>::max()) * 2;
		const Float too_small = static_cast<Float>(std::numeric_limits<double>::denorm_min()) / 4;

		if (soml::to_value(too_large).error().code != soml::error_code::out_of_range || soml::to_value(-too_large).error().code != soml::error_code::out_of_range || soml::to_value(too_small).error().code != soml::error_code::out_of_range || soml::to_value(-too_small).error().code != soml::error_code::out_of_range) {
			return false;
		}

		const Float above_max = static_cast<Float>(std::numeric_limits<double>::max()) + static_cast<Float>(std::numeric_limits<double>::max()) * std::numeric_limits<Float>::epsilon();

		if (soml::to_value(above_max).error().code != soml::error_code::out_of_range) {
			return false;
		}
	}

	return true;
}

static_assert(wide_examples<long double>());

template<class Float>
constexpr bool floating_instant_examples() {
	using seconds = std::chrono::duration<Float>;
	using nanoseconds = std::chrono::duration<Float, std::nano>;
	using days = std::chrono::duration<Float, std::ratio<86'400>>;

	for (const std::int64_t count : {-1'000'000'001, -999'999'999, -1'000, -100, -10, -2, -1, 0, 1, 2, 10, 100, 1'000, 999'999'999, 1'000'000'001}) {
		const soml::instant time{std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{count}}};
		const auto as_seconds = time.to_sys_time<seconds>();
		const auto as_nanoseconds = time.to_sys_time<nanoseconds>();
		const auto as_days = time.to_sys_time<days>();
		const Float expected_seconds = static_cast<Float>(static_cast<long double>(count) / 1'000'000'000.0L);
		const Float expected_days = static_cast<Float>(static_cast<long double>(count) / 86'400'000'000'000.0L);

		if (!as_seconds || !as_nanoseconds || !as_days || as_seconds->time_since_epoch().count() != expected_seconds || as_nanoseconds->time_since_epoch().count() != static_cast<Float>(count) || as_days->time_since_epoch().count() != expected_days) {
			return false;
		}
	}

	return !soml::instant{0, -1}.to_sys_time<seconds>() && !soml::instant{0, 1'000'000'000}.to_sys_time<seconds>();
}

static_assert(floating_instant_examples<float>());
static_assert(floating_instant_examples<double>());
static_assert(floating_instant_examples<long double>());

constexpr bool subnanosecond_instants() {
	using picoseconds = std::chrono::duration<std::int64_t, std::pico>;

	// Instants floor toward the past; durations round to the nearest nanosecond with ties to even.
	for (const std::int64_t count : {-1'999, -1'500, -1'001, -1'000, -999, -1, 0, 1, 999, 1'000, 1'001, 1'500, 1'999}) {
		const auto time = std::chrono::sys_time<picoseconds>{picoseconds{count}};
		const auto expected = std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{(count - (count < 0 ? 999 : 0)) / 1'000}};
		const soml::instant converted{time};

		if (!converted.is_valid() || converted.to_sys_time<std::chrono::nanoseconds>() != expected || soml::to_value(time) != soml::value{soml::instant{expected}}) {
			return false;
		}

		const auto duration = soml::to_value(picoseconds{count});

		if (duration != soml::value{std::chrono::round<soml::duration>(picoseconds{count})}) {
			return false;
		}
	}

	return true;
}

constexpr bool small_duration_ranges() {
	using signed_nanoseconds = std::chrono::duration<std::int8_t, std::nano>;
	using unsigned_nanoseconds = std::chrono::duration<std::uint8_t, std::nano>;

	for (const std::int64_t count : {-129, -128, -1, 0, 127, 128, 255, 256}) {
		const soml::value source{std::chrono::nanoseconds{count}};
		const auto signed_result = soml::from_value<signed_nanoseconds>(source);
		const auto unsigned_result = soml::from_value<unsigned_nanoseconds>(source);
		const bool fits_signed = count >= -128 && count <= 127;
		const bool fits_unsigned = count >= 0 && count <= 255;

		if (signed_result.has_value() != fits_signed || unsigned_result.has_value() != fits_unsigned) {
			return false;
		}

		if (fits_signed && (signed_result->count() != count || soml::to_value(*signed_result) != source)) {
			return false;
		}

		if (fits_unsigned && (unsigned_result->count() != count || soml::to_value(*unsigned_result) != source)) {
			return false;
		}
	}

	return true;
}

static_assert(subnanosecond_instants());
static_assert(small_duration_ranges());

} // namespace

int main() {
	check(float_examples());
	check(float_range_examples());
	check(large_plain_integer_examples());
	check(plain_number_boundary_examples());
	check(speculative_diagnostic_examples());
	check(bare_key_examples(24));
	check(bigint_boundaries());
	check(bigint_reuse());
	check(decimal_chunk_boundaries());
	check(decimal_mixed_chunks(800));
	check(decimal_chunk_values());
	check(positive_decimal_ties());
	positive_decimal_exponents();
	shortest_binary_powers<float>();
	shortest_binary_powers<double>();
	check(powers_of_five_examples());
	check(padded_float_examples());
	check(shortest_decimal_examples());
	check(shortest_candidate_rounding<float>());
	check(shortest_candidate_rounding<double>());
	check(shortest_asymmetric_intervals());
	check(shortest_decimal_lengths());
	check(shortest_float_lengths());
	for (const std::size_t padding : {744UZ, 745UZ, 746UZ, 1000UZ}) {
		check(truncated_float_example(padding));
	}
	check(subnanosecond_instants());
	check(small_duration_ranges());
	check(wide_examples<long double>());
	check(floating_instant_examples<float>());
	check(floating_instant_examples<double>());
	check(floating_instant_examples<long double>());

	// A subsecond instant before the epoch must not disappear when its parts cancel.
	const soml::instant before_epoch{-1, 999'999'999};
	using float_seconds = std::chrono::duration<float>;
	using double_seconds = std::chrono::duration<double>;
	check(before_epoch.to_sys_time<float_seconds>()->time_since_epoch().count() == -1e-9F);
	check(before_epoch.to_sys_time<double_seconds>()->time_since_epoch().count() == -1e-9);
	check(soml::from_value<std::chrono::sys_time<float_seconds>>(soml::value{before_epoch})->time_since_epoch().count() == -1e-9F);

	std::uint32_t bits = 1;

	for (int iteration = 0; iteration < 100'000; iteration++) {
		bits = bits * 1664525U + 1013904223U;
		const float number = std::bit_cast<float>(bits);

		if (!std::isfinite(number)) {
			continue;
		}

		const auto encoded = soml::to_value(number);
		check(encoded.has_value());
		const double normalized = *encoded->get_if<double>();
		check(soml::from_value<float>(*encoded) == number);

		std::array<char, 32> buffer{};
		const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number, std::chars_format::scientific);
		double expected = 0;
		const auto read = std::from_chars(buffer.data(), written.ptr, expected);
		check(read.ec == std::errc{} && normalized == expected);

		// The constexpr fallback uses the same rounding choice as the standard library.
		soml::detail::decimal digits;
		digits.assign_shortest<23, -127, 8>(bits & 0x7FFF'FFFFU);
		const double magnitude = *digits.to_double();
		check((number < 0 ? -magnitude : magnitude) == normalized);
	}

	std::printf("numeric conversion: %d failures (long double: %zu bytes)\n", failures, sizeof(long double));
	return failures == 0 ? 0 : 1;
}
