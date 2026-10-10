#include <soml.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>

namespace {

std::string read_file(const std::filesystem::path& path) {
	std::ifstream file{path, std::ios::binary};
	return {std::istreambuf_iterator<char>{file}, {}};
}

soml::value untag(const soml::value& tagged) {
	if (const auto* items = tagged.get_if<soml::array>()) {
		soml::array result;

		for (const auto& item : *items) {
			result.push_back(untag(item));
		}

		return result;
	}

	const auto& object = *tagged.get_if<soml::object>();

	if (const auto* kind = object.find("type"); kind != nullptr && kind->is_string()) {
		const std::string& text = *object.find("value")->get_if<std::string>();

		if (*kind->get_if<std::string>() == "string") {
			return text;
		}

		if (*kind->get_if<std::string>() == "duration") {
			return soml::duration{std::stoll(text)};
		}

		return soml::parse("[" + text + "]")->get_if<soml::array>()->front();
	}

	soml::object result;

	for (const auto& [key, item] : object) {
		result.insert_or_assign(key, untag(item));
	}

	return result;
}

} // namespace

int main() {
	int failures = 0;
	std::size_t count = 0;

	for (const auto& entry : std::filesystem::directory_iterator{"tests/conformance/edit"}) {
		if (entry.path().extension() != ".json") {
			continue;
		}

		const auto fixture = soml::parse(read_file(entry.path())).value();
		const std::string& source = *fixture.find("document")->get_if<std::string>();
		std::vector<soml::path_segment> path;

		for (const auto& segment : *fixture.find("path")->get_if<soml::array>()) {
			if (const auto* key = segment.get_if<std::string>()) {
				path.emplace_back(*key);
			} else {
				path.emplace_back(static_cast<std::size_t>(*segment.get_if<std::int64_t>()));
			}
		}

		std::optional<soml::value> replacement;

		if (const auto* tagged = fixture.find("value")) {
			replacement = untag(*tagged);
		}

		const auto result = soml::edit(source, path, replacement);
		const bool expected_error = fixture.find("error") != nullptr;

		if (expected_error) {
			if (result) {
				std::println(stderr, "{}: expected error, got {}", entry.path().string(), *result);
				failures++;
			}
		} else {
			const auto& expected = *fixture.find("expected")->get_if<std::string>();

			if (!result || *result != expected) {
				std::println(stderr, "{}: mismatch\nExpected:\n{}\nGot:\n{}", entry.path().string(), expected, result ? *result : result.error().description());
				failures++;
			} else if (!soml::parse(*result)) {
				std::println(stderr, "{}: invalid result", entry.path().string());
				failures++;
			}

		}

		count++;
	}

	const auto check = [&](bool condition, std::string_view message) {
		if (!condition) {
			std::println(stderr, "{}", message);
			failures++;
		}
	};
	check(soml::edit("a: 1\n", {"missing"}, std::nullopt) == std::expected<std::string, soml::error>{"a: 1\n"}, "removing a missing member must preserve the source");
	check(!soml::edit("a: 1", {}, 2), "empty path must fail");
	check(!soml::edit("a: [", {"a"}, 2), "invalid source must fail");
	check(!soml::edit("a: 1", {"a"}, std::numeric_limits<double>::quiet_NaN()), "NaN must fail");
	check(!soml::edit("a: 1", {"a"}, std::string{"\r"}), "invalid string must fail");
	check(soml::edit("a: 1 # stays\n", {"a"}, nullptr) == std::expected<std::string, soml::error>{"a: null # stays\n"}, "null is a replacement, not removal");
	check(soml::edit("a: [1, 2]\n", {"a", std::size_t{0}}, 3) == std::expected<std::string, soml::error>{"a: [3, 2]\n"}, "initializer-list path must work");
	check(soml::edit("a: {\n\tb: 1\n}\n", {"a", "b"}, soml::array{1, 2}) == std::expected<std::string, soml::error>{"a: {\n\tb: [\n\t\t1\n\t\t2\n\t]\n}\n"}, "multiline replacements must inherit their indentation");
	for (const std::string_view newline_position : {"before", "after", "none"}) {
		const std::string before = newline_position == "before" ? "\n" : "";
		const std::string after = newline_position == "after" ? "\n" : "";
		const std::string source = std::string(8, '[') + before + "'" + std::string(100'000, 'a') + "'" + after + std::string(8, ']');
		const std::vector<soml::path_segment> path(8, std::size_t{0});
		const std::string expected = std::string(8, '[') + before + (newline_position == "none" ? "[1, 2]" : "[\n\t1\n\t2\n]") + after + std::string(8, ']');
		check(soml::edit(source, path, soml::array{1, 2}) == std::expected<std::string, soml::error>{expected}, "nested edits must inherit line breaks before or after their values and retain compact layout without them");
	}

	// Missing removals never create nesting, regardless of the remaining path length.
	std::vector<soml::path_segment> long_path(soml::max_depth + 1, std::string{"missing"});
	check(soml::edit("a: 1\n", long_path, std::nullopt) == std::expected<std::string, soml::error>{"a: 1\n"}, "a long path under a missing member must preserve the source");
	long_path.front() = std::size_t{2};
	check(soml::edit("[1]\n", long_path, std::nullopt) == std::expected<std::string, soml::error>{"[1]\n"}, "a long path under a missing item must preserve the source");
	long_path.front() = std::string{"a"};
	const auto through_scalar = soml::edit("a: 1\n", long_path, std::nullopt);
	check(!through_scalar && through_scalar.error().code == soml::error_code::type_mismatch, "a long removal path through a scalar must fail with a type error");
	const auto too_deep = soml::edit("{}", long_path, 1);
	check(!too_deep && too_deep.error().code == soml::error_code::too_deep, "setting a long path must still respect the nesting limit");
	const std::string_view invalid_source = "a: [";
	const auto invalid_document = soml::parse(invalid_source);
	const auto invalid_empty_path = soml::edit(invalid_source, {}, 1);
	const auto invalid_long_path = soml::edit(invalid_source, long_path, 1);
	check(!invalid_empty_path && !invalid_long_path && invalid_empty_path.error() == invalid_document.error() && invalid_long_path.error() == invalid_document.error(), "source parsing errors must precede empty or excessive edit paths");
	for (const soml::value& replacement : {soml::value{1}, soml::value{std::numeric_limits<double>::quiet_NaN()}, soml::value{soml::array{1, 2, 3}}}) {
		const auto missing_array = soml::edit("{}", {"missing", std::size_t{0}, "child"}, replacement);
		check(!missing_array && missing_array.error().code == soml::error_code::out_of_range && missing_array.error().message == "A missing value is created as an object and cannot have an array index", "a missing intermediate array must fail before validating the replacement");
	}

	// A comma before a removed last item stays when a line break separates them.
	check(soml::edit("a: [1,\n2\n]", {"a", std::size_t{1}}, std::nullopt) == std::expected<std::string, soml::error>{"a: [1,\n]"}, "removal must preserve a comma across a line break");
	// A block comment can carry ownership across several physical lines.
	check(soml::edit("[1,\n/* leading\n comment */ 2 /* trailing\n comment */\n]", {std::size_t{1}}, std::nullopt) == std::expected<std::string, soml::error>{"[1,\n]"}, "removal must take multiline comments owned by the item");
	// A comment after the trailing comma is unowned when the bracket follows on its line.
	check(soml::edit("a: [1,\n# Keep\n2, /* unowned */]\n", {"a", std::size_t{1}}, std::nullopt) == std::expected<std::string, soml::error>{"a: [1,\n# Keep\n/* unowned */]\n"}, "removal must retain unowned comments after its comma");
	check(soml::edit("a: [1, {}]\n", {"a", std::size_t{1}, "new"}, soml::object{{"x", soml::array{2, 3}}}) == std::expected<std::string, soml::error>{"a: [1, {new: {x: [2, 3]}}]\n"}, "one-line layout must propagate through empty nested containers");
	const std::string key{"a\0b", 3};
	const auto inserted = soml::edit("{}", {key}, 4);
	check(inserted == std::expected<std::string, soml::error>{"{\n\t\"a\\u{0}b\": 4\n}"}, "inserting a key must preserve its embedded NUL");
	if (inserted) {
		const auto parsed = soml::parse(*inserted);
		check(parsed && (*parsed)[key].as<int>() == 4, "the inserted key must be looked up by its full length");
	}
	const auto invalid_key = soml::edit("{}", {std::string{"\xff", 1}}, 1);
	check(!invalid_key && invalid_key.error().code == soml::error_code::invalid_value, "inserting an invalid UTF-8 key must fail");

	// Compact replacements must share canonical spelling and validation with multiline replacements.
	const soml::value mixed = soml::array{nullptr, false, -123, 1.25, "it's\n\t", soml::instant{0, 1}, soml::duration{-1}, soml::object{{"odd key", soml::array{}}, {"empty", soml::object{}}}};
	const soml::value expected = soml::object{{"payload", soml::array{mixed, 9}}};
	const std::string compact = "[null, false, -123, 1.25, \"it's\\n\\t\", 1970-01-01T00:00:00.000000001Z, -0.000000001s, {'odd key': [], empty: {}}]";
	check(soml::edit("payload: [0, 9] # retained\n", {"payload", std::size_t{0}}, mixed) == std::expected<std::string, soml::error>{"payload: [" + compact + ", 9] # retained\n"}, "compact array replacement must spell every scalar type correctly");
	check(soml::edit("payload: [0, 9] # retained\n", {"payload", std::size_t{2}}, mixed) == std::expected<std::string, soml::error>{"payload: [0, 9, " + compact + "] # retained\n"}, "compact array insertion must separate nested values with commas");
	check(soml::edit("payload: {old: 0, keep: 9} # retained\n", {"payload", "old"}, mixed) == std::expected<std::string, soml::error>{"payload: {old: " + compact + ", keep: 9} # retained\n"}, "compact object replacement must preserve member order and comments");

	for (const std::string_view source : {"payload: [0, 9] # retained\n", "payload: [\n\t0\n\t9\n] # retained\n"}) {
		const auto result = soml::edit(source, {"payload", std::size_t{0}}, mixed);
		check(result && soml::parse(*result) == expected, "editing either layout must preserve the entire replacement and the untouched sibling");

		for (const soml::value& invalid : {soml::value{std::numeric_limits<double>::quiet_NaN()}, soml::value{std::string{"\xff", 1}}, soml::value{soml::instant{0, -1}}}) {
			const auto failed = soml::edit(source, {"payload", std::size_t{0}}, soml::object{{"inner", soml::array{1, soml::object{{"bad", invalid}}}}});
			check(!failed && failed.error().code == soml::error_code::invalid_value && failed.error().path == "inner[1].bad", "replacement validation must report the nested failing path in either layout");
		}

		// Mutable member keys can introduce duplicates after object construction.
		for (const std::size_t size : {2UZ, 17UZ}) {
			soml::object duplicate;
			for (std::size_t index = 0; index < size; index++) {
				duplicate.insert_or_assign("key" + std::to_string(index), 0);
			}
			duplicate.begin()[1].key = "key0";
			const auto failed = soml::edit(source, {"payload", std::size_t{0}}, soml::object{{"nested", std::move(duplicate)}});
			check(!failed && failed.error().code == soml::error_code::invalid_value && failed.error().path == "nested", "both duplicate-check algorithms must reject invalid replacements in either layout");
		}

		soml::value deep = 0;
		for (std::size_t level = 0; level < soml::max_depth - 2; level++) {
			deep = soml::array{std::move(deep)};
		}
		check(soml::edit(source, {"payload", std::size_t{0}}, deep).has_value(), "a replacement reaching exactly the depth limit must succeed in either layout");
		deep = soml::array{std::move(deep)};
		const auto failed = soml::edit(source, {"payload", std::size_t{0}}, deep);
		check(!failed && failed.error().code == soml::error_code::too_deep, "replacement depth must include the existing parent containers in either layout");
	}

	std::println("{} edit fixtures, {} failures", count, failures);
	return failures == 0 ? 0 : 1;
}
