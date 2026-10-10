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

struct byte_case {
	std::string_view name;
	std::string_view bytes;
};

struct boxed_count {
	std::int64_t count;
};

static_assert(!soml::encodable<std::chrono::duration<boxed_count>>);
static_assert(!soml::decodable<std::chrono::duration<boxed_count>>);
static_assert(!std::constructible_from<soml::instant, std::chrono::sys_time<std::chrono::duration<boxed_count>>>);

// These probe each malformed UTF-8 class through separate public trust boundaries.
constexpr std::array malformed{
	byte_case{"lowest stray continuation", "\x80"},
	byte_case{"highest stray continuation", "\xBF"},
	byte_case{"overlong NUL", "\xC0\x80"},
	byte_case{"overlong slash", "\xC0\xAF"},
	byte_case{"overlong two-byte maximum", "\xC1\xBF"},
	byte_case{"two-byte missing continuation", "\xC2"},
	byte_case{"two-byte ASCII continuation", "\xC2\x41"},
	byte_case{"two-byte leading-byte continuation", "\xDF\xC2"},
	byte_case{"three-byte overlong minimum", "\xE0\x80\x80"},
	byte_case{"three-byte overlong maximum", "\xE0\x9F\xBF"},
	byte_case{"three-byte missing both continuations", "\xE1"},
	byte_case{"three-byte missing final continuation", "\xE1\x80"},
	byte_case{"three-byte ASCII final continuation", "\xE1\x80\x41"},
	byte_case{"first high surrogate", "\xED\xA0\x80"},
	byte_case{"last high surrogate", "\xED\xAF\xBF"},
	byte_case{"first low surrogate", "\xED\xB0\x80"},
	byte_case{"last low surrogate", "\xED\xBF\xBF"},
	byte_case{"four-byte overlong minimum", "\xF0\x80\x80\x80"},
	byte_case{"four-byte overlong maximum", "\xF0\x8F\xBF\xBF"},
	byte_case{"four-byte missing all continuations", "\xF1"},
	byte_case{"four-byte missing two continuations", "\xF1\x80"},
	byte_case{"four-byte missing final continuation", "\xF1\x80\x80"},
	byte_case{"four-byte ASCII final continuation", "\xF1\x80\x80\x41"},
	byte_case{"first scalar above Unicode", "\xF4\x90\x80\x80"},
	byte_case{"obsolete five-byte lead", "\xF8\x88\x80\x80\x80"},
	byte_case{"obsolete six-byte lead", "\xFC\x84\x80\x80\x80\x80"},
	byte_case{"reserved FE lead", "\xFE"},
	byte_case{"reserved FF lead", "\xFF"},
};

void malformed_boundaries(test_results& results) {
	for (const auto& [name, bytes] : malformed) {
		bool parses_safely = true;
		// Cover every alignment of the eight-byte fast scan, including an invalid sequence at EOF.
		for (std::size_t padding = 0; padding < 8; padding++) {
			const std::string prefix = "valid: 1\n# " + std::string(padding, 'x');
			const auto parsed = soml::parse(prefix + std::string{bytes});
			parses_safely = parses_safely && !parsed && parsed.error().code == soml::error_code::syntax && parsed.error().offset == prefix.size() && parsed.error().line == 2 && parsed.error().column == padding + 3;
		}
		results.check(std::format("parser comment boundary: {}", name), parses_safely);

		bool preserves_output = true;
		const soml::value invalid = soml::object{{"outer", soml::array{0, soml::object{{"payload", std::string{bytes}}}}}};
		for (const bool canonical : {false, true}) {
			std::string output = "keep this destination\n";
			const auto written = canonical ? soml::write_canonical(invalid, output) : soml::write(invalid, output);
			preserves_output = preserves_output && !written && written.error().code == soml::error_code::invalid_value && written.error().path == "outer[1].payload" && output == "keep this destination\n";
		}
		results.check(std::format("nested invalid string append rollback: {}", name), preserves_output);

		std::string key;
		for (int repeat = 0; repeat < 1000; repeat++) {
			key += bytes;
		}
		const auto written = soml::write(soml::object{{key, 1}});
		results.check(std::format("invalid key bounded valid diagnostic: {}", name), !written && written.error().code == soml::error_code::invalid_value && written.error().path.size() < 1250 && soml::detail::classify_string(written.error().description()) != soml::detail::string_kind::invalid_utf8);
	}
}

