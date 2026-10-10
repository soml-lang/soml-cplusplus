#include <soml.hpp>

#include <print>

namespace {

struct test_results {
	std::size_t count = 0;
	std::size_t failures = 0;

	void check(std::string_view name, bool condition) {
		count++;
		if (!condition) {
			std::println(stderr, "FAIL: {}", name);
			failures++;
		}
	}
};

constexpr bool lane_order() {
	for (unsigned mask = 0; mask < 256; mask++) {
		std::array<std::uint8_t, 8> bytes{};
		std::size_t expected = bytes.size();
		for (std::size_t index = 0; index < bytes.size(); index++) {
			if ((mask & (1U << index)) != 0) {
				bytes[index] = 0x80;
				expected = std::min(expected, index);
			}
		}
		if (soml::detail::first_marked_byte(std::bit_cast<std::uint64_t>(bytes)) != expected) {
			return false;
		}
	}
	return true;
}

constexpr bool signed_magnitudes() {
	for (const std::int64_t number : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}, std::numeric_limits<std::int64_t>::max()}) {
		if (soml::detail::to_signed(soml::detail::magnitude(number), number < 0) != number) {
			return false;
		}
	}
	return true;
}

constexpr bool hexadecimal_appends() {
	std::string output = "prefix:";
	soml::detail::append_hex(output, 0, 4, "0123456789ABCDEF");
	soml::detail::append_hex(output, std::numeric_limits<std::uint32_t>::max(), 2, "0123456789abcdef");
	soml::detail::append_hex(output, 1, 12, "0123456789abcdef");
	return output == "prefix:0000ffffffff000000000001";
}

constexpr bool integer_appends() {
	std::string output = "prefix:";
	soml::detail::append_integer(output, std::numeric_limits<std::uint64_t>::max());
	output += ',';
	soml::detail::append_integer(output, std::numeric_limits<std::int64_t>::min());
	return output == "prefix:18446744073709551615,-9223372036854775808";
}

constexpr bool value_predicates() {
	const std::array<soml::value, 10> values{nullptr, false, 0, 0.0, std::string_view{}, soml::date{}, soml::instant{}, soml::duration{}, soml::array{}, soml::object{}};
	for (std::size_t index = 0; index < values.size(); index++) {
		const auto& value = values[index];
		const std::array predicates{value.is_null(), value.is_bool(), value.is_int(), value.is_float(), value.is_string(), value.is_date(), value.is_instant(), value.is_duration(), value.is_array(), value.is_object()};
		for (std::size_t alternative = 0; alternative < predicates.size(); alternative++) {
			if (predicates[alternative] != (index == alternative)) {
				return false;
			}
		}
	}
	return true;
}

constexpr bool owned_string_values() {
	std::string source = "before";
	soml::value view{std::string_view{source}};
	soml::value pointer{source.c_str()};
	source[0] = 'B';
	const soml::value embedded{std::string_view{"a\0b", 3}};
	return view.as<std::string_view>() == "before" && pointer.as<std::string_view>() == "before" && embedded.as<std::string_view>() == std::string_view{"a\0b", 3};
}

constexpr bool checked_durations() {
	using ticks = std::chrono::duration<std::int64_t, std::ratio<11, 100'000'000>>;
	using extreme_ticks = std::chrono::duration<std::int64_t, std::ratio<std::numeric_limits<std::int64_t>::max(), 1'000'000'000>>;
	constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
	constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
	const std::array<std::pair<std::expected<soml::value, soml::error>, std::optional<soml::duration>>, 9> cases{{
		{soml::to_value(ticks{maximum / 110}), soml::duration{maximum / 110 * 110}},
		{soml::to_value(ticks{maximum / 110 + 1}), std::nullopt},
		{soml::to_value(ticks{minimum / 110}), soml::duration{minimum / 110 * 110}},
		{soml::to_value(ticks{minimum / 110 - 1}), std::nullopt},
		{soml::to_value(extreme_ticks{1}), soml::duration{maximum}},
		{soml::to_value(extreme_ticks{2}), std::nullopt},
		{soml::to_value(extreme_ticks{-1}), soml::duration{-maximum}},
		{soml::to_value(extreme_ticks{-2}), std::nullopt},
		{soml::to_value(std::chrono::duration<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()}), std::nullopt},
	}};
	for (const auto& [input, expected] : cases) {
		if (input.has_value() != expected.has_value()) {
			return false;
		}
		if (input && input->as<soml::duration>() != expected) {
			return false;
		}
	}
	return true;
}

