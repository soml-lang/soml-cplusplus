#include <soml.hpp>

struct configuration {
	std::string name;
	int port = 0;
};

template<>
inline constexpr auto soml::fields<configuration> = std::tuple{
	soml::field{"name", &configuration::name},
	soml::field{"port", &configuration::port},
};

int main() {
	const auto decoded = soml::decode<configuration>("name: 'package'\nport: 8080\n");

	if (!decoded || decoded->name != "package" || decoded->port != 8080) {
		return 1;
	}

	const auto encoded = soml::to_value(*decoded);

	if (!encoded) {
		return 1;
	}

	const auto text = soml::write(*encoded);

	if (!text) {
		return 1;
	}

	const auto round_trip = soml::decode<configuration>(*text);
	return round_trip && round_trip->name == decoded->name && round_trip->port == decoded->port ? 0 : 1;
}