template<class T>
void strict_conversion(test_results& results, std::string_view target_name, soml::type target_kind) {
	const std::array<soml::value, 10> values{
		nullptr, true, -17, 1.5, "text", soml::date{std::chrono::year{2026}, std::chrono::month{11}, std::chrono::day{1}}, soml::instant{0, 123}, soml::duration{123}, soml::array{1}, soml::object{{"key", 1}},
	};
	for (const auto& value : values) {
		const auto converted = soml::from_value<T>(value);
		bool correct = converted.has_value() == (value.type() == target_kind);
		if (converted) {
			correct = correct && soml::to_value(*converted) == value;
		} else {
			correct = correct && converted.error().code == soml::error_code::type_mismatch && converted.error().path.empty() && converted.error().line == 0;
		}
		results.check(std::format("strict conversion {} to {}", soml::to_string(value.type()), target_name), correct);
	}
}

void key_identity(test_results& results) {
	const std::array<std::string, 16> keys{
		"", "a.b", "[0]", "quote'", "quote\"", "both'\"", "back\\slash", "line\nbreak", "tab\tkey", std::string{"nul\0key", 7}, "é", "é", "😀", " space ", "__proto__", "a:#/*b*/",
	};
	for (const auto& key : keys) {
		const soml::value source = soml::object{{"target", soml::object{{key, 1}, {"sibling", 7}}}, {"keep", 9}};
		const auto text = soml::write(source).value();
		auto expected = source;
		*expected.find("target")->find(key) = soml::array{2, 3};
		const auto replaced = soml::edit(text, {"target", key}, soml::array{2, 3});
		results.check(std::format("exact nested replacement for key {}", soml::detail::json_quote(key)), replaced && soml::parse(*replaced) == expected && replaced->ends_with("keep: 9\n"));

		expected = source;
		expected.find("target")->get_if<soml::object>()->erase(key);
		const auto removed = soml::edit(text, {"target", key}, std::nullopt);
		results.check(std::format("exact nested removal for key {}", soml::detail::json_quote(key)), removed && soml::parse(*removed) == expected && removed->ends_with("keep: 9\n"));
	}
}

