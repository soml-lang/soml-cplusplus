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

static_assert(soml::detail::json_quote("\"\\\n\t😀") == "\"\\\"\\\\\\n\\t😀\"");
static_assert(soml::detail::find_shortest_digits(std::numeric_limits<double>::max()).count == std::numeric_limits<double>::max_digits10);

void quoted_boundaries(test_results& results) {
	for (std::size_t prefix_size = 0; prefix_size < 24; prefix_size++) {
		const std::string prefix(prefix_size, 'a');
		for (std::size_t suffix_size = 0; suffix_size < 24; suffix_size++) {
			const std::string suffix(suffix_size, 'z');
			for (int character = 0; character < 128; character++) {
				std::string escaped;
				switch (character) {
					case '"': {
						escaped = "\\\"";
						break;
					}
					case '\\': {
						escaped = "\\\\";
						break;
					}
					case '\b': {
						escaped = "\\b";
						break;
					}
					case '\f': {
						escaped = "\\f";
						break;
					}
					case '\n': {
						escaped = "\\n";
						break;
					}
					case '\r': {
						escaped = "\\r";
						break;
					}
					case '\t': {
						escaped = "\\t";
						break;
					}
					default: {
						escaped = character < 0x20 ? std::format("\\u{:04x}", character) : std::string(1, static_cast<char>(character));
					}
				}
				results.check("JSON ASCII at every word and tail boundary", soml::detail::json_quote(prefix + static_cast<char>(character) + suffix) == '"' + prefix + escaped + suffix + '"');
			}
			for (const auto& [bytes, escaped] : std::array{
				std::pair{std::string_view{"😀"}, std::string_view{"😀"}},
				std::pair{std::string_view{"\xC2"}, std::string_view{"\\u00c2"}},
				std::pair{std::string_view{"\xE0\x80\x80"}, std::string_view{"\\u00e0\\u0080\\u0080"}},
				std::pair{std::string_view{"\xF4\x90\x80\x80"}, std::string_view{"\\u00f4\\u0090\\u0080\\u0080"}},
			}) {
				results.check("JSON UTF-8 validation survives fast scan boundaries", soml::detail::json_quote(prefix + std::string{bytes} + suffix) == '"' + prefix + escaped + suffix + '"');
			}
		}
	}
	for (std::size_t length = 0; length < 128; length++) {
		const std::string ordinary(length, 'x');
		results.check("JSON plain strings finish at every word and tail length", soml::detail::json_quote(ordinary) == '"' + ordinary + '"');
	}
}

} // namespace

int main() {
	test_results results;
	quoted_boundaries(results);
	std::println("{} value performance checks, {} failures", results.count, results.failures);
	return results.failures == 0 ? 0 : 1;
}
