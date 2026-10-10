/*
Unit tests. Run them from the repository root, because the float tests read files in `tests/fixtures`.
*/
#include <soml.hpp>

#include <cfenv>
#include <ctime>
#include <chrono>
#include <cstdio>
#include <deque>
#include <flat_map>
#include <fstream>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <print>
#include <source_location>
#include <sstream>
#include <thread>
#include <unordered_map>

using namespace std::literals;

namespace {

int failure_count = 0;

void check(bool condition, const std::source_location location = std::source_location::current()) {
	if (!condition) {
		std::println(stderr, "{}:{}: check failed", location.file_name(), location.line());
		failure_count++;
	}
}

template<class Actual, class Expected>
void check_equal(const Actual& actual, const Expected& expected, const std::source_location location = std::source_location::current()) {
	if (actual == expected) {
		return;
	}

	if constexpr (std::formattable<Actual, char> && std::formattable<Expected, char>) {
		std::println(stderr, "{}:{}: expected {}, got {}", location.file_name(), location.line(), expected, actual);
	} else {
		std::println(stderr, "{}:{}: values differ", location.file_name(), location.line());
	}

	failure_count++;
}

soml::value parse(std::string_view text, const std::source_location location = std::source_location::current()) {
	auto result = soml::parse(text);

	if (!result) {
		std::println(stderr, "{}:{}: parse failed: {}", location.file_name(), location.line(), result.error());
		failure_count++;
		return {};
	}

	return std::move(*result);
}

soml::error parse_error(std::string_view text, const std::source_location location = std::source_location::current()) {
	auto result = soml::parse(text);

	if (result) {
		std::println(stderr, "{}:{}: parse succeeded, but should fail", location.file_name(), location.line());
		failure_count++;
		return {};
	}

	return result.error();
}

std::string canonical_text(const soml::value& value, const std::source_location location = std::source_location::current()) {
	auto result = soml::write_canonical(value);

	if (!result) {
		std::println(stderr, "{}:{}: write failed: {}", location.file_name(), location.line(), result.error());
		failure_count++;
		return {};
	}

	return *result;
}

std::string read_file(const char* path) {
	std::ifstream file{path, std::ios::binary};
	return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::vector<std::string> read_lines(const char* path) {
	std::vector<std::string> lines;
	std::istringstream stream{read_file(path)};
	std::string line;

	while (std::getline(stream, line)) {
		lines.push_back(line);
	}

	return lines;
}

std::string canonical_float(double number) {
	std::string output;
	soml::detail::append_float(output, number);
	return output;
}

// MARK: Compile time

constexpr std::optional<std::int64_t> compile_time_port() {
	auto document = soml::parse("server: {port: 8080}");
	return document ? (*document)["server"]["port"].as<std::int64_t>() : std::nullopt;
}

static_assert(compile_time_port() == 8080);
static_assert(soml::parse("a: 0.1")->find("a")->as<double>() == 0.1);
static_assert(soml::parse("a: 5e-324")->find("a")->as<double>() == 5e-324);
static_assert(soml::parse("a: 1.7976931348623157e308")->find("a")->as<double>() == 1.7976931348623157e308);
static_assert(soml::parse("a: 2.2250738585072011e-308")->find("a")->as<double>() == 2.2250738585072011e-308);
static_assert(*soml::write_canonical(*soml::parse("b: 1\na: [0.1, 1e21, 5e-324, 123.456, -0.0, 1h30m, 2026-09-19T14:00:00.5+07:00]")) == "a: [\n\t0.1\n\t1e21\n\t5e-324\n\t123.456\n\t0.0\n\t1h30m\n\t2026-09-19T07:00:00.5Z\n]\nb: 1\n");
static_assert(soml::parse("a: 1\na: 2").error().line == 2);
static_assert(soml::parse("a: 1e999").error().message == "1e999 is too large to be a finite float. Use infinity if you mean it");
static_assert(soml::parse(u8"name: 'å'")->find("name")->as<std::string_view>() == "å");
static_assert(soml::is_bare_key("content-type") && !soml::is_bare_key("a.b") && !soml::is_bare_key(""));
static_assert(std::formattable<soml::error, char> && std::formattable<soml::instant, char> && std::formattable<soml::type, char> && std::formattable<soml::error_code, char>);

struct compile_time_point {
	int x = 0;
	int y = 0;
};

} // namespace

template<>
inline constexpr auto soml::fields<compile_time_point> = std::tuple{
	soml::field{"x", &compile_time_point::x},
	soml::field{"y", &compile_time_point::y},
};

namespace {

static_assert(soml::decode<compile_time_point>("x: 1\ny: 2")->y == 2);
static_assert(soml::decode<compile_time_point>("x: 1\ny: 'bad'").error().line == 2);
static_assert(soml::decode<std::vector<int>>("[1, 'bad']").error().column == 5);
static_assert(soml::decode<compile_time_point>("unknown: 1").error().column == 1);
static_assert(*soml::encode(compile_time_point{.x = 3, .y = 4}) == "x: 3\ny: 4\n");

// MARK: Reading

void test_values() {
	const soml::value document = parse(R"(
/* The edge service. */
name: 'api-gateway'
replicas: 3
ratio: 0.75
enabled: true
owner: null
timeout: 1m30s
deployed: 2026-09-19T14:00:00Z
labels: ['prod', 'eu-west']
postgres: {host: 'db.internal', port: 5432}
)");

	check(document.is_object());
	check_equal(document.size(), 9UZ);
	check_equal(document["name"].value_or("none"), "api-gateway"sv);
	check_equal(document["replicas"].as<int>(), std::optional<int>{3});
	check_equal(document["ratio"].as<double>(), std::optional<double>{0.75});
	check_equal(document["enabled"].as<bool>(), std::optional<bool>{true});
	check(document["owner"].has_value() && document["owner"]->is_null());
	check(!document["missing"].has_value());
	check(!document["missing"]["deeper"][3].has_value());
	check_equal(document["timeout"].as<std::chrono::seconds>(), std::optional{std::chrono::seconds{90}});
	check(!document["timeout"].as<std::chrono::minutes>().has_value());
	check_equal(document["deployed"].as<std::chrono::sys_seconds>(), std::optional{std::chrono::sys_days{std::chrono::year{2026} / 9 / 19} + std::chrono::hours{14}});
	check_equal(document["labels"][1].value_or(""), "eu-west"sv);
	check(!document["labels"][2].has_value());
	check_equal(document["postgres"]["port"].value_or(0), 5432);
	check_equal(document["postgres"]["port"].as<std::uint8_t>(), std::optional<std::uint8_t>{});
	check_equal(document["postgres"].type(), std::optional{soml::type::object});

	// An int is not a float, and a float is not an int.
	check(!document["replicas"].as<double>().has_value());
	check(!document["ratio"].as<int>().has_value());

	const auto* labels = document.find("labels")->get_if<soml::array>();
	check(labels != nullptr && labels->size() == 2);
	check(document.get_if<soml::array>() == nullptr);

	// Members keep their order.
	std::vector<std::string> keys;

	for (const auto& [key, item] : *document.get_if<soml::object>()) {
		keys.push_back(key);
	}

	check_equal(keys.front(), "name"s);
	check_equal(keys.back(), "postgres"s);
}

void test_input_kinds() {
	const std::u8string text = u8"name: 'Ærøskøbing'";
	check_equal(soml::parse(text)->find("name")->as<std::string>(), std::optional{"Ærøskøbing"s});

	const std::array bytes{std::byte{'a'}, std::byte{':'}, std::byte{' '}, std::byte{'1'}};
	check_equal(soml::parse(std::span{bytes})->find("a")->as<int>(), std::optional{1});

	// The text does not need to outlive the value.
	soml::value document;

	{
		std::string source = "a: 'a long string that does not fit in the small string buffer'";
		document = parse(source);
		source.assign(source.size(), 'x');
	}

	check_equal(document["a"].value_or(""), "a long string that does not fit in the small string buffer"sv);
}

void test_errors() {
	const soml::error duplicate = parse_error("name: 'a'\nport: 1\nname: 'b'\n");
	check_equal(duplicate.code, soml::error_code::syntax);
	check_equal(duplicate.message, "Duplicate key name"s);
	check_equal(duplicate.line, 3UZ);
	check_equal(duplicate.column, 1UZ);
	check_equal(duplicate.offset, 18UZ);
	check_equal(duplicate.description(), "Duplicate key name at line 3, column 1"s);
	check_equal(std::format("{}", duplicate), "Duplicate key name at line 3, column 1"s);
	check_equal(std::format("{}", std::vector{soml::type::integer, soml::type::object}), R"(["int", "object"])"s);
	check_equal(duplicate.code_frame("name: 'a'\nport: 1\nname: 'b'\n"), "  1 | name: 'a'\n  2 | port: 1\n> 3 | name: 'b'\n    | ^"s);

	// The column counts Unicode scalar values, and the caret keeps tabs.
	const soml::error unicode = parse_error("a: 'å'\n\tb: 'ø' c");
	check_equal(unicode.line, 2UZ);
	check_equal(unicode.column, 9UZ);
	check_equal(unicode.code_frame("a: 'å'\n\tb: 'ø' c"), "  1 | a: 'å'\n> 2 | \tb: 'ø' c\n    | \t       ^"s);

	// A raw control character is shown as U+FFFD in a code frame.
	const soml::error control = parse_error("a: '\x1B[31m'");
	check_equal(control.message, "A raw control character (U+001B) is not allowed anywhere, including in strings and comments. In a string, write it as the escape \\u{1b} inside \"...\""s);
	check_equal(control.code_frame("a: '\x1B[31m'"), "> 1 | a: '\uFFFD[31m'\n    |     ^"s);
	const std::string invalid_utf8 = "a: '\xFF'";
	check_equal(parse_error(invalid_utf8).code_frame(invalid_utf8), "> 1 | a: '\uFFFD'\n    |     ^"s);
	const std::string stray_continuation = "a: '\x80x'";
	check_equal(parse_error(stray_continuation).code_frame(stray_continuation), "> 1 | a: '\uFFFDx'\n    |     ^"s);

	// A long line is cut to a window around the caret.
	const std::string long_line = std::format("a: [{}x]", std::string(1000, '1'));
	const std::string frame = parse_error(long_line).code_frame(long_line);
	check(frame.size() < 300);
	check(frame.contains("…") && frame.ends_with("^"));

	check_equal(parse_error("").message, "A document must contain an object or an array, but this one is empty"s);
	check_equal(parse_error("5").message, "A bare value is not a document. A document is an object or an array, so write it as `key: value` or `[value]`"s);
	check_equal(parse_error("\xEF\xBB\xBF" "a: 1").message, "A byte order mark (BOM) is not allowed"s);
	check_equal(parse_error("a: 1\r\n").message, "A carriage return (U+000D) is not allowed anywhere. Use LF line endings"s);
	check_equal(parse_error("a: '\xC3'").message, "Invalid UTF-8 byte 0xC3"s);
	check_equal(parse_error("a: '\xE2\x82").message, "Incomplete UTF-8 sequence at the end of the input"s);
	check_equal(parse_error("a: '\xED\xA0\x80'").message, "Invalid UTF-8 byte 0xED"s);
	check_equal(parse_error("a: '\xC0\xAF'").message, "Invalid UTF-8 byte 0xC0"s);
	check_equal(parse_error("timeout: 30 seconds").message, "A unit cannot follow a number after a space. Write a duration without the space, as in 10s, and anything else, such as a size, as a string, as in '30 seconds'"s);
	check_equal(parse_error("a: [1,000]").message, "Leading zeros are not allowed in a decimal number. A comma separates items, so 1,000 is not one number. Write it as 1_000 or 1000"s);
	check_equal(parse_error("mode: 0644").message, "Leading zeros are not allowed in a decimal number. Write an octal number, such as a file mode, as 0o644, and an identifier, such as a ZIP code, as a string: '0644'"s);
	check_equal(parse_error("color: 0xff").message, "Hexadecimal digits are uppercase: 0xFF"s);
	check_equal(parse_error("a: 2026-09-19 14:00:00Z").message, "The date and time separator in an instant is an uppercase “T”, not a space, as in 2026-09-19T14:00:00Z"s);
	check_equal(parse_error("example.com: 1").message, "A bare key cannot contain “.”. Quote it, as in 'example.com', or use braces to nest, as in example: {com: …}"s);
	check_equal(parse_error("a: \"\\uD83D\\uDE00\"").message, "The four-digit \\uD83D\\uDE00 form is not an escape. Write \\u{1f600}"s);
	check_equal(parse_error("a: 1\u00A0").message, "Unexpected U+00A0 (NO-BREAK SPACE; only space, tab, and line feed are whitespace) after a value"s);

	// The value of an uppercase \u{…} escape is checked before its spelling, and a hint for a number, a duration, or an instant is only given for a token it fits.
	check_equal(parse_error("a: \"\\u{D}\"").message, "A carriage return (U+000D) cannot be represented, so \\u{D} is not allowed"s);
	check_equal(parse_error("a: \"\\u{FFFFFF}\"").message, "\\u{FFFFFF} is above U+10FFFF, the largest Unicode scalar value"s);
	check_equal(parse_error("a: 2026-09-19T14:00:00 Z").message, "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00Z"s);
	check_equal(parse_error("a: 2026-02-30T14:00:00 Z").message, "An instant's offset follows its time directly, without a space"s);
	check_equal(parse_error("a: 2026-09-19T14:00:00 +02:00 # é").message, "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00+02:00"s);
	check_equal(parse_error("mac: 00:1A:2B:3C:4D:5E").message, "Invalid number “00:1A:2B:3C:4D:5E”. A string value must be quoted, as in '00:1A:2B:3C:4D:5E'"s);
	check_equal(parse_error("a: 2years").message, "Invalid duration 2years: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"s);
	check_equal(parse_error("a: 1byte").message, "Invalid number “1byte”. A string value must be quoted, as in '1byte'"s);
	check_equal(parse_error("a: -nano").message, "Expected a digit or “infinity” after “-”"s);
	check_equal(parse_error("a: -info").message, "Expected a digit or “infinity” after “-”"s);
	check_equal(parse_error("speed: 100 km/h").message, "Expected a line break before the next entry, but found “k”"s);
	check_equal(parse_error("a: 2026-09-19T14:00:00/2026-09-20T15:00:00").message, "An instant needs an offset: Z or ±HH:MM"s);
	check_equal(parse_error("a: 2026-09-19T14:00:00 Z/x").message, "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, as in '2026-09-19T14:00:00', or add the offset it was meant in"s);
	check_equal(parse_error("a: 4k_video").message, "Invalid number “4k_video”. A string value must be quoted, as in '4k_video'"s);
	check_equal(parse_error("price: 5EUR").message, "Invalid number “5EUR”. A string value must be quoted, as in '5EUR'"s);
}

void test_depth() {
	const auto nested = [](std::size_t depth) {
		return std::string(depth, '[') + std::string(depth, ']');
	};

	check(soml::parse(nested(100)).has_value());
	const soml::error error = parse_error(nested(101));
	check_equal(error.code, soml::error_code::too_deep);
	check_equal(error.column, 101UZ);

	// The top-level object counts as a level, with or without braces.
	check(soml::parse("a: " + nested(99)).has_value());
	check_equal(parse_error("a: " + nested(100)).code, soml::error_code::too_deep);

	// The writer refuses what the reader refuses.
	soml::value deep = soml::array{};

	for (int level = 1; level < 100; level++) {
		deep = soml::array{std::move(deep)};
	}

	check(soml::write(deep).has_value());
	deep = soml::array{std::move(deep)};
	check_equal(soml::write(deep).error().code, soml::error_code::too_deep);

	// A secondary thread, whose stack is smaller than the main thread's, reads the deepest documents, and deep hostile input fails cleanly.
	std::jthread worker{[&] {
		check(soml::parse(nested(100)).has_value());
		check(!soml::parse(nested(1'000'000)).has_value());
	}};
}

void test_duplicates() {
	// A large object finds a duplicate by sorting, and reports the first in the text.
	std::string text;

	for (int index = 0; index < 1000; index++) {
		text += std::format("key{}: {}\n", index, index);
	}

	check(soml::parse(text).has_value());
	const soml::error error = parse_error(text + "key999: 1\nkey5: 2\nkey5: 3\n");
	check_equal(error.message, "Duplicate key key999"s);
	check_equal(error.line, 1001UZ);

	// A duplicate before a later error in the same object is reported first, as a reader that checks each member as it reads reports it.
	check_equal(parse_error(text + "key3: 1\nbad: [\n").message, "Duplicate key key3"s);
	check_equal(parse_error("a: 1\nb: {c: 1, c: 2}\na: 2").message, "Duplicate key c"s);
	check_equal(parse_error("a: 1\na: 2\nb: {c: 1, c: 2}").message, "Duplicate key a"s);
	check_equal(parse_error("a: 1\nb: {c: 1, c: 2, d: [}\na: 2").message, "Duplicate key c"s);

	// A closed object's members are not compared with the members of the object around it.
	check_equal(parse_error("'': 1\nt: {b: 1, '': 2, '': 3}").column, 18UZ);
	check_equal(parse_error("a: 1\nb: [{c: 1, c: 2}]\na: 2").message, "Duplicate key c"s);

	// Uniqueness is decided on the decoded key.
	check_equal(parse_error("a: 1\n\"\\u{61}\": 2").message, "Duplicate key a"s);
	check(soml::parse("'a': 1\n'a\u200B': 2").has_value());
}

// MARK: Writing

void test_writing() {
	const soml::value document = soml::object{
		{"name", "api"},
		{"replicas", 3},
		{"ratio", 0.5},
		{"labels", soml::array{"prod", "it's", "tab\there", "back\\slash", "quote\"d", "line\nbreak", "\x01\x7F"}},
		{"empty", soml::array{}},
		{"nested", soml::object{{"z", nullptr}, {"a", false}}},
		{"the key", soml::object{}},
		{"timeout", soml::duration{1500ms}},
		{"deployed", std::chrono::sys_days{std::chrono::year{2026} / 9 / 19}},
	};

	check_equal(*soml::write(document), "name: 'api'\nreplicas: 3\nratio: 0.5\nlabels: [\n\t'prod'\n\t\"it's\"\n\t\"tab\\there\"\n\t'back\\slash'\n\t'quote\"d'\n\t\"line\\nbreak\"\n\t\"\\u{1}\\u{7f}\"\n]\nempty: []\nnested: {\n\tz: null\n\ta: false\n}\n'the key': {}\ntimeout: 1.5s\ndeployed: 2026-09-19T00:00:00Z\n"s);
	check_equal(canonical_text(document), "deployed: 2026-09-19T00:00:00Z\nempty: []\nlabels: [\n\t'prod'\n\t\"it's\"\n\t\"tab\\there\"\n\t'back\\slash'\n\t'quote\"d'\n\t\"line\\nbreak\"\n\t\"\\u{1}\\u{7f}\"\n]\nname: 'api'\nnested: {\n\ta: false\n\tz: null\n}\nratio: 0.5\nreplicas: 3\n'the key': {}\ntimeout: 1.5s\n"s);
	check_equal(canonical_text(soml::object{}), "{}\n"s);
	check_equal(canonical_text(soml::array{}), "[]\n"s);
	check_equal(canonical_text(soml::array{1, soml::object{{"a", 1}}}), "[\n\t1\n\t{\n\t\ta: 1\n\t}\n]\n"s);

	// Durations, with the spec's examples.
	const auto duration_text = [](soml::duration length) {
		std::string output;
		soml::detail::append_duration(output, length);
		return output;
	};

	check_equal(duration_text(90min), "1h30m"s);
	check_equal(duration_text(1500ms), "1.5s"s);
	check_equal(duration_text(250us), "0.00025s"s);
	check_equal(duration_text(0s), "0s"s);
	check_equal(duration_text(-5min), "-5m"s);
	check_equal(duration_text(soml::duration::min()), "-2562047h47m16.854775808s"s);

	// Floats, with the spec's examples of rule 11.
	check_equal(canonical_float(1.0), "1.0"s);
	check_equal(canonical_float(1e20), "100000000000000000000.0"s);
	check_equal(canonical_float(1e21), "1e21"s);
	check_equal(canonical_float(1.5), "1.5"s);
	check_equal(canonical_float(0.000001), "0.000001"s);
	check_equal(canonical_float(1e-7), "1e-7"s);
	check_equal(canonical_float(1.5e300), "1.5e300"s);
	check_equal(canonical_float(5e-324), "5e-324"s);
	check_equal(canonical_float(-0.0), "0.0"s);
	check_equal(canonical_float(-std::numeric_limits<double>::infinity()), "-infinity"s);
	check_equal(canonical_float(9007199254740993.0), "9007199254740992.0"s);
}

void test_writer_checks() {
	const auto write_error = [](const soml::value& document) {
		auto result = soml::write(document);
		check(!result.has_value());
		return result ? soml::error{} : result.error();
	};

	const soml::error nan = write_error(soml::object{{"servers", soml::array{soml::object{{"weight", 1.0}}, soml::object{{"weight", std::numeric_limits<double>::quiet_NaN()}}}}});
	check_equal(nan.code, soml::error_code::invalid_value);
	check_equal(nan.path, "servers[1].weight"s);
	check_equal(nan.description(), "servers[1].weight: NaN is not a SOML value"s);

	check_equal(write_error(soml::object{{"a", "carriage\rreturn"}}).message, "A string holds a carriage return (U+000D), which SOML cannot represent"s);
	check_equal(write_error(soml::object{{"a", "\xFF"}}).message, "A string is not valid UTF-8"s);
	check_equal(write_error(soml::object{{"bad\xFF", 1}}).path, "[\"bad\\u00ff\"]"s);
	check_equal(write_error(soml::object{{"a", soml::instant{soml::instant::max().seconds + 1, 0}}}).code, soml::error_code::invalid_value);
	check_equal(write_error(soml::object{{"a", soml::instant{0, 1'000'000'000}}}).code, soml::error_code::invalid_value);
	check_equal(write_error(1).message, "A document must be an object or an array, but this is int"s);

	// Keys are mutable through iterators, so the writer checks for duplicates, in small and large objects.
	soml::object members{{"a", 1}, {"b", 2}};
	members.begin()[1].key = "a";
	check_equal(write_error(members).message, "An object has the key a more than once"s);

	soml::object large;

	for (int index = 0; index < 100; index++) {
		large.insert_or_assign(std::format("k{}", index), index);
	}

	check(soml::write(large).has_value());
	large.begin()[50].key = "k7";
	check_equal(write_error(large).message, "An object has the key k7 more than once"s);
}

void test_round_trip() {
	const std::string text = read_file("tests/fixtures/example.soml");
	const soml::value document = parse(text);
	const std::string canonical = canonical_text(document);
	const soml::value again = parse(canonical);
	check(document == again);
	check_equal(canonical_text(again), canonical);
	check(parse(*soml::write(document)) == document);
}

// MARK: Values

void test_object() {
	soml::object members{{"b", 1}, {"a", 2}, {"b", 3}};
	check_equal(members.size(), 2UZ);
	check_equal(members.begin()->key, "b"s);
	check(*members.find("b") == soml::value{3});

	members["c"] = "new";
	check_equal(members.size(), 3UZ);
	check(members.contains("c"));
	members.insert_or_assign("a", 4);
	check(*members.find("a") == soml::value{4});
	check(members.erase("b"));
	check(!members.erase("b"));
	check_equal(members.members().front().key, "a"s);

	// Equality ignores member order, and an int is never equal to a float.
	check(soml::value{soml::object{{"a", 1}, {"b", 2}}} == soml::value{soml::object{{"b", 2}, {"a", 1}}});
	check(soml::value{soml::object{{"a", 1}}} != soml::value{soml::object{{"a", 1.0}}});
	check(soml::value{0.0} == soml::value{-0.0});
	check(soml::value{soml::object{{"a", 1}}} != soml::value{soml::object{{"b", 1}}});

	soml::object left;
	soml::object right;

	for (int index = 0; index < 50; index++) {
		left.insert_or_assign(std::format("k{}", index), index);
		right.insert_or_assign(std::format("k{}", 49 - index), 49 - index);
	}

	check(left == right);
	right["k3"] = 4;
	check(left != right);
}

// A pointer, a `char`, an unsigned 64-bit integer, or an enum does not silently become a bool or an int.
static_assert(!std::is_constructible_v<soml::value, std::size_t>);
static_assert(!std::is_constructible_v<soml::value, std::uint64_t>);
static_assert(!std::is_constructible_v<soml::value, soml::detail::int128> && !soml::encodable<soml::detail::int128>);
static_assert(!std::is_constructible_v<soml::value, char>);
static_assert(!std::is_constructible_v<soml::value, const int*>);
static_assert(!std::is_constructible_v<soml::value, const char8_t*>);
static_assert(!std::is_constructible_v<soml::value, soml::type>);
static_assert(std::is_constructible_v<soml::value, bool> && std::is_constructible_v<soml::value, std::uint32_t> && std::is_constructible_v<soml::value, std::int64_t>);

// Keys that do not own their text cannot be decoded, because the document is gone when decoding returns.
static_assert(!soml::decodable<std::map<std::string_view, int>>);
static_assert(soml::encodable<std::map<std::string_view, int>>);
static_assert(soml::decodable<std::chrono::sys_time<std::chrono::duration<double>>> && !soml::encodable<std::chrono::sys_time<std::chrono::duration<double>>>);

// The errors for an int out of range work in constant expressions too.
static_assert(!soml::from_value<std::uint8_t>(soml::value{300}).has_value());
static_assert(soml::from_value<std::int8_t>(soml::value{-129}).error().message == "The int -129 is outside the range of the type, -128 to 127");
static_assert(!soml::to_value(std::numeric_limits<std::uint64_t>::max()).has_value());

void test_assignment() {
	// A value can be assigned a part of itself.
	soml::value document = soml::object{{"server", soml::object{{"host", "a long host name that needs an allocation of its own"}, {"port", 80}}}};
	document = *document.find("server");
	check_equal(document["port"].as<int>(), std::optional{80});

	document = soml::object{{"server", soml::object{{"host", "a long host name that needs an allocation of its own"}, {"port", 80}}}};
	document = std::move(*document.find("server"));
	check_equal(document["host"].value_or(""), "a long host name that needs an allocation of its own"sv);

	soml::object members{{"inner", soml::object{{"a", 1}}}};
	members = *members.find("inner")->get_if<soml::object>();
	check(members.contains("a"));

	members = soml::object{{"inner", soml::object{{"a", 1}}}};
	members = std::move(*members.find("inner")->get_if<soml::object>());
	check(members.contains("a"));

	// A view compares values, not addresses.
	const soml::value port = *soml::parse("port: 80\nother: 80");
	check(port["port"] == soml::value{80});
	check(port["port"] == port["other"]);
	check(port["port"] != port["missing"]);
	check(port["missing"] == port["absent"]);
}

void test_instant() {
	const soml::instant instant = soml::parse("a: 2026-09-19T14:00:00.123456789+02:00")->find("a")->as<soml::instant>().value();
	check_equal(std::format("{}", instant), "2026-09-19T12:00:00.123456789Z"s);
	check_equal(instant.to_sys_time<std::chrono::nanoseconds>()->time_since_epoch().count(), 1'789'819'200'123'456'789LL);
	check(!instant.to_sys_time<std::chrono::seconds>().has_value());
	check(soml::instant{std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{-1}}} == soml::instant(-1, 999'999'999));

	// Floating counts may lose nanoseconds; integral subnanosecond times floor toward the past when encoded.
	using floating_seconds = std::chrono::duration<double>;
	const soml::instant precise{1'700'000'000, 1};
	check(precise.to_sys_time<floating_seconds>()->time_since_epoch().count() == 1'700'000'000.0);
	check(soml::from_value<std::chrono::sys_time<floating_seconds>>(soml::value{precise})->time_since_epoch().count() == 1'700'000'000.0);
	using picoseconds = std::chrono::duration<std::int64_t, std::pico>;
	check(soml::to_value(std::chrono::sys_time<picoseconds>{picoseconds{1500}}).value() == soml::value{soml::instant{0, 1}});
	check(soml::to_value(std::chrono::sys_time<picoseconds>{picoseconds{-1500}}).value() == soml::value{soml::instant{-1, 999'999'998}});

	// The ends of the range do not fit nanoseconds since 1970, but do fit seconds.
	check(!soml::instant::max().to_sys_time<std::chrono::nanoseconds>().has_value());
	check(soml::instant::min().to_sys_time<std::chrono::days>().has_value());
	check_equal(std::format("{}", soml::instant::min()), "0001-01-01T00:00:00Z"s);
	check_equal(std::format("{}", soml::instant::max()), "9999-12-31T23:59:59.999999999Z"s);
	check_equal(std::format("{}", soml::instant{0, -1}), "invalid instant"s);
	check(soml::instant::min() < soml::instant::max());

	// The ends of `std::chrono::sys_time<std::chrono::nanoseconds>` read and write exactly, and a time beyond 64-bit seconds is not valid rather than wrapped.
	using nanosecond_time = std::chrono::sys_time<std::chrono::nanoseconds>;

	for (const nanosecond_time time : {nanosecond_time::min(), nanosecond_time::max(), nanosecond_time{std::chrono::nanoseconds{-1}}}) {
		const soml::instant converted{time};
		check(converted.is_valid() && converted.nanoseconds >= 0 && converted.nanoseconds < 1'000'000'000);
		check(converted.to_sys_time<std::chrono::nanoseconds>() == time);
		check(soml::from_value<nanosecond_time>(*soml::to_value(time)) == time);
	}

	check(!soml::instant{std::chrono::sys_time<std::chrono::hours>{std::chrono::hours{std::numeric_limits<std::int64_t>::max() / 2}}}.is_valid());
	check_equal(soml::to_value(std::chrono::sys_time<std::chrono::hours>{std::chrono::hours{std::numeric_limits<std::int64_t>::max() / 2}}).error().code, soml::error_code::out_of_range);

	// A period that is not a power of ten.
	using three_halves = std::chrono::duration<std::int64_t, std::ratio<3, 2>>;
	check(soml::instant(1, 500'000'000).to_sys_time<three_halves>() == std::chrono::sys_time<three_halves>{three_halves{1}});
	check(soml::instant{std::chrono::sys_time<three_halves>{three_halves{-1}}} == soml::instant(-2, 500'000'000));
	check(soml::to_value(std::chrono::duration<std::int64_t, std::ratio<1, 3>>{15'000'000'000}).value() == soml::value{soml::duration{std::chrono::seconds{5'000'000'000}}});

	// Potentially overflowing chrono durations use checked conversion instead of direct construction.
	static_assert(!std::constructible_from<soml::value, std::chrono::hours>);
	check(!soml::to_value(std::chrono::hours{3'000'000}));
	check(!soml::to_value(std::chrono::hours{-3'000'000}));
}

// MARK: Floats

/*
Rewrites a decimal literal in a common form, such as `+1.5E+07` or `0012.5`, as `0.<digits>e<exponent>` in SOML syntax.
*/
std::string soml_literal(std::string_view text) {
	std::string digits;
	std::int64_t point = 0;
	bool is_negative = false;
	bool has_seen_point = false;
	std::size_t index = 0;

	if (text[index] == '-' || text[index] == '+') {
		is_negative = text[index] == '-';
		index++;
	}

	for (; index < text.size() && text[index] != 'e' && text[index] != 'E'; index++) {
		if (text[index] == '.') {
			has_seen_point = true;
			continue;
		}

		if (digits.empty() && text[index] == '0') {
			point -= has_seen_point ? 1 : 0;
			continue;
		}

		digits.push_back(text[index]);
		point += has_seen_point ? 0 : 1;
	}

	if (index < text.size()) {
		point += std::stoll(std::string{text.substr(index + 1)});
	}

	if (digits.empty()) {
		return "0.0";
	}

	return std::format("{}0.{}e{}", is_negative ? "-" : "", digits, point);
}

void check_parse_corpus(const char* cases_path, const char* expected_path) {
	const std::vector<std::string> cases = read_lines(cases_path);
	const std::vector<std::string> expected = read_lines(expected_path);
	check(cases.size() == expected.size() && cases.size() > 7000);
	std::size_t failures = 0;

	for (std::size_t index = 0; index < cases.size(); index++) {
		// The expected bits are little-endian bytes in hexadecimal.
		std::uint64_t bits = 0;

		for (int byte = 7; byte >= 0; byte--) {
			bits = (bits << 8U) | std::stoull(expected[index].substr(static_cast<std::size_t>(byte) * 2, 2), nullptr, 16);
		}

		const std::string literal = soml_literal(cases[index]);
		const std::uint64_t magnitude = bits & ~(std::uint64_t{1} << 63U);

		// The reader's path and the exact path, which constant evaluation uses, must both be right.
		double fast = 0.0;
		const soml::detail::float_status fast_status = soml::detail::parse_float(literal, fast);
		double exact = 0.0;
		const soml::detail::float_status exact_status = literal == "0.0" ? soml::detail::float_status::ok : soml::detail::exact_float(literal, exact);

		for (const auto& [status, number] : {std::pair{fast_status, fast}, std::pair{exact_status, exact}}) {
			bool is_correct = false;

			if (magnitude == 0x7FF0'0000'0000'0000) {
				is_correct = status == soml::detail::float_status::too_large;
			} else if (magnitude == 0) {
				is_correct = status == soml::detail::float_status::too_small || literal == "0.0";
			} else {
				is_correct = status == soml::detail::float_status::ok && std::bit_cast<std::uint64_t>(number) == bits;
			}

			if (!is_correct && failures++ < 5) {
				std::println(stderr, "Float {:.60}… read as {:016x}, expected {:016x}", literal, std::bit_cast<std::uint64_t>(number), bits);
			}
		}
	}

	check_equal(failures, 0UZ);
}

void test_float_parsing() {
	check_parse_corpus("tests/fixtures/float-parse-cases.txt", "tests/fixtures/float-parse-expected.txt");
	check_parse_corpus("tests/fixtures/float-parse-long-cases.txt", "tests/fixtures/float-parse-long-expected.txt");

	// A halfway value followed by 800 zeros rounds to even, and a tail beyond 800 digits decides a near-halfway value.
	std::string literal = "9007199254740993." + std::string(800, '0');
	check_equal(parse(std::format("a: {}", literal))["a"].as<double>(), std::optional{9007199254740992.0});
	literal.back() = '1';
	check_equal(parse(std::format("a: {}", literal))["a"].as<double>(), std::optional{9007199254740994.0});

	// Underscores, and a long run of leading zeros.
	check_equal(parse("a: 1_000.000_5")["a"].as<double>(), std::optional{1000.0005});
	check_equal(parse("a: 1_2.3_4e1_0")["a"].as<double>(), std::optional{12.34e10});
	check_equal(parse(std::format("a: 0.{}1e600", std::string(600, '0')))["a"].as<double>(), std::optional{0.1});
	check_equal(parse("a: 1.7976931348623158e308")["a"].as<double>(), std::optional{1.7976931348623157e308});
	check_equal(parse_error("a: 1.7976931348623159e308").message, "1.7976931348623159e308 is too large to be a finite float. Use infinity if you mean it"s);
	check_equal(parse_error("a: -1e400").message, "-1e400 is too large to be a finite float. Use -infinity if you mean it"s);
	check_equal(parse_error("a: 2e-324").message, "2e-324 is too small to be told apart from zero. Write 0.0 if you mean zero"s);
	check_equal(parse_error("a: 1e-99999999999999999999").message, "1e-99999999999999999999 is too small to be told apart from zero. Write 0.0 if you mean zero"s);
	check(std::bit_cast<std::uint64_t>(*parse("a: -0.0")["a"].as<double>()) == 0);
	check(std::bit_cast<std::uint64_t>(*parse("a: -0.000e5")["a"].as<double>()) == 0);
}

void test_rounding_modes() {
	// Clinger's fast path and libc++'s `std::from_chars` depend on the rounding mode, so another mode takes the exact path.
	for (const int mode : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
		std::fesetround(mode);
		const std::optional<double> short_number = soml::parse("a: 0.1")->find("a")->as<double>();
		const std::optional<double> long_number = soml::parse("a: 0.10000000000000000555")->find("a")->as<double>();
		std::fesetround(FE_TONEAREST);
		check_equal(short_number, std::optional{0.1});
		check_equal(long_number, std::optional{0.1});
	}
}

void test_float_formatting() {
	const std::vector<std::string> bits_lines = read_lines("tests/fixtures/float-shortest-bits.txt");
	const std::vector<std::string> expected_lines = read_lines("tests/fixtures/float-shortest-expected.txt");
	check(bits_lines.size() == expected_lines.size() && bits_lines.size() > 30'000);
	std::size_t failures = 0;

	for (std::size_t index = 0; index < bits_lines.size(); index++) {
		const std::uint64_t bits = std::stoull(bits_lines[index], nullptr, 16);
		const std::size_t space = expected_lines[index].find(' ');
		const std::string expected_digits = expected_lines[index].substr(0, space);
		const std::int64_t expected_exponent = std::stoll(expected_lines[index].substr(space + 1));
		const auto number = std::bit_cast<double>(bits);

		// The writer's path, and the exact path that constant evaluation uses.
		const soml::detail::shortest_digits shortest = soml::detail::find_shortest_digits(number);
		const std::string digits{shortest.digits.data(), static_cast<std::size_t>(shortest.count)};
		const std::int64_t exponent = shortest.point - shortest.count;
		soml::detail::decimal exact;
		exact.assign_shortest(bits);
		std::string exact_digits;

		for (int digit = 0; digit < exact.count; digit++) {
			exact_digits.push_back(static_cast<char>('0' + exact.digits[static_cast<std::size_t>(digit)]));
		}

		const std::int64_t exact_exponent = exact.point - exact.count;

		if ((digits != expected_digits || exponent != expected_exponent || exact_digits != expected_digits || exact_exponent != expected_exponent) && failures++ < 5) {
			std::println(stderr, "Float {:016x} written as {}e{} and {}e{}, expected {}e{}", bits, digits, exponent, exact_digits, exact_exponent, expected_digits, expected_exponent);
		}

		// The canonical text reads back as the same float.
		const std::string text = canonical_float(number);
		double back = 0.0;

		if (soml::detail::parse_float(text, back) != soml::detail::float_status::ok || std::bit_cast<std::uint64_t>(back) != bits) {
			if (failures++ < 5) {
				std::println(stderr, "Float {:016x} written as {} reads back differently", bits, text);
			}
		}
	}

	check_equal(failures, 0UZ);
}

// MARK: Typed mapping

enum class level : std::uint8_t {
	debug,
	info,
	warning,
};

struct database {
	std::string host = "localhost";
	std::uint16_t port = 5432;
};

struct service {
	std::string name;
	int replicas = 1;
	double ratio = 1.0;
	std::chrono::seconds timeout{30};
	std::optional<std::string> owner;
	std::vector<std::string> labels;
	std::map<std::string, int> limits;
	level log_level = level::info;
	std::vector<database> databases;
	std::chrono::sys_seconds deployed{};
};

struct pair_of_ints {
	std::array<int, 2> items{};
	std::deque<std::int64_t> queue;
	std::list<bool> flags;
	std::unordered_map<std::string, double> weights;
	std::flat_map<std::string, std::string> names;
	soml::value anything;
};

struct nonassignable {
	nonassignable() = default;
	nonassignable(nonassignable&&) = default;
	nonassignable& operator=(nonassignable&&) = delete;
};

struct nonmovable {
	nonmovable() = default;
	nonmovable(nonmovable&&) = delete;
	nonmovable& operator=(nonmovable&&) = default;
};

struct void_assignable {
	void_assignable() = default;
	void_assignable(void_assignable&&) = default;
	void operator=(void_assignable&&) {
	}
};

struct first_character_less {
	bool operator()(const std::string& left, const std::string& right) const {
		return std::string_view{left}.substr(0, 1) < std::string_view{right}.substr(0, 1);
	}
};

} // namespace

template<>
inline constexpr auto soml::enum_names<level> = std::array{
	std::pair{level::debug, "debug"sv},
	std::pair{level::info, "info"sv},
	std::pair{level::warning, "warning"sv},
};

template<>
struct soml::converter<nonassignable> {
	static constexpr std::expected<nonassignable, soml::error> from_value(const soml::value&, const soml::decode_options&) {
		return nonassignable{};
	}
};

template<>
struct soml::converter<nonmovable> {
	static constexpr std::expected<nonmovable, soml::error> from_value(const soml::value&, const soml::decode_options&) {
		return std::expected<nonmovable, soml::error>{std::in_place};
	}
};

template<>
struct soml::converter<void_assignable> {
	static constexpr std::expected<void_assignable, soml::error> from_value(const soml::value&, const soml::decode_options&) {
		return void_assignable{};
	}
};

static_assert(soml::decodable<nonassignable>);
static_assert(soml::decodable<nonmovable>);
static_assert(!soml::decodable<std::optional<nonmovable>>);
static_assert(!soml::decodable<std::vector<nonmovable>>);
static_assert(!soml::decodable<std::map<std::string, nonmovable>>);
static_assert(!soml::decodable<std::map<std::string, nonassignable>>);
static_assert(soml::decodable<std::vector<void_assignable>>);
static_assert(soml::decodable<std::map<std::string, void_assignable>>);
static_assert(soml::decodable<void_assignable>);
static_assert(!soml::decodable<std::array<nonassignable, 1>>);
static_assert(!soml::decodable<std::array<nonmovable, 1>>);
static_assert(soml::decodable<std::array<void_assignable, 1>>);

// Text decoding enriches an error before returning its result, which must be movable or copyable.
static_assert([]<class T>() {
	return !requires { soml::decode<T>("{}"); };
}.operator()<nonmovable>());
static_assert(soml::from_value<nonmovable>(soml::value{}).has_value());

template<>
inline constexpr auto soml::fields<database> = std::tuple{
	soml::field{"host", &database::host},
	soml::field{"port", &database::port},
};

template<>
inline constexpr auto soml::fields<service> = std::tuple{
	soml::field{"name", &service::name},
	soml::field{"replicas", &service::replicas},
	soml::field{"ratio", &service::ratio},
	soml::field{"timeout", &service::timeout},
	soml::field{"owner", &service::owner},
	soml::field{"labels", &service::labels},
	soml::field{"limits", &service::limits},
	soml::field{"log-level", &service::log_level},
	soml::field{"databases", &service::databases},
	soml::field{"deployed", &service::deployed},
};

template<>
inline constexpr auto soml::fields<pair_of_ints> = std::tuple{
	soml::field{"items", &pair_of_ints::items},
	soml::field{"queue", &pair_of_ints::queue},
	soml::field{"flags", &pair_of_ints::flags},
	soml::field{"weights", &pair_of_ints::weights},
	soml::field{"names", &pair_of_ints::names},
	soml::field{"anything", &pair_of_ints::anything},
};

namespace {

constexpr bool nested_sequence_encoding() {
	const std::array<std::vector<std::optional<int>>, 3> rows{{{1, std::nullopt}, {}, {2, 3}}};
	const soml::value expected = soml::array{soml::array{1, nullptr}, soml::array{}, soml::array{2, 3}};
	return soml::to_value(rows) == expected && soml::from_value<std::remove_cv_t<decltype(rows)>>(expected) == rows && soml::to_value(std::array<int, 0>{}) == soml::value{soml::array{}};
}

static_assert(nested_sequence_encoding());

void test_container_conversion() {
	check(nested_sequence_encoding());

	// Fixed arrays, dynamic sequences, and maps must compose error paths without losing the failing index or quoted key.
	using row = std::map<std::string, std::array<level, 2>>;
	const row valid{{"first", {level::debug, level::info}}};
	const row invalid{{"odd key", {level::warning, static_cast<level>(99)}}};
	const auto check_failure = [](const auto& source) {
		const auto result = soml::to_value(source);
		check(!result);
		if (!result) {
			check_equal(result.error().code, soml::error_code::out_of_range);
			check_equal(result.error().path, "[1][\"odd key\"][1]"s);
		}
	};
	check_failure(std::array{valid, invalid, valid});
	check_failure(std::vector{valid, invalid, valid});
	check_failure(std::list{valid, invalid, valid});
	check_failure(std::deque{valid, invalid, valid});

	// Hash-map conversion must preserve every owning key and value, independently of bucket order.
	std::unordered_map<std::string, std::vector<std::optional<int>>> source;
	for (int index = 0; index < 128; index++) {
		source.emplace("long owning key " + std::to_string(index), std::vector<std::optional<int>>{index, std::nullopt, -index});
	}
	source.emplace(std::string{"nul\0key", 7}, std::vector<std::optional<int>>{});
	const auto encoded = soml::to_value(source);
	check(encoded.has_value());
	if (encoded) {
		check(soml::from_value<decltype(source)>(*encoded) == source);
		const auto text = soml::write_canonical(*encoded);
		check(text.has_value());
		if (text) {
			check(soml::decode<decltype(source)>(*text) == source);
		}
	}
}

void test_typed_mapping() {
	check(soml::converter<std::array<void_assignable, 1>>::from_value(soml::value{soml::array{soml::value{1}}}, {}).has_value());
	check(soml::from_value<std::vector<void_assignable>>(soml::array{1, 2})->size() == 2);
	check(soml::from_value<std::map<std::string, void_assignable>>(soml::object{{"first", 1}, {"second", 2}})->size() == 2);
	const auto colliding_keys = soml::from_value<std::map<std::string, int, first_character_less>>(soml::value{soml::object{{"a", 1}, {"ab", 2}}});
	check(!colliding_keys.has_value());

	if (!colliding_keys) {
		check_equal(colliding_keys.error().code, soml::error_code::out_of_range);
		check_equal(colliding_keys.error().path, "ab"s);
	}

	const auto decoded = soml::decode<service>(R"(
name: 'api'
replicas: 3
timeout: 1m30s
labels: ['prod', 'eu-west']
limits: {cpu: 2, memory: 512}
log-level: 'warning'
databases: [{host: 'db1'}, {host: 'db2', port: 6432}]
deployed: 2026-09-19T14:00:00Z
)");

	check(decoded.has_value());

	if (decoded) {
		const service& result = *decoded;
		check_equal(result.name, "api"s);
		check_equal(result.replicas, 3);
		check_equal(result.ratio, 1.0);
		check(result.timeout == std::chrono::seconds{90});
		check(!result.owner.has_value());
		check(result.labels == std::vector<std::string>{"prod", "eu-west"});
		check(result.limits == std::map<std::string, int>{{"cpu", 2}, {"memory", 512}});
		check(result.log_level == level::warning);
		check_equal(result.databases.size(), 2UZ);
		check_equal(result.databases[0].port, std::uint16_t{5432});
		check_equal(result.databases[1].port, std::uint16_t{6432});
		check(result.deployed == std::chrono::sys_days{std::chrono::year{2026} / 9 / 19} + std::chrono::hours{14});

		// Encoding keeps the field order, and canonical form sorts.
		const std::string encoded = *soml::encode(result);
		check(encoded.starts_with("name: 'api'\nreplicas: 3\nratio: 1.0\ntimeout: 1m30s\nowner: null\nlabels: [\n"));
		check(soml::decode<service>(encoded).transform([&](const service& again) {
			return again.name == result.name && again.databases.size() == 2 && again.log_level == level::warning;
		}).value_or(false));
		check(soml::encode_canonical(result)->starts_with("databases: ["));
	}

	// Errors name the path.
	const auto port_error = soml::decode<service>("databases: [{host: 'a'}, {port: 70000}]");
	check_equal(port_error.error().code, soml::error_code::out_of_range);
	check_equal(port_error.error().path, "databases[1].port"s);
	check_equal(port_error.error().description(), "databases[1].port: The int 70000 is outside the range of the type, 0 to 65535 at line 1, column 33"s);

	const auto float_error = soml::decode<service>("ratio: 3");
	check_equal(float_error.error().code, soml::error_code::type_mismatch);
	check_equal(float_error.error().description(), "ratio: Expected a float, written with a fraction or an exponent, as in 3.0, but found the int 3 at line 1, column 8"s);

	const auto unknown = soml::decode<service>("nmae: 'typo'");
	check_equal(unknown.error().code, soml::error_code::unknown_key);
	check_equal(unknown.error().description(), "nmae: Unknown key nmae, which the type has no field for. Did you mean name? at line 1, column 1"s);
	check(soml::decode<service>("nmae: 'typo'", {.ignore_unknown_keys = true}).has_value());

	const auto enum_error = soml::decode<service>("log-level: 'loud'");
	check_equal(enum_error.error().message, "Unknown value \"loud\". Expected one of: debug, info, warning"s);

	check_equal(soml::decode<service>("timeout: 1.5s").error().message, "The duration is not a whole number of the type's unit"s);
	check_equal(soml::decode<service>("name: 'a'\nname: 'b'").error().code, soml::error_code::syntax);
	check_equal(soml::decode<service>("[]").error().message, "Expected an object, but found an array"s);
	check_equal(soml::decode<service>("replicas: '3'").error().message, "Expected an int, but found the string '3'"s);
	check_equal(soml::decode<service>("timeout: 30").error().message, "Expected a duration, but found the int 30"s);
	check_equal(soml::decode<service>("zzz: 1").error().message, "Unknown key zzz, which the type has no field for"s);
	check_equal(soml::decode<service>("limits: {'a b': 'x'}").error().path, "limits[\"a b\"]"s);

	const auto containers = soml::decode<pair_of_ints>("items: [1, 2]\nqueue: [3]\nflags: [true]\nweights: {a: 0.5}\nnames: {b: 'c'}\nanything: {x: [1]}");
	check(containers.has_value());

	if (containers) {
		check(containers->items == std::array{1, 2});
		check(containers->queue.front() == 3);
		check(containers->flags.front());
		check(containers->weights.at("a") == 0.5);
		check(containers->names.at("b") == "c");
		check(containers->anything["x"][0].as<int>() == 1);
	}

	check_equal(soml::decode<pair_of_ints>("items: [1]").error().message, "Expected an array of 2 items, but found 1"s);

	// Values that SOML cannot hold.
	check_equal(soml::to_value(std::numeric_limits<std::uint64_t>::max()).error().code, soml::error_code::out_of_range);
	check(soml::to_value(std::uint64_t{42}).has_value());
	check_equal(soml::to_value(std::numeric_limits<double>::quiet_NaN()).error().code, soml::error_code::invalid_value);
	check_equal(soml::to_value(std::chrono::hours::max()).error().code, soml::error_code::out_of_range);
	check(soml::to_value(std::chrono::duration<double>{1.5}).value() == soml::value{soml::duration{1500ms}});
	check_equal(soml::to_value(std::vector<double>{1.0, std::numeric_limits<double>::quiet_NaN()}).error().path, "[1]"s);
	check(soml::from_value<float>(soml::value{0.1}).value() == 0.1F);
	check_equal(soml::from_value<float>(soml::value{1e300}).error().code, soml::error_code::out_of_range);
	check(soml::from_value<float>(soml::value{3.4028235e38}).value() == std::numeric_limits<float>::max());
	check(soml::from_value<float>(soml::value{-3.4028235e38}).value() == std::numeric_limits<float>::lowest());
	check(soml::from_value<float>(soml::value{3.40282356e38}).value() == std::numeric_limits<float>::max());
	check(!soml::from_value<float>(soml::value{3.40282357e38}).has_value());
	check(soml::from_value<float>(soml::value{-std::numeric_limits<double>::infinity()}).value() == -std::numeric_limits<float>::infinity());
	check(soml::from_value<std::chrono::duration<double>>(soml::value{soml::duration{1500ms}}).value().count() == 1.5);

	// An unsigned duration can hold counts above the signed 64-bit range.
	using unsigned_picoseconds = std::chrono::duration<std::uint64_t, std::pico>;
	const auto picoseconds = soml::from_value<unsigned_picoseconds>(soml::value{soml::duration{10'000'000s}});
	check(picoseconds.has_value());

	if (picoseconds) {
		check_equal(picoseconds->count(), 10'000'000'000'000'000'000ULL);
		check(soml::to_value(*picoseconds).value() == soml::value{soml::duration{10'000'000s}});
	}

	check(!soml::from_value<unsigned_picoseconds>(soml::value{-1ns}).has_value());
	check(!soml::from_value<unsigned_picoseconds>(soml::value{soml::duration{20'000'000s}}).has_value());

	// Floating-point durations use long double arithmetic when scaling to nanoseconds.
	check(soml::to_value(std::chrono::duration<float, std::ratio<3600>>{2.5F}).value() == soml::value{soml::duration{150min}});
	check(soml::to_value(std::chrono::duration<float>{9.2233722e9F}).value() == soml::value{soml::duration{9'223'371'776'000'000'000}});
	check(soml::to_value(std::chrono::duration<float>{9.3e9F}).error().code == soml::error_code::out_of_range);
	check(soml::to_value(std::chrono::duration<double, std::nano>{4503599627370497.0}).value() == soml::value{soml::duration{4'503'599'627'370'497}});
	check(soml::to_value(std::chrono::duration<double, std::nano>{-4503599627370499.0}).value() == soml::value{soml::duration{-4'503'599'627'370'499}});
	check(soml::to_value(std::chrono::duration<double, std::nano>{2.5}).value() == soml::value{soml::duration{2}});
	check(soml::to_value(std::chrono::duration<double, std::nano>{-2.5}).value() == soml::value{soml::duration{-2}});

	// A huge unknown key is reported quickly, and its path is cut short.
	const std::string huge_key(2'000'000, 'k');
	const auto start = std::clock();
	const auto huge = soml::decode<service>(std::format("{}: 1", huge_key));
	check(huge.error().code == soml::error_code::unknown_key && huge.error().path.size() < 300);
	check(std::clock() - start < CLOCKS_PER_SEC);
	check(soml::from_value<std::chrono::nanoseconds>(soml::value{soml::duration::min()}).has_value());
}

void test_decode_locations() {
	const std::string_view text = "# config\ndatabases: [\n\t{port: 80}\n\t{port: 70000}\n]\n";
	const auto nested = soml::decode<service>(text);
	check(!nested);
	if (!nested) {
		check_equal(nested.error().path, "databases[1].port"s);
		check_equal(nested.error().offset, text.find("70000"));
		check_equal(nested.error().line, 4UZ);
		check_equal(nested.error().column, 9UZ);
		check_equal(nested.error().code_frame(text), "  2 | databases: [\n  3 | \t{port: 80}\n> 4 | \t{port: 70000}\n    | \t       ^"s);
	}

	// Paths use decoded keys, independently of their spelling and of punctuation in the key.
	for (const std::string_view input : {"'a.b': [1, 'bad']"sv, "'x[0]': [1, 'bad']"sv, "'': [1, 'bad']"sv, "\"a\\u{0}b\": [1, 'bad']"sv, "'a\"b': [1, 'bad']"sv, "\"a\\\\b\": [1, 'bad']"sv, "'é': [1, 'bad']"sv, "a: [1]\nab: [1, 'bad']"sv}) {
		const auto result = soml::decode<std::map<std::string, std::vector<int>>>(input);
		check(!result);
		if (!result) {
			check_equal(result.error().offset, input.find("'bad'"));
			check(result.error().line > 0);
		}
	}
	const auto unicode = soml::decode<std::map<std::string, std::vector<int>>>("'é': [1, 'bad']");
	check_equal(unicode.error().column, 10UZ);

	// Unknown keys point to the key; type errors point to the value or container.
	const auto unknown = soml::decode<service>("# comment\n'nmae': 'api'");
	check_equal(unknown.error().offset, 10UZ);
	check_equal(unknown.error().line, 2UZ);
	check_equal(unknown.error().column, 1UZ);
	const auto root = soml::decode<int>("# comment\n[1]");
	check_equal(root.error().code, soml::error_code::type_mismatch);
	check_equal(root.error().offset, 10UZ);
	check_equal(root.error().line, 2UZ);
	check_equal(root.error().column, 1UZ);
	const auto container = soml::decode<pair_of_ints>("items: [1]");
	check_equal(container.error().offset, 7UZ);
	check_equal(container.error().column, 8UZ);

	// Locating an error must skip comments and block strings without mistaking their contents for tokens.
	const std::string_view blocks = "name: '''\n\tport: 70000 # content\n'''\n/* port: 70000 */\ndatabases: [{port: 70000}]";
	const auto after_block = soml::decode<service>(blocks);
	check_equal(after_block.error().offset, blocks.rfind("70000"));
	check_equal(after_block.error().line, 5UZ);
	check_equal(after_block.error().column, 20UZ);

	const std::string_view invalid = "databases: [";
	check_equal(soml::decode<service>(invalid).error(), soml::parse(invalid).error());
	const auto constructed = soml::from_value<service>(soml::object{{"replicas", "bad"}});
	check_equal(constructed.error().path, "replicas"s);
	check_equal(constructed.error().line, 0UZ);
	check(constructed.error().code_frame("replicas: 'bad'").empty());

	// Abbreviated keys may collide. Retain the path rather than pointing at the wrong value.
	const std::string prefix(201, 'k');
	const std::string ambiguous = prefix + "a: [1]\n" + prefix + "b: ['bad']";
	const auto abbreviated = soml::decode<std::map<std::string, std::vector<int>>>(ambiguous);
	check(!abbreviated && !abbreviated.error().path.empty() && abbreviated.error().line == 0);
}

#if defined(__cpp_impl_reflection)

struct [[=soml::kebab_case]] reflected_server {
	std::string host = "localhost";
	int max_connections = 100;
	[[=soml::key("log")]] level log_level = level::info;
	std::optional<std::vector<int>> ports;
};

enum class color : std::uint8_t {
	red,
	green,
};

struct reflected_palette {
	color primary = color::red;
	std::vector<reflected_server> servers;
};

struct reflected_keys {
	[[=soml::key("a\0b"sv)]] int embedded = 1;
	[[=soml::key(""sv)]] int empty = 2;
};

static_assert(soml::decode<reflected_server>("max-connections: 5")->max_connections == 5);
static_assert(soml::decode<reflected_keys>("\"a\\u{0}b\": 3")->embedded == 3);
static_assert(soml::encode(reflected_keys{}) == "\"a\\u{0}b\": 1\n'': 2\n");

// A struct with a base class is not read by reflection, because its members leave out the base's, and a sequence that derives from a container is a sequence.
struct reflected_base {
	int a = 1;
};

struct reflected_derived : reflected_base {
	int b = 2;
};

struct reflected_list : std::vector<int> {};

static_assert(!soml::decodable<reflected_derived> && !soml::encodable<reflected_derived>);
static_assert(soml::decodable<reflected_list>);

// An aggregate that is also a sequence is a sequence.
struct reflected_sequence {
	using value_type = int;
	std::array<int, 4> items{};
	std::size_t count = 0;

	void push_back(int item) {
		items.at(count++) = item;
	}

	[[nodiscard]] const int* begin() const {
		return items.data();
	}

	[[nodiscard]] const int* end() const {
		return items.data() + count;
	}
};

static_assert(soml::decodable<reflected_sequence>);

void test_reflection() {
	// Key annotations preserve their string-view length, including embedded NULs.
	const auto keyed = soml::decode<reflected_keys>("\"a\\u{0}b\": 3\n'': 4");
	check(keyed.has_value() && keyed->embedded == 3 && keyed->empty == 4);
	check_equal(soml::encode(reflected_keys{}), std::expected<std::string, soml::error>{"\"a\\u{0}b\": 1\n'': 2\n"});
	check(!soml::decode<reflected_keys>("a: 3"));

	const auto server = soml::decode<reflected_server>("host: 'a'\nmax-connections: 5\nlog: 'debug'\nports: [80]");
	check(server.has_value() && server->max_connections == 5 && server->log_level == level::debug && server->ports->front() == 80);
	check_equal(*soml::encode(reflected_server{}), "host: 'localhost'\nmax-connections: 100\nlog: 'info'\nports: null\n"s);
	check_equal(soml::decode<reflected_server>("max_connections: 5").error().code, soml::error_code::unknown_key);

	const auto palette = soml::decode<reflected_palette>("primary: 'green'\nservers: [{host: 'b'}]");
	check(palette.has_value() && palette->primary == color::green && palette->servers.front().host == "b");
	check_equal(soml::decode<reflected_palette>("primary: 'blue'").error().message, "Unknown value \"blue\". Expected one of: red, green"s);
	check_equal(*soml::encode(reflected_palette{}), "primary: 'red'\nservers: []\n"s);
}

#endif

// MARK: Complexity

/*
Reading time must grow linearly, or as n log n, with every input shape, so a document 8 times larger takes about 8 times as long, not 64.
*/
void test_complexity() {
	const std::vector<std::pair<std::string_view, std::function<std::string(std::size_t)>>> shapes{
		{"wide object", [](std::size_t size) {
			std::string text;

			for (std::size_t index = 0; index < size; index++) {
				text += std::format("k{}: 1\n", index);
			}

			return text;
		}},
		{"keys with a long shared prefix", [](std::size_t size) {
			std::string text;
			const std::string prefix(200, 'p');

			for (std::size_t index = 0; index < size / 20; index++) {
				text += std::format("{}{}: 1\n", prefix, index);
			}

			return text;
		}},
		{"many small objects", [](std::size_t size) {
			std::string text = "a: [";

			for (std::size_t index = 0; index < size / 4; index++) {
				text += "{a: 1, b: 2, c: 3, d: 4}, ";
			}

			return text + "]";
		}},
		{"long string", [](std::size_t size) {
			return std::format("a: \"{}\"", std::string(size * 10, 'x'));
		}},
		{"long escaped string", [](std::size_t size) {
			std::string text = "a: \"";

			for (std::size_t index = 0; index < size; index++) {
				text += "\\n\\u{1f600}";
			}

			return text + "\"";
		}},
		{"long block string", [](std::size_t size) {
			std::string text = "a:\n\t'''\n";

			for (std::size_t index = 0; index < size; index++) {
				text += "\tline\n\n";
			}

			return text + "\t'''";
		}},
		{"long float", [](std::size_t size) {
			return std::format("a: 0.{}1", std::string(size * 10, '0'));
		}},
		{"long duration fraction", [](std::size_t size) {
			return std::format("a: 1.{}s", std::string(size * 10, '0'));
		}},
		{"many comments", [](std::size_t size) {
			std::string text;

			for (std::size_t index = 0; index < size; index++) {
				text += "/* a */ # b\n";
			}

			return text + "a: 1";
		}},
		{"unterminated comment", [](std::size_t size) {
			return std::format("a: 1 /*{}", std::string(size * 10, '/'));
		}},
	};

	for (const auto& [name, generate] : shapes) {
		const auto measure = [&](std::size_t size) {
			// Processor time rather than wall time, so that time while the process waits for a busy machine does not count.
			const std::string text = generate(size);
			std::clock_t best = std::numeric_limits<std::clock_t>::max();

			for (int run = 0; run < 5; run++) {
				const std::clock_t start = std::clock();
				static_cast<void>(soml::parse(text));
				best = std::min(best, std::clock() - start);
			}

			return static_cast<double>(best);
		};

		// A quadratic shape takes about 64 times as long. Each shape gets three tries, because a busy machine can stall one measurement.
		double ratio = 0.0;

		for (int attempt = 0; attempt < 3 && (attempt == 0 || ratio > 24.0); attempt++) {
			ratio = measure(160'000) / std::max(measure(20'000), 1.0);
		}

		if (ratio > 24.0) {
			std::println(stderr, "{}: 8 times the input took {:.1f} times as long", name, ratio);
			failure_count++;
		}
	}
}

} // namespace

int main() {
	const std::vector<std::pair<std::string_view, void (*)()>> tests{
		{"values", test_values},
		{"input kinds", test_input_kinds},
		{"errors", test_errors},
		{"depth", test_depth},
		{"duplicates", test_duplicates},
		{"writing", test_writing},
		{"writer checks", test_writer_checks},
		{"round trip", test_round_trip},
		{"object", test_object},
		{"assignment", test_assignment},
		{"instant", test_instant},
		{"float parsing", test_float_parsing},
		{"rounding modes", test_rounding_modes},
		{"float formatting", test_float_formatting},
		{"typed mapping", test_typed_mapping},
		{"decode locations", test_decode_locations},
		{"container conversion", test_container_conversion},
#if defined(__cpp_impl_reflection)
		{"reflection", test_reflection},
#endif
		{"complexity", test_complexity},
	};

	for (const auto& [name, run] : tests) {
		const int before = failure_count;
		run();
		std::println("{} {}", failure_count == before ? "✔" : "✘", name);
	}

	if (failure_count > 0) {
		std::println(stderr, "{} checks failed", failure_count);
		return 1;
	}

	std::println("All tests passed");
	return 0;
}