void temporal_boundaries(test_results& results) {
	using namespace std::chrono;
	const std::array<std::int64_t, 20> counts{
		std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(), -3'600'000'000'001, -3'600'000'000'000, -3'599'999'999'999, -60'000'000'001, -60'000'000'000, -59'999'999'999, -1'000'000'001, -1'000'000'000, -999'999'999, -1, 0, 1, 999'999'999, 1'000'000'000, 1'000'000'001, 59'999'999'999, 60'000'000'000, 3'600'000'000'000,
	};
	for (const auto count : counts) {
		const soml::value source = soml::array{soml::duration{count}};
		const auto encoded = soml::write(source);
		const auto converted = soml::from_value<seconds>(soml::value{soml::duration{count}});
		results.check(std::format("duration nanoseconds {} retain exactness", count), encoded && soml::parse(*encoded) == source && converted.has_value() == (count % 1'000'000'000 == 0) && (!converted || converted->count() == count / 1'000'000'000));
	}

	struct instant_case {
		std::string_view text;
		soml::instant expected;
	};
	const std::array cases{
		instant_case{"0001-01-01T00:00:00Z", soml::instant::min()},
		instant_case{"9999-12-31T23:59:59.999999999Z", soml::instant::max()},
		instant_case{"1969-12-31T23:59:59.999999999Z", {-1, 999'999'999}},
		instant_case{"1970-01-01T00:00:00.000000001Z", {0, 1}},
		instant_case{"1970-01-01T00:00:00.1Z", {0, 100'000'000}},
		instant_case{"1970-01-01T00:00:00.01Z", {0, 10'000'000}},
		instant_case{"1970-01-01T00:00:00.001Z", {0, 1'000'000}},
		instant_case{"1970-01-01T00:00:00.0001Z", {0, 100'000}},
		instant_case{"1970-01-01T00:00:00.00001Z", {0, 10'000}},
		instant_case{"1970-01-01T00:00:00.000001Z", {0, 1000}},
		instant_case{"1970-01-01T00:00:00.0000001Z", {0, 100}},
		instant_case{"1970-01-01T00:00:00.00000001Z", {0, 10}},
		instant_case{"1970-01-01T00:00:00+01:00", {-3600, 0}},
		instant_case{"1970-01-01T00:00:00-01:00", {3600, 0}},
		instant_case{"1970-01-01T00:00:00+23:59", {-86'340, 0}},
		instant_case{"1970-01-01T00:00:00-23:59", {86'340, 0}},
		instant_case{"2000-02-29T00:00:00Z", {951'782'400, 0}},
		instant_case{"1900-03-01T00:00:00Z", {-2'203'891'200, 0}},
	};
	for (const auto& [text, expected] : cases) {
		const auto parsed = soml::parse("time: " + std::string{text});
		const auto encoded = parsed ? soml::write_canonical(*parsed) : std::expected<std::string, soml::error>{std::unexpected(parsed.error())};
		results.check(std::format("instant exact UTC normalization {}", text), parsed && (*parsed)["time"].as<soml::instant>() == expected && encoded && soml::parse(*encoded) == *parsed);
	}
}

void code_frame_security(test_results& results) {
	// Diagnostics render these controls visibly while preserving their scalar column.
	constexpr std::array<char32_t, 16> scalars{
		0, 1, 0x1B, 0x7F, 0x80, 0x9F, 0x061C, 0x200E, 0x200F, 0x202A, 0x202B, 0x202C, 0x202D, 0x202E, 0x2066, 0x2069,
	};
	for (const auto scalar : scalars) {
		std::string text = "a: '";
		soml::detail::append_utf8(text, scalar);
		text += "'";
		const soml::error error{.message = {}, .offset = 4, .line = 1, .column = 5, .path = {}};
		results.check(std::format("code frame replaces U+{:04X}", static_cast<std::uint32_t>(scalar)), error.code_frame(text) == "> 1 | a: '�'\n    |     ^");
	}
}

void array_diagnostic_positions(test_results& results) {
	for (const std::size_t index : {0UZ, 9UZ, 10UZ, 99UZ, 100UZ, 999UZ}) {
		std::string text = "[";
		for (std::size_t preceding = 0; preceding < index; preceding++) {
			text += "[1],";
		}
		const std::size_t offset = text.size() + 1;
		text += "['invalid']]";
		const auto decoded = soml::decode<std::vector<std::vector<int>>>(text);
		results.check(std::format("nested array diagnostic locates item {}", index), !decoded && decoded.error().code == soml::error_code::type_mismatch && decoded.error().path == std::format("[{}][0]", index) && decoded.error().offset == offset && decoded.error().line == 1 && decoded.error().column == offset + 1);
	}
}

void escaped_string_batches(test_results& results) {
	for (int byte = 0; byte < 128; byte++) {
		if (byte == '\r') {
			continue;
		}
		bool correct = true;
		for (std::size_t alignment = 0; alignment < 8; alignment++) {
			const std::string prefix = "'" + std::string(alignment, 'a');
			const std::string suffix(24, 'b');
			const std::string content = prefix + static_cast<char>(byte) + suffix;
			std::string escaped;
			if (byte == '"' || byte == '\\') {
				escaped = std::string{"\\"} + static_cast<char>(byte);
			} else if (byte == '\n') {
				escaped = "\\n";
			} else if (byte == '\t') {
				escaped = "\\t";
			} else if (byte < 0x20 || byte == 0x7F) {
				escaped = std::format("\\u{{{:x}}}", byte);
			} else {
				escaped = static_cast<char>(byte);
			}
			const auto written = soml::write(soml::array{content});
			correct = correct && written == "[\n\t\"" + prefix + escaped + suffix + "\"\n]\n";
		}
		results.check(std::format("escaped ASCII U+{:04X} at every batch alignment", byte), correct);
	}
	for (const char32_t scalar : {char32_t{0x80}, char32_t{0x89}, char32_t{0x9F}, char32_t{0xA2}, char32_t{0x700}, char32_t{0x7F0}, char32_t{0x20AC}, char32_t{0x1F600}}) {
		std::string bytes;
		soml::detail::append_utf8(bytes, scalar);
		bool correct = true;
		for (std::size_t alignment = 0; alignment < 8; alignment++) {
			const std::string content = "'" + std::string(alignment, 'a') + bytes + std::string(24, 'b');
			const auto written = soml::write(soml::array{content});
			correct = correct && written == "[\n\t\"" + content + "\"\n]\n" && soml::parse(*written) == soml::value{soml::array{content}};
		}
		results.check(std::format("UTF-8 U+{:04X} stays unescaped across byte batches", static_cast<std::uint32_t>(scalar)), correct);
	}
}

void nan_payloads(test_results& results) {
	for (const std::uint64_t sign : {0ULL, 0x8000'0000'0000'0000ULL}) {
		for (const std::uint64_t payload : {1ULL, 0x0007'FFFF'FFFF'FFFFULL, 0x0008'0000'0000'0000ULL, 0x000F'FFFF'FFFF'FFFFULL}) {
			const double number = std::bit_cast<double>(sign | 0x7FF0'0000'0000'0000ULL | payload);
			const auto written = soml::write(soml::array{number});
			results.check(std::format("NaN sign {:x} payload {:x} is rejected", sign, payload), soml::detail::is_nan(number) && !written && written.error().code == soml::error_code::invalid_value && written.error().path == "[0]");
		}
		for (const std::uint64_t magnitude : {0ULL, 1ULL, 0x7FEF'FFFF'FFFF'FFFFULL, 0x7FF0'0000'0000'0000ULL}) {
			const double number = std::bit_cast<double>(sign | magnitude);
			results.check(std::format("binary64 sign {:x} magnitude {:x} is valid", sign, magnitude), !soml::detail::is_nan(number) && soml::write(soml::array{number}).has_value());
		}
	}
}

} // namespace

int main() {
	test_results results;
	malformed_boundaries(results);
	strict_conversion<std::nullptr_t>(results, "null", soml::type::null);
	strict_conversion<bool>(results, "bool", soml::type::boolean);
	strict_conversion<std::int64_t>(results, "int", soml::type::integer);
	strict_conversion<double>(results, "float", soml::type::floating);
	strict_conversion<std::string>(results, "string", soml::type::string);
	strict_conversion<soml::date>(results, "date", soml::type::date);
	strict_conversion<soml::instant>(results, "instant", soml::type::instant);
	strict_conversion<soml::duration>(results, "duration", soml::type::duration);
	strict_conversion<soml::array>(results, "array", soml::type::array);
	strict_conversion<soml::object>(results, "object", soml::type::object);
	key_identity(results);
	temporal_boundaries(results);
	code_frame_security(results);
	array_diagnostic_positions(results);
	escaped_string_batches(results);
	nan_payloads(results);
	std::println("{} named hardening checks, {} failures", results.count, results.failures);
	return results.failures == 0 ? 0 : 1;
}
