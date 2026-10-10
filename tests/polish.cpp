#include <soml.hpp>

#include <map>
#include <print>
#include <source_location>

namespace {

enum class label {
	empty,
	full,
};

} // namespace

template<>
inline constexpr auto soml::enum_names<label> = std::array{
	std::pair{label::empty, std::string_view{}},
	std::pair{label::full, std::string_view{"full"}},
};

namespace {

int failures = 0;

void check(bool condition, const std::source_location location = std::source_location::current()) {
	if (!condition) {
		std::println(stderr, "{}:{}: check failed", location.file_name(), location.line());
		failures++;
	}
}

constexpr bool compile_time_apis() {
	const std::vector<char> input{'a', ':', ' ', '1'};
	const auto parsed = soml::parse(input);
	std::string output = "# prefix\n";

	if (!parsed || !soml::write(*parsed, output) || output != "# prefix\na: 1\n") {
		return false;
	}

	std::vector<soml::member> members{{"b", 1}, {"a", 2}, {"b", 3}};
	const soml::object object{std::move(members)};
	return object.size() == 2 && object.begin()->key == "b" && object.find("b")->as<int>() == 3 && soml::from_value<std::nullptr_t>(soml::value{}).has_value() && soml::to_value(nullptr)->is_null();
}

static_assert(compile_time_apis());
static_assert(soml::to_value(label::empty) == soml::value{""});
static_assert(soml::encode(std::array{label::empty}) == "[\n\t''\n]\n");

constexpr bool boolean_sequence_conversion() {
	// vector<bool> iterators yield proxies or bool values rather than bool references.
	const std::vector<bool> flags{true, false, true};
	const soml::value expected = soml::array{true, false, true};
	const auto encoded = soml::to_value(flags);
	const auto decoded = soml::from_value<std::vector<bool>>(expected);
	return encoded == expected && decoded == flags && soml::to_value(std::vector<bool>{}) == soml::value{soml::array{}};
}

static_assert(boolean_sequence_conversion());

constexpr bool diagnostic_truncation() {
	for (const std::string_view character : {std::string_view{"a"}, std::string_view{"é"}, std::string_view{"€"}, std::string_view{"😀"}}) {
		std::string text;

		for (int index = 0; index < 41; index++) {
			text += character;
		}

		if (soml::detail::abbreviate(text) != text.substr(0, character.size() * 40) + "…" || soml::detail::abbreviate(text, 41) != text || soml::detail::abbreviate(text, 0) != "…") {
			return false;
		}
	}

	return soml::detail::abbreviate("", 0).empty() && soml::detail::abbreviate(std::string(41, '\x80')) == std::string(40, '\x80') + "…";
}

static_assert(diagnostic_truncation());

constexpr bool displayed_string_diagnostics() {
	const auto has_description = [](std::string_view source, std::string_view expected) {
		const auto result = soml::from_value<int>(soml::value{source});
		return !result && result.error().code == soml::error_code::type_mismatch && result.error().message == "Expected an int, but found the string " + std::string{expected};
	};
	if (!has_description("a\r", "\"a\\r\"") || !has_description(std::string{"a\xff", 2}, "\"a\\u00ff\"") || !has_description("it's", "\"it's\"") || !has_description("a\n", "\"a\\n\"")) {
		return false;
	}
	const std::string prefix(40, 'x');
	for (const std::string& suffix : {std::string{"\r"}, std::string(1, '\xff'), std::string{"'"}, std::string{"\n"}, std::string{"😀"}}) {
		if (!has_description(prefix + suffix, "'" + prefix + "…'")) {
			return false;
		}
	}
	for (const bool invalid_utf8 : {false, true}) {
		const std::string source = "a" + std::string(1, invalid_utf8 ? '\xff' : '\r') + std::string(100, 'x');
		const std::string expected = "\"a" + std::string{invalid_utf8 ? "\\u00ff" : "\\r"} + std::string(38, 'x') + "…\"";
		if (!has_description(source, expected)) {
			return false;
		}
	}
	return true;
}

static_assert(displayed_string_diagnostics());

static_assert(soml::error{.message = "héllo\u202Eעברית", .line = 3, .column = 7, .path = "[\"a\u061Cb\"]"}.description() == "[\"a�b\"]: héllo�עברית at line 3, column 7");

constexpr bool plain_string_runs(std::size_t first = 0, std::size_t end = 24) {
	// UTF-8 bytes whose low bits resemble a quote, a line feed, or a backslash must remain plain characters.
	const std::string_view unicode = "\u008a§¢\u0700🙂";
	for (const char quote : {'\'', '"'}) {
		for (std::size_t length = first; length < end; length++) {
			const std::string content = std::string(length, 'a') + std::string{unicode} + std::string(24, 'b');
			const std::string source = "a: " + std::string(1, quote) + content + quote;
			const auto parsed = soml::parse(source);
			if (!parsed || (*parsed)["a"].as<std::string_view>() != content) {
				return false;
			}
			const auto newline = soml::parse("a: " + std::string(1, quote) + content + "\n");
			const auto unterminated = soml::parse("a: " + std::string(1, quote) + content);
			if (newline || unterminated || newline.error().offset != 3 || unterminated.error().offset != 3 || !newline.error().message.starts_with("Unterminated string") || newline.error().message != unterminated.error().message) {
				return false;
			}
			const std::string escaped = "a: \"" + content + "\\n" + content + "\\\"\\\\\"";
			const auto decoded = soml::parse(escaped);
			if (!decoded || (*decoded)["a"].as<std::string_view>() != content + "\n" + content + "\"\\") {
				return false;
			}
			const auto unknown_escape = soml::parse("a: \"" + content + "\\q\"");
			if (unknown_escape || unknown_escape.error().offset != 4 + content.size() || !unknown_escape.error().message.starts_with("Unknown escape")) {
				return false;
			}
		}
	}
	return soml::parse("a: 'literal\\nbackslash'")->find("a")->as<std::string_view>() == "literal\\nbackslash";
}

// Separate batches also fit the hardened standard library's constexpr evaluation budget.
static_assert(plain_string_runs(0, 2));
static_assert(plain_string_runs(2, 4));
static_assert(plain_string_runs(4, 6));
static_assert(plain_string_runs(6, 8));

constexpr bool ascii_source_runs(std::size_t alignment) {
	const std::string blanks = std::string(alignment, ' ') + " \t \t \t \t \t \t \t \t";
	const std::string key = std::string(alignment, 'a') + "AZaz019_-AZaz019_-AZaz019_-";
	const auto parsed = soml::parse(blanks + key + ":" + blanks + "\n" + blanks + "123\n" + blanks);
	if (!parsed || parsed->find(key) == nullptr || parsed->find(key)->as<int>() != 123) {
		return false;
	}
	const std::string prefix = std::string(alignment, 'a');
	for (const std::string_view character : {"¢", "é", "😀"}) {
		const auto invalid_key = soml::parse(prefix + std::string{character} + std::string(24, 'a') + ": 1");
		if (invalid_key || invalid_key.error().offset != alignment) {
			return false;
		}
	}
	const auto nonblank = soml::parse(blanks + "¢" + std::string(24, ' ') + "a: 1");
	return !nonblank && nonblank.error().offset == blanks.size() && soml::parse(blanks + "a: 1\n" + blanks + ",").error().message == "Top-level entries are separated by line breaks, not commas. Use braces for a one-line object";
}

static_assert(ascii_source_runs(0));
static_assert(ascii_source_runs(7));

constexpr bool block_indentation_examples() {
	for (const char quote : {'\'', '"'}) {
		const std::string delimiter(3, quote);
		const auto parsed = soml::parse("a: " + delimiter + "\n \t\n\t x\n\t \t\n\t y\n  \t\n\t " + delimiter);
		if (!parsed || parsed->find("a")->as<std::string_view>() != "x\n\ny") {
			return false;
		}
	}
	const std::string bad_indentation = "a: \"\"\"\n\t\\q\nx\n\t\"\"\"";
	const auto invalid = soml::parse(bad_indentation);
	return !invalid && invalid.error().offset == bad_indentation.find('x') && invalid.error().message.starts_with("This line does not start with the indentation");
}

static_assert(block_indentation_examples());

constexpr bool scalar_count_examples(std::size_t limit) {
	for (const auto& [unit, scalars] : std::array{
		std::pair{std::string_view{"a"}, 1UZ},
		std::pair{std::string_view{"é"}, 1UZ},
		std::pair{std::string_view{"€"}, 1UZ},
		std::pair{std::string_view{"😀"}, 1UZ},
		std::pair{std::string_view{"aé€😀"}, 4UZ},
	}) {
		for (std::size_t alignment = 0; alignment < 8; alignment++) {
			std::string text(alignment, 'a');
			for (std::size_t length = 0; length < limit; length++) {
				const std::size_t expected = alignment + length * scalars;
				if (soml::detail::count_scalars(text) != expected || soml::detail::locate("first\n" + text, text.size() + 6).column != expected + 1) {
					return false;
				}
				text += unit;
			}
		}
	}
	return true;
}

static_assert(scalar_count_examples(3));

constexpr bool location_examples() {
	const std::string_view text = "é\n€😀\nx";
	for (const auto& [offset, line, column] : std::array{
		std::array{0UZ, 1UZ, 1UZ},
		std::array{2UZ, 1UZ, 2UZ},
		std::array{3UZ, 2UZ, 1UZ},
		std::array{6UZ, 2UZ, 2UZ},
		std::array{10UZ, 2UZ, 3UZ},
		std::array{11UZ, 3UZ, 1UZ},
		std::array{12UZ, 3UZ, 2UZ},
		std::array{100UZ, 3UZ, 2UZ},
	}) {
		const auto found = soml::detail::locate(text, offset);
		if (found.line != line || found.column != column) {
			return false;
		}
	}
	const auto empty = soml::detail::locate("", 100);
	return empty.line == 1 && empty.column == 1;
}

static_assert(location_examples());

bool reindented_block_examples() {
	for (const char quote : {'\'', '"'}) {
		const std::string delimiter(3, quote);
		const std::string indentation = std::string(100000, ' ') + "\t";
		const std::string source = "a: " + delimiter + "\n" + indentation + "x\n" + indentation + " \t\n" + indentation + "y\n" + indentation + delimiter;
		const auto formatted = soml::format(source);
		const std::string expected = "a:\n\t" + delimiter + "\n\tx\n" + indentation + " \t\n\ty\n\t" + delimiter + "\n";
		if (!formatted || *formatted != expected || soml::parse(*formatted) != soml::parse(source)) {
			return false;
		}
	}
	return true;
}

} // namespace

