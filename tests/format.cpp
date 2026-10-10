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

}

int main() {
	int failures = 0;
	std::size_t count = 0;
	for (const auto& entry : std::filesystem::recursive_directory_iterator{"tests/conformance/valid"}) {
		const auto path = entry.path();
		const auto name = path.string();
		if (!name.ends_with(".formatted.soml")) {
			continue;
		}
		const auto source = read_file(name.substr(0, name.size() - std::string_view{".formatted.soml"}.size()) + ".soml");
		const auto expected = read_file(path);
		const auto formatted = soml::format(source);
		if (!formatted || *formatted != expected) {
			std::println(stderr, "{}: formatting mismatch\nExpected:\n{}\nGot:\n{}", name, expected, formatted ? *formatted : formatted.error().description());
			failures++;
			continue;
		}
		const auto repeated = soml::format(*formatted);
		if (!repeated || *repeated != *formatted || soml::parse(source) != soml::parse(*formatted)) {
			std::println(stderr, "{}: idempotence or value preservation failed", name);
			failures++;
		}
		count++;
	}
	const auto invalid = soml::format("a: [1,,2]");
	const auto parsed_invalid = soml::parse("a: [1,,2]");
	if (invalid || invalid.error().description() != parsed_invalid.error().description()) {
		std::println(stderr, "invalid input did not return the parser error");
		failures++;
	}
	for (const auto& [source, expected] : std::array{
		std::pair{"a: /* header */\n[1,\n2]", "a: /* header */\n\t[\n\t\t1\n\t\t2\n\t]\n"},
		std::pair{"a:\n/* first */\n/* second */\n[1,\n2]", "a:\n\t/* first */\n\t/* second */\n\t[\n\t\t1\n\t\t2\n\t]\n"},
	}) {
		const auto formatted = soml::format(source);
		if (formatted != expected || soml::parse(*formatted) != soml::parse(source)) {
			std::println(stderr, "comments between a key and multiline value must keep the value indentation");
			failures++;
		}
	}
	for (std::size_t alignment = 0; alignment < 8; alignment++) {
		const std::string key = std::string(alignment, 'a') + "'\"\u008a§\u0700\\";
		const std::string content = std::string(alignment, 'a') + "'\"\u008a§\u0700\\\n" + std::string(24, 'b');
		const std::string source = soml::write_canonical(soml::object{{key, content}}).value();
		if (soml::format(source) != source) {
			std::println(stderr, "quoted source scanning must preserve escapes and UTF-8 at every batch alignment");
			failures++;
		}
	}
	for (const bool multiline : {false, true}) {
		const std::string source = "[1" + std::string(100'000, ' ') + "\t/* ignored , comma */\t," + (multiline ? "\n \t " : "\t ") + "2]";
		const std::string_view expected = multiline ? "[\n\t1 /* ignored , comma */\n\t2\n]\n" : "[1 /* ignored , comma */, 2]\n";
		const auto formatted = soml::format(source);
		if (formatted != expected || soml::parse(*formatted) != soml::parse(source)) {
			std::println(stderr, "comma scanning must skip space and tab runs while respecting comments and line breaks");
			failures++;
		}
	}
	for (const std::string_view padding : {"", " ", "\t", " \t \t"}) {
		const std::string whitespace{padding};
		const std::string source = "/*" + whitespace + '\n' + whitespace + '\n' + whitespace + "\ntext" + whitespace + "\n*/" + whitespace + "\na: 1";
		const auto formatted = soml::format(source);
		if (formatted != "/*\n\ntext\n*/\na: 1\n" || soml::format(*formatted) != formatted) {
			std::println(stderr, "block comment trimming must collapse empty and whitespace-only lines without retaining trailing blanks");
			failures++;
		}
	}
	std::println("{} formatting fixtures, {} failures", count, failures);
	return failures == 0 ? 0 : 1;
}
