/*
The examples in the readme, compiled and run, so that they stay correct. Each block prints what the readme says it prints.
*/
#include <soml.hpp>

#include <chrono>
#include <map>
#include <print>
#include <string>
#include <vector>

using namespace std::literals;

namespace {

struct postgres {
	std::string host;
	std::uint16_t port = 5432;
};

struct config {
	std::string name;
	int replicas = 1;
	std::chrono::seconds timeout{30};
	std::vector<std::string> labels;
	postgres database;
};

} // namespace

#if !defined(__cpp_impl_reflection)
template<>
inline constexpr auto soml::fields<postgres> = std::tuple{
	soml::field{"host", &postgres::host},
	soml::field{"port", &postgres::port},
};

template<>
inline constexpr auto soml::fields<config> = std::tuple{
	soml::field{"name", &config::name},
	soml::field{"replicas", &config::replicas},
	soml::field{"timeout", &config::timeout},
	soml::field{"labels", &config::labels},
	soml::field{"database", &config::database},
};
#endif

int main() {
	// Read
	{
		auto document = soml::parse(R"(
# The edge service.
name: 'api-gateway'
replicas: 3
launch-date: 2027-09-19
timeout: 1m30s
postgres: {host: 'db.internal', port: 5432}
)");

		if (!document) {
			std::println(stderr, "{}", document.error());
			return 1;
		}

		std::println("{}", (*document)["name"].value_or("unnamed"));
		//=> api-gateway

		std::println("{}", (*document)["postgres"]["port"].value_or(0));
		//=> 5432

		std::println("{}", (*document)["timeout"].as<std::chrono::seconds>()->count());
		//=> 90

		std::println("{}", (*document)["missing"]["deeper"].value_or(8080));
		//=> 8080
	}

	// Iterate
	{
		const soml::value document = *soml::parse("a: 1\nb: [true, 'x']");

		for (const auto& [key, item] : *document.get_if<soml::object>()) {
			std::println("{}: {}", key, item.type());
		}
		//=> a: int
		//   b: array
	}

	// Write
	{
		const soml::value document = soml::object{
			{"name", "api-gateway"},
			{"replicas", 3},
			{"timeout", soml::duration{90s}},
			{"labels", soml::array{"prod", "eu-west"}},
		};

		std::print("{}", *soml::write(document));
		//=> name: 'api-gateway'
		//   replicas: 3
		//   timeout: 1m30s
		//   labels: [
		//   	'prod'
		//   	'eu-west'
		//   ]

		std::print("{}", *soml::write_canonical(document));
		//=> labels: [
		//   	'prod'
		//   	'eu-west'
		//   ]
		//   name: 'api-gateway'
		//   replicas: 3
		//   timeout: 1m30s
	}

	// Typed
	{
		auto result = soml::decode<config>(R"(
name: 'api-gateway'
timeout: 1m30s
labels: ['prod', 'eu-west']
database: {host: 'db.internal'}
)");

		if (!result) {
			std::println(stderr, "{}", result.error());
			return 1;
		}

		std::println("{} {} {} {}", result->replicas, result->timeout.count(), result->database.host, result->database.port);
		//=> 1 90 db.internal 5432

		std::print("{}", *soml::encode(*result));
		//=> name: 'api-gateway'
		//   replicas: 1
		//   timeout: 1m30s
		//   labels: [
		//   	'prod'
		//   	'eu-west'
		//   ]
		//   database: {
		//   	host: 'db.internal'
		//   	port: 5432
		//   }

		std::println("{}", soml::decode<config>("database: {port: 70000}").error());
		//=> database.port: The int 70000 is outside the range of the type, 0 to 65535 at line 1, column 18

		std::println("{}", soml::decode<config>("replicas: 3\nreplica: 4").error());
		//=> replica: Unknown key replica, which the type has no field for. Did you mean replicas? at line 2, column 1
	}

	// Appending, byte buffers, and bulk construction.
	{
		const std::vector<char> text{'a', ':', ' ', '1'};
		const auto document = soml::parse(text);
		std::string output = "# Generated configuration\n";

		if (!document || !soml::write(*document, output) || output != "# Generated configuration\na: 1\n") {
			return 1;
		}

		std::vector<soml::member> members{{"host", "localhost"}, {"port", 8080}};
		const soml::object settings{std::move(members)};

		if (soml::write(settings) != "host: 'localhost'\nport: 8080\n") {
			return 1;
		}
	}

	// Formatting and editing
	{
		const auto formatted = soml::format("port:   0x1F90  # HTTP\n");
		const auto changed = soml::edit("port: 8080 # HTTP\n", {"port"}, 9090);
		const auto added = soml::edit("server: {host: 'localhost'}\n", {"server", "port"}, 8080);
		const auto removed = soml::edit("a: 1\nb: 2\n", {"a"}, std::nullopt);

		if (!formatted || *formatted != "port: 0x1F90 # HTTP\n" || !changed || *changed != "port: 9090 # HTTP\n" || !added || *added != "server: {host: 'localhost', port: 8080}\n" || !removed || *removed != "b: 2\n") {
			return 1;
		}
	}

	// Errors
	{
		const std::string_view text = "name: 'api'\nport: 80 80\n";
		const soml::error error = soml::parse(text).error();
		std::println("{}", error);
		//=> Expected a line break before the next entry, but found “8” at line 2, column 10

		std::println("{}", error.code_frame(text));
		//=>   1 | name: 'api'
		//   > 2 | port: 80 80
		//       |          ^
	}

	// Compile time
	{
		static_assert(soml::parse("port: 8080")->find("port")->as<int>() == 8080);
		static_assert(!soml::parse("port: 0x1g"));
	}

	return 0;
}