constexpr bool vector_input_ranges() {
	const std::vector<char> empty;
	const std::vector<char> valid{'[', '1', ']'};
	const std::vector<char> trailing_null{'[', '1', ']', '\0'};
	const auto parsed = soml::parse(valid);
	const auto invalid = soml::parse(trailing_null);
	return !soml::parse(empty) && parsed && (*parsed)[0].as<int>() == 1 && !invalid && invalid.error().offset == 3;
}

static_assert(vector_input_ranges());
static_assert(integer_appends());
static_assert(checked_durations());
static_assert(value_predicates());
static_assert(owned_string_values());
static_assert(hexadecimal_appends());
static_assert(lane_order());
static_assert(signed_magnitudes());

std::string reference_quoted(std::string_view text) {
	std::string output = "\"";
	for (const char character : text) {
		const auto byte = static_cast<unsigned char>(character);
		switch (byte) {
			case '"': {
				output += "\\\"";
				break;
			}
			case '\\': {
				output += "\\\\";
				break;
			}
			case '\b': {
				output += "\\b";
				break;
			}
			case '\f': {
				output += "\\f";
				break;
			}
			case '\n': {
				output += "\\n";
				break;
			}
			case '\r': {
				output += "\\r";
				break;
			}
			case '\t': {
				output += "\\t";
				break;
			}
			default: {
				if (byte < 0x20) {
					output += std::format("\\u{:04x}", byte);
				} else {
					output.push_back(static_cast<char>(byte));
				}
			}
		}
	}
	output.push_back('"');
	return output;
}

void scan_oracles(test_results& results) {
	std::uint32_t state = 731;
	for (std::size_t length = 0; length < 257; length++) {
		std::string text;
		for (std::size_t index = 0; index < length; index++) {
			state = state * 1664525U + 1013904223U;
			text.push_back(static_cast<char>(state >> 25U));
		}
		results.check("JSON quoting matches scalar oracle for mixed ASCII", soml::detail::json_quote(text) == reference_quoted(text));

		for (std::size_t start = 0; start <= text.size(); start++) {
			std::size_t expected = start;
			while (expected < text.size() && (text[expected] == ' ' || text[expected] == '\t')) {
				expected++;
			}
			results.check("whitespace scan matches scalar oracle at every starting offset", soml::detail::skip_spaces(text, start) == expected);
		}

		for (const char byte : {' ', '\t'}) {
			std::string padding(length, byte);
			for (const std::string_view suffix : {"", "x", "\n", "é", "😀", "\xFF"}) {
				results.check("whitespace prefixes stop before every nonblank kind", soml::detail::skip_spaces(padding + std::string{suffix}, 0) == length);
			}
		}

		// U+0027 selects escaped quotes, but double quotes and slashes only need escaping after that selection.
		std::erase(text, '\r');
		text += "'😀é";
		std::string written;
		soml::detail::append_string(written, text, soml::detail::classify_string(text));
		const auto parsed = soml::parse("[" + written + "]");
		results.check("mixed escaped strings round-trip after partial word skips", parsed && (*parsed)[0].as<std::string_view>() == text);
	}
}

