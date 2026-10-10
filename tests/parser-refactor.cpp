#include <soml.hpp>

#include <chrono>
#include <cmath>
#include <print>

namespace {

constexpr bool utf8_input_equivalence() {
	for (const unsigned int byte : {0U, 1U, 9U, 10U, 0x7FU, 0x80U, 0xC0U, 0xC2U, 0xDFU, 0xE0U, 0xEDU, 0xEFU, 0xF0U, 0xF4U, 0xF5U, 0xFFU}) {
		const std::array<char8_t, 5> utf8_bytes{u8'[', u8'\'', static_cast<char8_t>(byte), u8'\'', u8']'};
		const std::array<char, 5> bytes{'[', '\'', static_cast<char>(byte), '\'', ']'};
		if (soml::parse(std::u8string_view{utf8_bytes.data(), utf8_bytes.size()}) != soml::parse(std::string_view{bytes.data(), bytes.size()})) {
			return false;
		}
	}
	const auto unicode = soml::parse(u8"name: 'é 🦀'\nzero: \"\\u{0}\"");
	return soml::parse(std::u8string_view{}) == soml::parse(std::string_view{}) && unicode && (*unicode)["name"].as<std::string_view>() == "é 🦀" && (*unicode)["zero"].as<std::string_view>() == std::string_view{"\0", 1};
}

static_assert(utf8_input_equivalence());

constexpr bool instant_fraction_scaling() {
	constexpr std::string_view digits = "123456789";
	for (std::size_t length = 0; length <= digits.size(); length++) {
		std::int32_t expected_nanoseconds = 0;
		for (std::size_t digit = 0; digit < 9; digit++) {
			expected_nanoseconds = (expected_nanoseconds * 10) + (digit < length ? digits[digit] - '0' : 0);
		}
		for (const std::string_view offset : {"Z", "+05:45", "-03:30"}) {
			const auto literal = "[2024-02-29T12:34:56" + (length == 0 ? std::string{} : "." + std::string{digits.substr(0, length)}) + std::string{offset} + "]";
			const auto parsed = soml::parse(literal);
			if (!parsed) {
				return false;
			}
			const auto instant = (*parsed)[0].as<soml::instant>();
			const auto day = std::chrono::sys_days{std::chrono::year{2024} / std::chrono::February / 29};
			const auto time = day + std::chrono::hours{12} + std::chrono::minutes{34} + std::chrono::seconds{56};
			const auto offset_seconds = offset == "Z" ? 0 : offset == "+05:45" ? 20'700 : -12'600;
			const auto expected_seconds = std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count() - offset_seconds;
			if (!instant || instant->seconds != expected_seconds || instant->nanoseconds != expected_nanoseconds) {
				return false;
			}
		}
	}
	return true;
}

constexpr bool integer_separator_radices() {
	constexpr std::array<std::pair<std::string_view, std::int64_t>, 8> cases{{
		{"9_223_372_036_854_775_807", std::numeric_limits<std::int64_t>::max()},
		{"-9_223_372_036_854_775_808", std::numeric_limits<std::int64_t>::min()},
		{"0x7FFF_FFFF_FFFF_FFFF", std::numeric_limits<std::int64_t>::max()},
		{"0o7_777_777_777_777_777_777_77", std::numeric_limits<std::int64_t>::max()},
		{"0b111_111", 63},
		{"12_3_456_7_89", 123'456'789},
		{"-12_3_456_7_89", -123'456'789},
		{"0xA_B_C_D_E_F", 0xABCDEF},
	}};
	for (const auto& [literal, expected] : cases) {
		const auto parsed = soml::parse("[" + std::string{literal} + "]");
		if (!parsed || (*parsed)[0].as<std::int64_t>() != expected) {
			return false;
		}
	}
	for (const std::string_view literal : {"0xFFFF_FFFF_FFFF_FFFF_FFFF", "0xFFFFFFFFFFFFFFFFFFFFFFFF", "18_446_744_073_709_551_616", "18446744073709551616", "0o1_000_000_000_000_000_000_000"}) {
		const auto parsed = soml::parse("[" + std::string{literal} + "]");
		if (parsed || !parsed.error().message.contains("outside the 64-bit range")) {
			return false;
		}
	}
	return true;
}

constexpr bool sparse_duration_fractions() {
	constexpr std::array<std::pair<std::string_view, std::int64_t>, 4> cases{{
		{"0.000000000005h", 18},
		{"0.010000000005h", 36'000'000'018},
		{"0.100000001s", 100'000'001},
		{"0.000000000000000000000000000000000000h", 0},
	}};
	for (const auto& [literal, expected] : cases) {
		const auto parsed = soml::parse("[" + std::string{literal} + "]");
		if (!parsed || (*parsed)[0].as<soml::duration>() != soml::duration{expected}) {
			return false;
		}
	}
	const auto too_precise = soml::parse("[0.0000000000005h]");
	return !too_precise && too_precise.error().message.contains("not a whole number of nanoseconds");
}

constexpr bool grammar_after_overflow() {
	constexpr std::array<std::pair<std::string_view, std::string_view>, 5> cases{{
		{"18446744073709551616h0m1d", "there is no day unit"},
		{"18446744073709551616h0m1h", "the units are in the order"},
		{"18446744073709551616h0m1.5s1ns", "only the last part may have a fraction"},
		{"18446744073709551616h0m0.0000000001s", "not a whole number of nanoseconds"},
		{"18446744073709551616h0m0s", "outside the 64-bit range"},
	}};
	for (const auto& [literal, message] : cases) {
		const auto parsed = soml::parse("[" + std::string{literal} + "]");
		if (parsed || !parsed.error().message.contains(message)) {
			return false;
		}
	}
	for (const std::string_view literal : {"[1/*/", "[1/* outer /* nested */]"}) {
		const auto parsed = soml::parse(literal);
		if (parsed || !parsed.error().message.contains(literal == "[1/*/" ? "Unterminated block comment" : "cannot be nested")) {
			return false;
		}
	}
	return soml::parse("[1/* ordinary / * / text */]").has_value() && soml::parse("[1/* outer /*/ ]").has_value();
}

constexpr bool exponent_float_examples() {
	constexpr std::array<std::pair<std::string_view, double>, 8> cases{{
		{"1e3", 1000.0},
		{"1.5e-2", 0.015},
		{"-1e22", -1e22},
		{"1e-22", 1e-22},
		{"999999999999999e0", 999999999999999.0},
		{"1.00000000000001e-8", 1.00000000000001e-8},
		{"-0e0", 0.0},
		{"-0.0e-22", 0.0},
	}};
	for (const auto& [literal, expected] : cases) {
		const auto parsed = soml::parse("[" + std::string{literal} + "]");
		if (!parsed || (*parsed)[0].type() != soml::type::floating || std::bit_cast<std::uint64_t>(*(*parsed)[0].as<double>()) != std::bit_cast<std::uint64_t>(expected)) {
			return false;
		}
	}
	return true;
}

constexpr bool unicode_key_hints() {
	for (const std::string_view key : {"é", "漢", "🐱", "$é漢🐱"}) {
		const auto rejected = soml::parse(std::string{key} + ": 1");
		if (rejected || rejected.error().offset != 0 || !rejected.error().message.contains("as in '" + std::string{key} + "'")) {
			return false;
		}
	}
	return true;
}

bool exponent_float_oracle() {
	for (int exponent = -23; exponent <= 23; exponent++) {
		for (const std::string_view mantissa : {"0", "-0", "1", "-1", "1.5", "-0.0001", "999999999999999", "9999999999999999", "1000000000000000", "1234567.89012345"}) {
			const std::string literal = std::string{mantissa} + "e" + std::to_string(exponent);
			double expected = 0;
			const auto converted = std::from_chars(literal.data(), literal.data() + literal.size(), expected);
			const auto parsed = soml::parse("[" + literal + "]");
			if (!converted || !parsed || (*parsed)[0].type() != soml::type::floating || (*parsed)[0].as<double>() != expected || (expected == 0 && std::signbit(*(*parsed)[0].as<double>()))) {
				return false;
			}
		}
	}
	return true;
}

bool control_word_boundaries() {
	for (std::size_t alignment = 0; alignment < 16; alignment++) {
		for (int control = 0; control <= 0x7F; control++) {
			if (!soml::detail::is_control(control)) {
				continue;
			}
			const std::string prefix = "['" + std::string(alignment, 'a');
			const std::string document = prefix + static_cast<char>(control) + std::string(24, 'a') + "']";
			const auto rejected = soml::parse(document);
			if (rejected || rejected.error().offset != prefix.size()) {
				return false;
			}
			const auto invalid_utf8 = soml::parse(document + static_cast<char>(0xFF));
			if (invalid_utf8 || invalid_utf8.error().offset != document.size() || !invalid_utf8.error().message.contains("Invalid UTF-8")) {
				return false;
			}
		}
	}
	return true;
}

constexpr bool duration_unit_spellings() {
	for (const auto& unit : soml::detail::duration_units) {
		if (soml::detail::duration_unit::find(unit.name) != &unit) {
			return false;
		}
	}
	for (const std::string_view name : {"", "H", "M", "S", "MS", "US", "NS", "mS", "uS", "nS", "hs", "ss", "mss", "seconds"}) {
		if (soml::detail::duration_unit::find(name) != nullptr) {
			return false;
		}
	}
	return true;
}

bool scanner_word_boundaries() {
	for (std::size_t alignment = 0; alignment < 16; alignment++) {
		for (int byte = 0; byte < 256; byte++) {
			const std::string text = std::string(alignment + 1, 'a') + static_cast<char>(byte) + std::string(16, 'a');
			std::size_t key_end = 0;
			while (key_end < text.size() && soml::detail::is_bare_key_character(text[key_end])) {
				key_end++;
			}
			if (soml::detail::skip_bare_key(text, 0) != key_end) {
				return false;
			}
			for (const char quote : {'\'', '"'}) {
				std::size_t string_end = 0;
				while (string_end < text.size() && text[string_end] != quote && text[string_end] != '\n' && (quote != '"' || text[string_end] != '\\')) {
					string_end++;
				}
				if (soml::detail::skip_string_characters(text, 0, quote) != string_end) {
					return false;
				}
			}
		}
	}
	return true;
}

bool nested_member_ownership() {
	std::string source = "records: {\n";
	for (int index = 0; index < 70; index++) {
		source += "'record" + std::to_string(index) + "': {'" + std::string(80, 'k') + std::to_string(index) + "': '" + std::string(96, 'v') + std::to_string(index) + "'}\n";
	}
	source += '}';
	const auto parsed = soml::parse(source);
	source.assign(source.size(), 'x');
	if (!parsed) {
		return false;
	}
	const auto records = (*parsed)["records"].as<soml::object>();
	if (!records || records->size() != 70) {
		return false;
	}
	for (std::size_t index = 0; index < records->size(); index++) {
		const auto& outer = records->members()[index];
		const auto record = outer.value.as<soml::object>();
		if (outer.key != "record" + std::to_string(index) || !record || record->size() != 1) {
			return false;
		}
		const auto& entry = record->members()[0];
		if (entry.key != std::string(80, 'k') + std::to_string(index) || entry.value.as<std::string_view>() != std::string(96, 'v') + std::to_string(index)) {
			return false;
		}
	}
	return true;
}

static_assert(exponent_float_examples());
static_assert(unicode_key_hints());
static_assert(instant_fraction_scaling());
static_assert(integer_separator_radices());
static_assert(grammar_after_overflow());
static_assert(sparse_duration_fractions());
static_assert(duration_unit_spellings());

constexpr bool non_key_missing_values() {
	for (const std::string_view character : {"}", "@", "/", "💥"}) {
		const auto parsed = soml::parse(soml::detail::concat("a:\n", character));
		if (parsed || parsed.error().code != soml::error_code::syntax || parsed.error().line != 2 || parsed.error().column != 1 || parsed.error().message != soml::detail::concat("Expected a value, but found “", character, "”")) {
			return false;
		}
	}
	const auto nested = soml::parse("a:\n[}");
	return !nested && nested.error().line == 2 && nested.error().column == 2 && nested.error().message == "Expected a value, but found “}”";
}

static_assert(non_key_missing_values());

} // namespace

int main() {
	const bool passed = utf8_input_equivalence() && instant_fraction_scaling() && integer_separator_radices() && grammar_after_overflow() && sparse_duration_fractions() && scanner_word_boundaries() && duration_unit_spellings() && control_word_boundaries() && exponent_float_oracle() && exponent_float_examples() && unicode_key_hints() && non_key_missing_values() && nested_member_ownership();
	std::println("Parser numeric oracles, UTF-8 scanning, and diagnostic precedence: {}", passed ? "passed" : "failed");
	return passed ? 0 : 1;
}
