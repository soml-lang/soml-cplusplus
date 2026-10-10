#include <soml.hpp>

#include <print>
#include <string>

static_assert(soml::detail::is_shown_by_code_point(U'\u0600'));
static_assert(!soml::detail::is_shown_by_code_point(U'\U0001E6C0'));

int main() {
	int failures = 0;
	// Format, unassigned, private-use, separator, and default-ignorable characters, including supplementary planes.
	for (const char32_t scalar : {U'\u0600', U'\u0378', U'\uE000', U'\u2028', U'\u034F', U'\U00013430', U'\U000E0100', U'\U0010FFFF'}) {
		std::string text;
		soml::detail::append_utf8(text, scalar);
		const auto parsed = soml::parse(text + ": 1");
		const auto code_point = soml::detail::format_code_point(scalar);

		if (parsed || !parsed.error().message.contains(code_point) || parsed.error().message.contains(text)) {
			std::println(stderr, "{} must be displayed by code point", code_point);
			failures++;
		}

		// The display policy does not reject these characters inside quoted strings.
		const auto quoted = soml::parse("value: '" + text + "'");
		if (!quoted || quoted->find("value")->as<std::string>() != text) {
			std::println(stderr, "{} changed quoted-string parsing", code_point);
			failures++;
		}
	}

	// Ordinary letters, symbols, and a letter assigned in Unicode 17 retain their readable spelling.
	for (const char32_t scalar : {U'A', U'é', U'\u4E00', U'\U0001F600', U'\U0001E6C0'}) {
		std::string text;
		soml::detail::append_utf8(text, scalar);
		const auto parsed = soml::parse("value: " + text);

		if (parsed || !parsed.error().message.contains(text)) {
			std::println(stderr, "{} must keep its visible spelling", soml::detail::format_code_point(scalar));
			failures++;
		}
	}

	// Literal and escaped strings must validate every byte at and across word boundaries.
	for (std::size_t padding = 0; padding < 32; padding++) {
		const std::string prefix = (padding < 16 ? "plain" : "it's") + std::string(padding % 16, 'a');
		for (int byte = 0; byte < 128; byte++) {
			const std::string text = prefix + static_cast<char>(byte) + std::string(32, 'z');
			const soml::value document = soml::array{text};
			const auto written = soml::write(document);
			if (byte == '\r') {
				if (written || written.error().code != soml::error_code::invalid_value) {
					std::println(stderr, "carriage return at padding {} was accepted", padding);
					failures++;
				}
			} else if (!written || soml::parse(*written) != document) {
				std::println(stderr, "ASCII byte {} at padding {} did not round-trip", byte, padding);
				failures++;
			}
		}

		for (const std::string& suffix : {std::string{"\xe2\x82\xac"}, std::string{"\xf0\x9f\x98\x80"}}) {
			const soml::value document = soml::array{prefix + suffix + std::string(32, 'z')};
			const auto written = soml::write(document);
			if (!written || soml::parse(*written) != document) {
				std::println(stderr, "UTF-8 at padding {} did not round-trip", padding);
				failures++;
			}
		}

		for (const std::string& suffix : {std::string{"\xff"}, std::string{"\xe2\x82"}, std::string{"\xed\xa0\x80"}}) {
			const auto written = soml::write(soml::array{prefix + std::string(32, 'z') + suffix});
			if (written || written.error().code != soml::error_code::invalid_value) {
				std::println(stderr, "invalid UTF-8 after escaped text at padding {} was accepted", padding);
				failures++;
			}
		}
	}

	return failures == 0 ? 0 : 1;
}