void utf8_prefixes(test_results& results) {
	for (const std::string_view valid : {"é", "€", "😀", "\xE0\xA0\x80", "\xED\x9F\xBF", "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF"}) {
		for (std::size_t padding = 0; padding < 16; padding++) {
			const std::string prefix(padding, 'x');
			for (std::size_t kept = 1; kept <= valid.size(); kept++) {
				const std::string text = prefix + std::string{valid.substr(0, kept)};
				bool truncated = false;
				const auto length = soml::detail::utf8_sequence_length(text, padding, &truncated);
				results.check("valid UTF-8 prefixes report truncation only when incomplete", length == (kept == valid.size() ? kept : 0) && truncated == (kept != valid.size()));
			}
			for (int replacement = 0; replacement < 256; replacement++) {
				std::string text = prefix + std::string{valid.substr(0, 2)};
				text.back() = static_cast<char>(replacement);
				const bool continuation = replacement >= 0x80 && replacement <= 0xBF;
				const auto lead = static_cast<unsigned char>(valid.front());
				const bool valid_second = continuation && (lead != 0xE0 || replacement >= 0xA0) && (lead != 0xED || replacement <= 0x9F) && (lead != 0xF0 || replacement >= 0x90) && (lead != 0xF4 || replacement <= 0x8F);
				bool truncated = false;
				const auto length = soml::detail::utf8_sequence_length(text, padding, &truncated);
				results.check("invalid UTF-8 prefix bytes precede the truncation diagnostic", length == (valid_second && valid.size() == 2 ? 2 : 0) && truncated == (valid_second && valid.size() > 2));
			}
		}
	}
}

void abbreviation_oracle(test_results& results) {
	const std::array<std::string_view, 7> units{"a", "é", "😀", "\xFF", "\t", "€", "z"};
	for (std::size_t repetitions = 0; repetitions < 20; repetitions++) {
		std::string text;
		for (std::size_t index = 0; index < repetitions * units.size(); index++) {
			text += units[index % units.size()];
		}
		for (std::size_t maximum = 0; maximum < 150; maximum++) {
			std::string expected;
			for (std::size_t index = 0; index < std::min(maximum, repetitions * units.size()); index++) {
				expected += units[index % units.size()];
			}
			if (maximum < repetitions * units.size()) {
				expected += "…";
			}
			results.check("abbreviation preserves whole Unicode scalars and isolated invalid bytes", soml::detail::abbreviate(text, maximum) == expected);
		}
	}
	for (std::size_t length = 0; length < 200; length++) {
		for (std::size_t maximum = 0; maximum < 100; maximum++) {
			results.check("ASCII abbreviation stops at every word and limit boundary", soml::detail::abbreviate(std::string(length, 'a'), maximum) == std::string(std::min(length, maximum), 'a') + (length > maximum ? "…" : ""));
		}
	}
}

