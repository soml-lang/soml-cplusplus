#include <array>
#include <chrono>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#if defined(__cpp_impl_reflection)
#include <meta>
#endif

import soml;

struct port {
	int number;
};

template<>
struct soml::converter<port> {
	static constexpr std::expected<port, soml::error> from_value(const soml::value& source, const soml::decode_options& options) {
		return soml::converter<int>::from_value(source, options).transform([](int number) {
			return port{number};
		});
	}

	static constexpr std::expected<soml::value, soml::error> to_value(const port& source) {
		return soml::value{source.number};
	}
};

enum class mode {
	development,
	production,
};

template<>
inline constexpr auto soml::enum_names<mode> = std::array{
	std::pair{mode::development, std::string_view{"development"}},
	std::pair{mode::production, std::string_view{"production"}},
};

struct configuration {
	port listen{80};
	mode environment = mode::development;
};

template<>
inline constexpr auto soml::fields<configuration> = std::tuple{
	soml::field{"listen", &configuration::listen},
	soml::field{"environment", &configuration::environment},
};

soml::value header_document();

#if defined(__cpp_impl_reflection)
struct reflected_configuration {
	[[=soml::key("listen-port")]] int listen_port = 80;
};

static_assert(soml::decode<reflected_configuration>("listen-port: 443")->listen_port == 443);
#endif

static_assert(soml::parse("[1582-10-10]")->get_if<soml::array>()->front().get_if<soml::date>()->day() == std::chrono::day{10});
static_assert(soml::max_depth == 100);
static_assert(soml::decodable<configuration> && soml::encodable<configuration>);
static_assert(soml::parse("port: 8080")->find("port")->as<int>() == 8080);
static_assert(soml::parse(u8"name: 'é 🦀'")->find("name")->as<std::string_view>() == "é 🦀");
static_assert(soml::view{soml::value{"stored"}}.value_or("fallback") == "stored");

int main() {
	const auto decoded = soml::decode<configuration>("listen: 8080\nenvironment: 'production'");

	if (!decoded || decoded->listen.number != 8080 || decoded->environment != mode::production || soml::encode(*decoded).value() != "listen: 8080\nenvironment: 'production'\n") {
		return 1;
	}

	if (soml::format("port:  80 # comment").value() != "port: 80 # comment\n" || soml::edit("port: 80 # comment", {"port"}, 443).value() != "port: 443 # comment") {
		return 2;
	}

	const auto invalid = soml::parse("port:");

	if (invalid || std::format("{}", invalid.error()) != invalid.error().description() || std::format("{}", soml::type::integer) != "int" || std::format("{}", soml::error_code::syntax) != "syntax" || std::format("{}", soml::instant{}) != "1970-01-01T00:00:00Z") {
		return 3;
	}

	const std::vector<char> bytes{'[', '1', ']'};
	const auto document = soml::parse(bytes);
	std::string output;

	if (!document || !soml::write(*document, output) || output != "[\n\t1\n]\n" || !soml::write_canonical(header_document(), output) || output != "[\n\t1\n]\nport: 8080\n") {
		return 4;
	}

	soml::array native_items{soml::object{{"name", "native"}}, 1};
	const auto native_text = soml::encode(native_items);
	const soml::object native_members{{"z", 2}, {"a", 1}};
	std::string native_output;

	if (soml::write(native_items) != native_text || soml::write_canonical(native_items) != soml::encode_canonical(native_items) || !soml::write(native_items, native_output) || native_output != native_text.value()) {
		return 6;
	}

	native_output.clear();
	if (soml::write(native_members) != soml::encode(native_members) || soml::write_canonical(native_members) != "a: 1\nz: 2\n" || !soml::write_canonical(native_members, native_output) || native_output != "a: 1\nz: 2\n") {
		return 7;
	}

	auto native_value = soml::to_value(std::move(native_items));
	if (!native_text || !native_value || soml::decode<soml::array>(*native_text) != soml::from_value<soml::array>(std::move(*native_value))) {
		return 5;
	}

	std::vector<std::string> strings{std::string(128, 'x')};
	const char* pointer = strings.front().data();
	auto converted = soml::to_value(std::move(strings));
	if (!converted || (*converted)[0].as<std::string_view>()->data() != pointer) {
		return 8;
	}
	const auto restored = soml::from_value<std::vector<std::string>>(std::move(*converted));
	if (!restored || restored->front().data() != pointer || soml::encode(std::vector<std::string>{"module"}) != "[\n\t'module'\n]\n" || soml::encode_canonical(std::vector<std::string>{"module"}) != "[\n\t'module'\n]\n") {
		return 9;
	}

	return 0;
}
