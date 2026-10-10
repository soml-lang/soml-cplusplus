/*
A command-line tool for the conformance and differential tests, with the same protocol as the C implementation's. For each file given, it prints one line of JSON:

	{"file": "...", "ok": true, "value": <tagged value>, "canonical": "..."}
	{"file": "...", "ok": false, "code": 1, "message": "...", "line": 1, "column": 5}

The tagged value is the format of the conformance suite: arrays and objects as JSON, and every scalar as `{"type": "...", "value": "..."}`. A float's value is its canonical spelling, an instant's is its canonical UTC form, and a duration's is its length in nanoseconds.

With `-` as the only argument, it reads the paths from standard input, one per line.
*/
#include <soml.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

namespace {

void print_json_string(std::string& output, std::string_view text) {
	output.push_back('"');

	for (const char character : text) {
		const auto byte = static_cast<unsigned char>(character);

		if (character == '"' || character == '\\') {
			output.push_back('\\');
			output.push_back(character);
		} else if (byte < 0x20 || byte == 0x7F) {
			output += std::format("\\u{:04x}", byte);
		} else {
			output.push_back(character);
		}
	}

	output.push_back('"');
}

void print_scalar(std::string& output, std::string_view type, std::string_view text) {
	output += R"({"type":")";
	output += type;
	output += R"(","value":)";
	print_json_string(output, text);
	output.push_back('}');
}

void print_tagged(std::string& output, const soml::value& value) {
	std::string text;

	switch (value.type()) {
		case soml::type::null: {
			print_scalar(output, "null", "null");
			return;
		}

		case soml::type::boolean: {
			print_scalar(output, "bool", *value.get_if<bool>() ? "true" : "false");
			return;
		}

		case soml::type::integer: {
			print_scalar(output, "int", std::to_string(*value.get_if<std::int64_t>()));
			return;
		}

		case soml::type::floating: {
			soml::detail::append_float(text, *value.get_if<double>());
			print_scalar(output, "float", text);
			return;
		}

		case soml::type::string: {
			print_scalar(output, "string", *value.get_if<std::string>());
			return;
		}

		case soml::type::date: {
			soml::detail::append_date(text, *value.get_if<soml::date>());
			print_scalar(output, "date", text);
			return;
		}

		case soml::type::instant: {
			soml::detail::append_instant(text, *value.get_if<soml::instant>());
			print_scalar(output, "instant", text);
			return;
		}

		case soml::type::duration: {
			print_scalar(output, "duration", std::to_string(value.get_if<soml::duration>()->count()));
			return;
		}

		case soml::type::array: {
			output.push_back('[');
			bool is_first = true;

			for (const soml::value& item : *value.get_if<soml::array>()) {
				if (!is_first) {
					output.push_back(',');
				}

				is_first = false;
				print_tagged(output, item);
			}

			output.push_back(']');
			return;
		}

		case soml::type::object: {
			output.push_back('{');
			bool is_first = true;

			for (const auto& [key, item] : *value.get_if<soml::object>()) {
				if (!is_first) {
					output.push_back(',');
				}

				is_first = false;
				print_json_string(output, key);
				output.push_back(':');
				print_tagged(output, item);
			}

			output.push_back('}');
			return;
		}
	}
}

void run(const std::string& path) {
	std::ifstream file{path, std::ios::binary};
	const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
	std::string output = R"({"file":)";
	print_json_string(output, path);
	const auto document = soml::parse(text);

	if (!document) {
		output += std::format(R"(,"ok":false,"code":{},"message":)", static_cast<int>(document.error().code));
		print_json_string(output, document.error().message);
		output += std::format(R"(,"line":{},"column":{}}})", document.error().line, document.error().column);
	} else {
		output += R"(,"ok":true,"value":)";
		print_tagged(output, *document);
		const auto canonical = soml::write_canonical(*document);
		output += R"(,"canonical":)";
		print_json_string(output, canonical ? *canonical : std::format("WRITE ERROR: {}", canonical.error().description()));
		output.push_back('}');
	}

	std::cout << output << '\n';
}

} // namespace

int main(int argument_count, char** arguments) {
	if (argument_count == 2 && std::string_view{arguments[1]} == "-") {
		std::string path;

		while (std::getline(std::cin, path)) {
			if (!path.empty()) {
				run(path);
			}
		}

		return 0;
	}

	for (int index = 1; index < argument_count; index++) {
		run(arguments[index]);
	}

	return 0;
}