void exact_float_oracle(test_results& results) {
	std::uint64_t state = 17;
	for (std::uint64_t exponent = 0; exponent < 2047; exponent++) {
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		const std::uint64_t bits = (exponent << 52U) | (state & 0x800F'FFFF'FFFF'FFFF);
		const double number = std::bit_cast<double>(bits);
		std::array<char, 32> buffer;
		const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number, std::chars_format::scientific);
		std::string literal{buffer.data(), written.ptr};
		std::erase(literal, '+'); // SOML exponents do not accept a plus sign.
		double parsed = 0;
		results.check("exact decimal conversion round-trips every binary64 exponent", written.ec == std::errc{} && soml::detail::exact_float(literal, parsed) == soml::detail::float_status::ok && std::bit_cast<std::uint64_t>(parsed) == bits);
		std::string rendered;
		soml::detail::append_float(rendered, number);
		results.check("scientific digit extraction preserves every binary64 exponent", soml::detail::parse_float(rendered, parsed) == soml::detail::float_status::ok && std::bit_cast<std::uint64_t>(parsed) == bits);
		const auto shortest = soml::detail::find_shortest_digits(std::bit_cast<double>(bits & 0x7FFF'FFFF'FFFF'FFFF));
		const auto marker = literal.find('e');
		const int expected_count = static_cast<int>(marker - (literal.front() == '-' ? 1 : 0) - (literal.contains('.') ? 1 : 0));
		results.check("shortest digits omit the scientific point and exponent", shortest.count == expected_count);
	}
}

void unicode_scalars(test_results& results) {
	std::string output;
	output.reserve(4);
	for (char32_t scalar = 0; scalar <= 0x10FFFF; scalar++) {
		if (scalar >= 0xD800 && scalar <= 0xDFFF) {
			continue;
		}
		output.clear();
		soml::detail::append_utf8(output, scalar);
		const std::size_t length = soml::detail::utf8_sequence_length(output, 0);
		results.check("every Unicode scalar encodes as one valid UTF-8 sequence", length == output.size() && soml::detail::decode_utf8(output, 0, length) == scalar);
	}
	for (const auto& [scalar, expected] : std::array<std::pair<char32_t, std::string_view>, 10>{
		std::pair{U'\0', std::string_view{"\0", 1}},
		{0x7F, "\x7F"},
		{0x80, "\xC2\x80"},
		{0x7FF, "\xDF\xBF"},
		{0x800, "\xE0\xA0\x80"},
		{0xD7FF, "\xED\x9F\xBF"},
		{0xE000, "\xEE\x80\x80"},
		{0xFFFF, "\xEF\xBF\xBF"},
		{0x10000, "\xF0\x90\x80\x80"},
		{0x10FFFF, "\xF4\x8F\xBF\xBF"},
	}) {
		for (const std::size_t length : {0UZ, 14UZ, 15UZ, 16UZ, 21UZ, 22UZ, 23UZ, 24UZ, 128UZ}) {
			std::string appended(length, 'x');
			soml::detail::append_utf8(appended, scalar);
			results.check("UTF-8 appending preserves prefixes across short-string and growth boundaries", appended == std::string(length, 'x') + std::string{expected});
		}
	}
}

} // namespace

int main() {
	test_results results;
	results.check("integer appending preserves prefixes while growing for both 64-bit extremes", integer_appends());
	results.check("vector input preserves empty ranges and trailing null bytes", vector_input_ranges());
	results.check("checked duration conversion rejects overflow across signed boundaries and extreme period ratios", checked_durations());
	results.check("zero and empty values retain exactly one of the ten types", value_predicates());
	results.check("string constructors own their bytes and views preserve embedded nulls", owned_string_values());
	for (const std::size_t length : {0UZ, 14UZ, 15UZ, 16UZ, 21UZ, 22UZ, 23UZ, 24UZ, 128UZ}) {
		std::string source(length, 'a');
		const soml::value view{std::string_view{source}};
		const soml::value pointer{source.c_str()};
		source.assign(length, 'b');
		results.check("string values own their input across short-string boundaries", view.as<std::string_view>() == std::string(length, 'a') && pointer.as<std::string_view>() == std::string(length, 'a'));
	}
	results.check("marked lanes follow native byte order for all 256 masks", lane_order());
	results.check("signed magnitudes cover both int64 extremes", signed_magnitudes());
	for (std::int64_t number = std::numeric_limits<std::int16_t>::min(); number <= std::numeric_limits<std::int16_t>::max(); number++) {
		results.check("signed magnitude conversion covers every int16", soml::detail::to_signed(soml::detail::magnitude(number), number < 0) == number);
	}
	scan_oracles(results);
	utf8_prefixes(results);
	abbreviation_oracle(results);
	exact_float_oracle(results);
	unicode_scalars(results);
	results.check("hexadecimal appending preserves prefix and pads wide fields", hexadecimal_appends());
	std::println("{} value refactor checks, {} failures", results.count, results.failures);
	return results.failures == 0 ? 0 : 1;
}