int main() {
	check(location_examples());
	check(reindented_block_examples());
	check(displayed_string_diagnostics());
	check(scalar_count_examples(33));
	check(plain_string_runs());
	check(block_indentation_examples());
	for (std::size_t alignment = 0; alignment < 24; alignment++) {
		check(ascii_source_runs(alignment));
		for (int byte = 0; byte < 128; byte++) {
			std::string key(alignment + 1, 'a');
			key += static_cast<char>(byte);
			key += std::string(24, 'a');
			const auto parsed = soml::parse(key + ": 1");
			check(parsed.has_value() == soml::detail::is_bare_key_character(byte));
			if (parsed) {
				check(parsed->find(key) != nullptr && parsed->find(key)->as<int>() == 1);
			}
		}
	}
	check(diagnostic_truncation());
	check(boolean_sequence_conversion());
	check(soml::decode<std::vector<bool>>("[true, false, true]") == std::vector<bool>{true, false, true});
	const auto invalid_boolean = soml::decode<std::vector<bool>>("[true, 1]");
	check(!invalid_boolean && invalid_boolean.error().code == soml::error_code::type_mismatch && invalid_boolean.error().path == "[1]");

	// Empty enum names are strings too, and must round-trip like nonempty names.
	check(soml::from_value<label>(soml::value{""}) == label::empty);
	check(soml::to_value(label::empty) == soml::value{""});
	check(soml::encode(std::array{label::empty, label::full}) == "[\n\t''\n\t'full'\n]\n");
	check(soml::decode<std::array<label, 2>>("['', 'full']") == std::array{label::empty, label::full});
	const auto unnamed = soml::to_value(static_cast<label>(99));
	check(!unnamed && unnamed.error().code == soml::error_code::out_of_range);

	const soml::value document = soml::object{{"z", 1}, {"a", soml::array{true, nullptr}}};
	std::string output = "prefix\n";
	check(soml::write(document, output).has_value());
	check(output == "prefix\n" + soml::write(document).value());
	check(soml::write_canonical(document, output).has_value());
	check(output == "prefix\n" + soml::write(document).value() + soml::write_canonical(document).value());

	const soml::value invalid = soml::object{{"a", 1}, {"b", std::numeric_limits<double>::quiet_NaN()}};
	const std::string original = output;
	check(!soml::write(invalid, output));
	check(output == original);
	check(!soml::write_canonical(invalid, output));
	check(output == original);
	check(!soml::write(soml::value{1}, output));
	check(output == original);

	// The destination may itself be a string inside the document.
	soml::value aliased = soml::object{{"a", "old"}};
	check(soml::write(aliased, *aliased.find("a")->get_if<std::string>()).has_value());
	check(*aliased.find("a")->get_if<std::string>() == "olda: 'old'\n");
	for (const bool canonical : {false, true}) {
		soml::value empty_aliased = soml::object{{"a", ""}};
		std::string& destination = *empty_aliased.find("a")->get_if<std::string>();
		check((canonical ? soml::write_canonical(empty_aliased, destination) : soml::write(empty_aliased, destination)).has_value());
		check(destination == "a: ''\n");
	}

	// A source-derived directional control must not reorder the displayed error or its location.
	for (const char32_t scalar : {U'\u061C', U'\u200E', U'\u200F', U'\u202A', U'\u202B', U'\u202C', U'\u202D', U'\u202E', U'\u2066', U'\u2067', U'\u2068', U'\u2069'}) {
		std::string control;
		soml::detail::append_utf8(control, scalar);
		const soml::error failure{.code = soml::error_code::type_mismatch, .message = "Expected " + control + "an int", .line = 3, .column = 7, .path = "[\"a" + control + "b\"]"};
		check(failure.description() == "[\"a�b\"]: Expected �an int at line 3, column 7");
		check(std::format("{}", failure) == failure.description());
		check(failure.path.find(control) != std::string::npos && failure.message.find(control) != std::string::npos);
		const auto mismatch = soml::decode<std::vector<int>>("['prefix" + control + "suffix']");
		check(!mismatch && mismatch.error().description().find(control) == std::string::npos && mismatch.error().message.find(control) != std::string::npos);
		const std::string keyed_source = "'a" + control + "b': 'x'";
		const auto keyed_mismatch = soml::decode<std::map<std::string, int>>(keyed_source);
		check(!keyed_mismatch && keyed_mismatch.error().path.find(control) != std::string::npos && keyed_mismatch.error().offset == keyed_source.find("'x'") && keyed_mismatch.error().description().find(control) == std::string::npos);
	}
	check(soml::error{.message = "héllo العربية עברית 😀", .path = "café"}.description() == "café: héllo العربية עברית 😀");
	check(soml::error{.message = std::string{"\xD8\xE2\x80", 3}, .path = {}}.description() == std::string{"\xD8\xE2\x80", 3});

	const std::vector<char> input{'a', ':', ' ', '1'};
	check(soml::parse(input) == soml::parse("a: 1"));
	check(!soml::parse(std::vector<char>{}));
	check(!soml::parse(std::vector<char>{'a', ':', '1', '\0'}));
	check(soml::from_value<std::nullptr_t>(soml::value{}).has_value());
	check(!soml::from_value<std::nullptr_t>(soml::value{false}));
	check(soml::decode<std::vector<std::nullptr_t>>("[null, null]")->size() == 2);
	check(soml::encode(std::vector<std::nullptr_t>{nullptr, nullptr}) == "[\n\tnull\n\tnull\n]\n");

	// Batch construction has the same duplicate and ordering semantics as individual insertion.
	std::vector<soml::member> members;
	soml::object expected;

	for (int index = 0; index < 5000; index++) {
		const std::string key = std::to_string((index * 31) % 1000);
		members.push_back({key, index});
		expected.insert_or_assign(key, index);
	}

	const soml::object actual{std::move(members)};
	check(actual == expected);
	check(soml::write(actual) == soml::write(expected));
	check(soml::object{std::vector<soml::member>{}}.empty());
	const soml::object empty_key{std::vector<soml::member>{{"", 1}, {"x", 2}, {"", 3}}};
	check(empty_key.size() == 2 && empty_key.begin()->key.empty() && empty_key.find("")->as<int>() == 3);

	const auto invalid_key = soml::write(soml::object{{std::string{"\xff", 1}, 1}});
	check(!invalid_key);
	check(invalid_key.error().path == "[\"\\u00ff\"]");
	check(soml::detail::classify_string(invalid_key.error().description()) != soml::detail::string_kind::invalid_utf8);
	check(soml::detail::json_quote("héllo") == "\"héllo\"");
	check(soml::detail::json_quote(std::string{"\xc0\xaf", 2}) == "\"\\u00c0\\u00af\"");

	// A type-error diagnostic quotes its displayed prefix; an omitted CR or invalid byte cannot change its quotes or UTF-8 validity.
	const std::string prefix(1000, 'a');
	for (const std::string& suffix : {std::string{"\r"}, std::string{"\xff", 1}}) {
		const auto mismatch = soml::from_value<int>(soml::value{prefix + suffix});
		check(!mismatch && mismatch.error().code == soml::error_code::type_mismatch);
		check(mismatch.error().message == "Expected an int, but found the string '" + std::string(40, 'a') + "…'");
		check(mismatch.error().message.size() < 300);
		check(soml::detail::classify_string(mismatch.error().message) != soml::detail::string_kind::invalid_utf8);
	}

	// Every invalid continuation byte counts toward the diagnostic limit, including after a valid displayed prefix.
	for (int byte = 0x80; byte <= 0xBF; byte++) {
		for (const std::size_t size : {std::size_t{201}, std::size_t{1000}}) {
			const std::string text(size, static_cast<char>(byte));
			const std::string displayed = std::string(40, static_cast<char>(byte)) + "…";
			check(soml::detail::abbreviate(text) == displayed);
			const auto mismatch = soml::from_value<int>(soml::value{text});
			check(!mismatch && mismatch.error().code == soml::error_code::type_mismatch);
			check(mismatch.error().message == "Expected an int, but found the string " + soml::detail::json_quote(displayed));

			const auto invalid_key_result = soml::write(soml::object{{text, 1}});
			check(!invalid_key_result && invalid_key_result.error().code == soml::error_code::invalid_value);
			const std::string expected_path = "[" + soml::detail::json_quote(std::string(200, static_cast<char>(byte)) + "…") + "]";
			check(invalid_key_result.error().path == expected_path);
			const auto invalid_key_mismatch = soml::from_value<std::map<std::string, int>>(soml::object{{text, "wrong type"}});
			check(!invalid_key_mismatch && invalid_key_mismatch.error().code == soml::error_code::type_mismatch && invalid_key_mismatch.error().path == expected_path);
			check(soml::detail::abbreviate(std::string(40, 'a') + text) == std::string(40, 'a') + "…");
		}
	}

	// Unit diagnoses keep their spelling suggestions without copying an arbitrarily long unknown unit.
	for (const std::string_view unit : {"H", "S", "MS", "mS", "Ms", "US", "NS"}) {
		std::string lowercase;
		for (const char character : unit) {
			lowercase.push_back(static_cast<char>(character | 0x20));
		}
		const auto parsed = soml::parse("value: 1" + std::string{unit});
		check(!parsed && parsed.error().message == "Invalid duration 1" + std::string{unit} + ": the units are lowercase: " + lowercase);
	}
	const auto long_unit = soml::parse("value: 1" + std::string(100'000, 'm'));
	check(!long_unit && long_unit.error().message.size() < 200 && long_unit.error().message.contains("“mmmmmmmmmm…” is not a unit"));

	// Huge lowercase radix literals get a bounded diagnosis; ordinary suggestions and malformed-digit priority stay intact.
	const std::string long_hex = "0x" + std::string(100'000, '0') + "a";
	const auto lowercase_hex = soml::parse("value: " + long_hex);
	check(!lowercase_hex && lowercase_hex.error().message == "Hexadecimal digits are uppercase");
	check(soml::parse("value: 0x0a").error().message == "Hexadecimal digits are uppercase: 0x0A");
	check(soml::parse("value: " + long_hex + "_").error().message == "An underscore in a number must be between two digits");
	check(soml::parse("value: " + long_hex + "g").error().message == "Invalid hexadecimal digit “g”");

	// Large malformed numbers keep bounded hints while ordinary spelling suggestions remain useful.
	const auto octal_hint = soml::parse("value: 007");
	check(!octal_hint && octal_hint.error().message.contains("Write an octal number, such as a file mode, as 0o7"));
	const auto thousands_hint = soml::parse("[1,000]");
	check(!thousands_hint && thousands_hint.error().message.ends_with("Write it as 1_000 or 1000"));
	const auto unit_hint = soml::parse("[10 s]");
	check(!unit_hint && unit_hint.error().message.contains("as in 10s") && unit_hint.error().message.ends_with("as in '10 s'"));
	const std::string long_fraction = "1." + std::string(100'000, '0');
	const auto oversized_unit_hint = soml::parse("[" + long_fraction + " h]");
	check(!oversized_unit_hint && oversized_unit_hint.error().message.contains("as in 10s") && oversized_unit_hint.error().message.ends_with("as in '1." + std::string(38, '0') + "…'"));
	check(soml::parse("[2026-09-19 14:00:00Z]").error().message.ends_with("as in 2026-09-19T14:00:00Z"));
	check(soml::parse("[10µs]").error().message == "The unit for microseconds is written us, as in 10us");
	check(soml::parse("a:\n  b: 1").error().message == "Expected a value, but found the key b. Indentation does not nest objects, so write a: {b: …}");
	check(soml::parse("['''\nx'''").error().message.contains("move the ''' at the end of line 2 to a new line"));

	std::println("{} polish/API failures", failures);
	return failures == 0 ? 0 : 1;
}
