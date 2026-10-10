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

struct long_named_field {
	int count = 0;
};

constexpr auto long_name = [] {
	std::array<char, 4096> name{};
	name.fill('a');
	return name;
}();

} // namespace

template<>
inline constexpr auto soml::fields<long_named_field> = std::tuple{
	soml::field{std::string_view{long_name.data(), long_name.size()}, &long_named_field::count},
};

static_assert(soml::decodable<soml::array> && soml::encodable<soml::array>);
static_assert(soml::decodable<soml::object> && soml::encodable<soml::object>);
static_assert(soml::decodable<std::string> && soml::encodable<std::string>);
static_assert(soml::decode<soml::array>("[1, true]")->size() == 2);
static_assert(soml::to_string(static_cast<soml::type>(255)) == "unknown");
static_assert(soml::to_string(static_cast<soml::error_code>(0)) == "unknown");
static_assert(soml::detail::json_quote("plain😀\"\\\n") == "\"plain😀\\\"\\\\\\n\"");
static_assert(!soml::detail::has_digits("123", std::numeric_limits<std::size_t>::max(), 2));

namespace {

void diagnostic_windows(test_results& results) {
	const std::string text = std::string(150, 'a') + std::string(150, 'b') + std::string(150, 'c');
	const std::array cases{
		std::pair{0UZ, "> 1 | " + std::string(100, 'a') + "…\n    | ^"},
		std::pair{225UZ, "> 1 | …" + std::string(100, 'b') + "…\n    | " + std::string(51, ' ') + "^"},
		std::pair{450UZ, "> 1 | …" + std::string(100, 'c') + "\n    | " + std::string(101, ' ') + "^"},
	};
	for (const auto& [offset, expected] : cases) {
		const soml::error failure{.message = {}, .offset = offset, .line = 1, .column = offset + 1, .path = {}};
		results.check(std::format("ASCII diagnostic window at {}", offset), failure.code_frame(text) == expected);
	}

	const std::string mixed = std::string(80, 'a') + "\t😀" + std::string(80, 'b');
	const soml::error emoji{.message = {}, .offset = 81, .line = 1, .column = 82, .path = {}};
	const std::string expected = "> 1 | …" + std::string(49, 'a') + "\t😀" + std::string(49, 'b') + "…\n    | " + std::string(50, ' ') + "\t^";
	results.check("Unicode window keeps tab-aligned caret", emoji.code_frame(mixed) == expected);

	// The context line is truncated independently of the error line's byte offset.
	const std::string context = std::string(1'000'000, 'x') + "\nbad";
	const soml::error last{.message = {}, .offset = 1'000'001, .line = 2, .column = 1, .path = {}};
	results.check("long preceding context line is cropped", last.code_frame(context) == "  1 | " + std::string(100, 'x') + "…\n> 2 | bad\n    | ^");
}

void json_chunks(test_results& results) {
	const std::array cases{
		std::pair{std::string{}, std::string{"\"\""}},
		std::pair{std::string{"a😀b\"c\\d\ne\tf\bg\fh\ri"}, std::string{"\"a😀b\\\"c\\\\d\\ne\\tf\\bg\\fh\\ri\""}},
		std::pair{std::string{"a\xFF" "😀\xC2" "b"}, std::string{"\"a\\u00ff😀\\u00c2b\""}},
		std::pair{std::string{"\0a\x1F" "b", 5}, std::string{"\"\\u0000a\\u001fb\\u0000\""}},
	};
	for (const auto& [text, expected] : cases) {
		results.check("JSON escaping flushes plain and invalid UTF-8 runs", soml::detail::json_quote(text) == expected);
	}

	const std::string ordinary(8192, 'x');
	results.check("JSON escaping preserves long surrounding runs", soml::detail::json_quote(ordinary + "\"😀\x80" + ordinary) == '"' + ordinary + "\\\"😀\\u0080" + ordinary + '"');
}

void fractional_rendering(test_results& results) {
	const std::array cases{
		std::pair{1, ".000000001"}, std::pair{10, ".00000001"}, std::pair{100, ".0000001"},
		std::pair{1000, ".000001"}, std::pair{10'000, ".00001"}, std::pair{100'000, ".0001"},
		std::pair{1'000'000, ".001"}, std::pair{10'000'000, ".01"}, std::pair{100'000'000, ".1"},
		std::pair{101'000'001, ".101000001"}, std::pair{999'999'999, ".999999999"},
	};
	for (const auto& [nanoseconds, spelling] : cases) {
		const soml::instant instant{-1, nanoseconds};
		results.check("instant fraction preserves leading zeros and trims trailing zeros", std::format("{}", instant) == "1969-12-31T23:59:59" + std::string{spelling} + 'Z');
		const auto written = soml::write(soml::array{soml::duration{nanoseconds}, soml::duration{-nanoseconds}});
		results.check("positive and negative subsecond durations round-trip", written && *written == "[\n\t0" + std::string{spelling} + "s\n\t-0" + spelling + "s\n]\n" && soml::parse(*written) == soml::value{soml::array{soml::duration{nanoseconds}, soml::duration{-nanoseconds}}});
	}
}

void bounded_suggestions(test_results& results) {
	std::string misspelled{long_name.data(), long_name.size()};
	misspelled.back() = 'b';
	const auto failure = soml::decode<long_named_field>(misspelled + ": 1");
	results.check("huge equal-length keys skip quadratic typo suggestions", !failure && failure.error().code == soml::error_code::unknown_key && !failure.error().message.contains("Did you mean") && failure.error().line == 1 && failure.error().column == 1);
	const auto accepted = soml::decode<long_named_field>(std::string{long_name.data(), long_name.size()} + ": 7");
	results.check("bounding suggestions does not bound accepted key lengths", accepted && accepted->count == 7);
}

void enum_bounds(test_results& results) {
	constexpr std::array<std::string_view, 10> type_names{"null", "bool", "int", "float", "string", "date", "instant", "duration", "array", "object"};
	constexpr std::array<std::string_view, 6> error_names{"syntax", "too_deep", "invalid_value", "type_mismatch", "out_of_range", "unknown_key"};
	for (unsigned index = 0; index <= 255; index++) {
		const std::string_view expected_type = index < type_names.size() ? type_names[index] : "unknown";
		const std::string_view expected_error = index > 0 && index <= error_names.size() ? error_names[index - 1] : "unknown";
		results.check("type names reject all out-of-table enum values", soml::to_string(static_cast<soml::type>(index)) == expected_type);
		results.check("error names reject zero and out-of-table enum values", soml::to_string(static_cast<soml::error_code>(index)) == expected_error);
	}
}

} // namespace

int main() {
	test_results results;
	diagnostic_windows(results);
	json_chunks(results);
	fractional_rendering(results);
	bounded_suggestions(results);
	enum_bounds(results);
	std::println("{} refinement checks, {} failures", results.count, results.failures);
	return results.failures == 0 ? 0 : 1;
}
