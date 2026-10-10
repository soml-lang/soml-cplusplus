/*
Reading, writing, editing, and struct decoding speed on the shared benchmark fixtures (`bench/fixtures`, a copy of the `soml` repository's `benchmark` folder). Run it from the repository root.

With `SOML_BENCH_COMPARE`, which the CMake option of that name sets, it also measures the most used JSON and TOML libraries on the same data: nlohmann/json and simdjson for JSON, and toml++ for TOML.

Each number is the processor time of one call, the fastest of several runs of at least 100 ms each. Calls are batched to amortize the clock query. Processor time, rather than wall time, leaves out the time that the process waits on a busy machine.
*/
#include <soml.hpp>
#include "types.hpp"

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <print>
#include <sstream>
#include <vector>

#if defined(SOML_BENCH_COMPARE)
#include <json.hpp>
#include <simdjson.h>
#include <toml.hpp>
#endif

namespace {

std::string read_file(const std::string& path) {
	std::ifstream file{path, std::ios::binary};

	if (!file) {
		std::println(stderr, "Cannot read {}. Run the benchmark from the repository root.", path);
		std::exit(1);
	}

	return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

/*
The fastest processor time of one call, in nanoseconds.
*/
template<class Operation>
double measure(Operation&& operation) {
	double best = std::numeric_limits<double>::max();

	for (int run = 0; run < 5; run++) {
		std::size_t iterations = 0;
		const std::clock_t start = std::clock();
		if (start == static_cast<std::clock_t>(-1)) {
			std::println(stderr, "Processor timing is unavailable");
			std::exit(1);
		}
		std::clock_t now = start;

		do {
			// Amortize the clock query over a batch, especially for conversion-only measurements.
			for (int iteration = 0; iteration < 64; iteration++) {
				operation();
			}

			iterations += 64;
			now = std::clock();
			if (now == static_cast<std::clock_t>(-1)) {
				std::println(stderr, "Processor timing is unavailable");
				std::exit(1);
			}
		} while (now - start < CLOCKS_PER_SEC / 10);

		best = std::min(best, static_cast<double>(now - start) * 1e9 / CLOCKS_PER_SEC / static_cast<double>(iterations));
	}

	return best;
}

std::string format_time(double nanoseconds) {
	if (nanoseconds >= 1e6) {
		return std::format("{:.2f} ms", nanoseconds / 1e6);
	}

	if (nanoseconds >= 1e3) {
		return std::format("{:.1f} µs", nanoseconds / 1e3);
	}

	return std::format("{:.0f} ns", nanoseconds);
}

struct row {
	std::string name;
	std::vector<std::string> cells;
};

/*
Compare every input value, allowing only additional null fields for optional members absent from an input object.
*/
bool matches_input(const soml::value& input, const soml::value& encoded) {
	if (const auto* members = input.get_if<soml::object>()) {
		const auto* encoded_members = encoded.get_if<soml::object>();

		if (encoded_members == nullptr) {
			return false;
		}

		for (const auto& [key, item] : *members) {
			const auto* encoded_item = encoded_members->find(key);

			if (encoded_item == nullptr || !matches_input(item, *encoded_item)) {
				return false;
			}
		}

		for (const auto& [key, item] : *encoded_members) {
			if (members->find(key) == nullptr && !item.is_null()) {
				return false;
			}
		}

		return true;
	}

	if (const auto* items = input.get_if<soml::array>()) {
		const auto* encoded_items = encoded.get_if<soml::array>();

		if (encoded_items == nullptr || items->size() != encoded_items->size()) {
			return false;
		}

		for (std::size_t index = 0; index < items->size(); index++) {
			if (!matches_input((*items)[index], (*encoded_items)[index])) {
				return false;
			}
		}

		return true;
	}

	return input == encoded;
}

template<class Configuration, class Consume>
void benchmark_struct(std::string_view fixture, Consume&& consume, std::size_t& sink) {
	const std::string text = read_file(std::format("bench/fixtures/{}.soml", fixture));
	const soml::value document = soml::parse(text).value();
	const auto decoded = soml::decode<Configuration>(text);
	const auto converted = soml::from_value<Configuration>(document);
	const auto decoded_value = decoded.and_then([](const Configuration& configuration) {
		return soml::to_value(configuration);
	});
	const auto converted_value = converted.and_then([](const Configuration& configuration) {
		return soml::to_value(configuration);
	});

	// Validate both decoding paths and their struct-to-value encoding against every fixture value before collecting timings. Unknown keys remain errors.
	if (!decoded_value || !converted_value || !matches_input(document, *decoded_value) || !matches_input(document, *converted_value)) {
		std::println(stderr, "Typed fixture validation failed: {}", fixture);
		std::exit(1);
	}

	const auto decode_time = measure([&] {
		sink += consume(soml::decode<Configuration>(text).value());
	});
	const auto conversion_time = measure([&] {
		sink += consume(soml::from_value<Configuration>(document).value());
	});

	const auto encoding_time = measure([&] {
		sink += soml::to_value(*decoded).value().size();
	});

	std::println("| {} | {} | {} | {} |", fixture, format_time(decode_time), format_time(conversion_time), format_time(encoding_time));
}

} // namespace

int main() {
	constexpr std::array<std::string_view, 3> fixtures{"service", "platform", "earthquakes"};
	std::vector<row> rows;
	std::size_t sink = 0;

	for (const std::string_view fixture : fixtures) {
		const std::string soml_text = read_file(std::format("bench/fixtures/{}.soml", fixture));
		const soml::value document = soml::parse(soml_text).value();
		row read{.name = std::format("Read {}", fixture), .cells = {}};
		row write{.name = std::format("Write {}", fixture), .cells = {}};

		read.cells.push_back(format_time(measure([&] {
			sink += soml::parse(soml_text)->size();
		})));

		write.cells.push_back(format_time(measure([&] {
			sink += soml::write(document)->size();
		})));

#if defined(SOML_BENCH_COMPARE)
		const std::string json_text = read_file(std::format("bench/fixtures/{}.json", fixture));
		const std::string toml_text = read_file(std::format("bench/fixtures/{}.toml", fixture));
		const nlohmann::json json = nlohmann::json::parse(json_text);
		const toml::table table = toml::parse(toml_text);

		read.cells.push_back(format_time(measure([&] {
			sink += nlohmann::json::parse(json_text).size();
		})));

		// The parser is reused, as simdjson recommends, and the text is padded once, outside the measurement.
		simdjson::dom::parser simdjson_parser;
		const simdjson::padded_string padded{json_text};

		if (simdjson_parser.parse(padded).error() != simdjson::SUCCESS) {
			std::println(stderr, "simdjson cannot read {}", fixture);
			return 1;
		}

		read.cells.push_back(format_time(measure([&] {
			sink += simdjson_parser.parse(padded).error() == simdjson::SUCCESS ? 1 : 0;
		})));

		read.cells.push_back(format_time(measure([&] {
			sink += toml::parse(toml_text).size();
		})));

		write.cells.push_back(format_time(measure([&] {
			sink += json.dump(1, '\t').size();
		})));

		// simdjson's document is immutable and has no writer that indents.
		write.cells.emplace_back("–");

		write.cells.push_back(format_time(measure([&] {
			std::ostringstream stream;
			stream << table;
			sink += stream.view().size();
		})));
#endif

		rows.push_back(std::move(read));
		rows.push_back(std::move(write));
	}

	std::print("| | soml.hpp, SOML |");

#if defined(SOML_BENCH_COMPARE)
	std::print(" nlohmann/json, JSON | simdjson, JSON | toml++, TOML |");
#endif

	std::println("");
	std::print("|---|---:|");

#if defined(SOML_BENCH_COMPARE)
	std::print("---:|---:|---:|");
#endif

	std::println("");

	for (const row& item : rows) {
		std::print("| {} |", item.name);

		for (const std::string& cell : item.cells) {
			std::print(" {} |", cell);
		}

		std::println("");
	}

	std::println("\nSOML-only conversion of complete structs (explicit field mappings, owning strings and vectors):");
	std::println("| Fixture | Parse and decode | Convert pre-parsed value | Struct to value |");
	std::println("|---|---:|---:|---:|");
	benchmark_struct<service>("service", [](const service& configuration) {
		return configuration.name.size() + configuration.server.tls.ciphers.size() + static_cast<std::size_t>(configuration.server.port) + static_cast<std::size_t>(configuration.queue.retry.initial_delay.count());
	}, sink);
	benchmark_struct<earthquakes>("earthquakes", [](const earthquakes& configuration) {
		std::size_t total = configuration.metadata.title.size();

		for (const auto& feature : configuration.features) {
			total += feature.id.size() + feature.properties.title.size() + feature.geometry.coordinates.size();
		}

		return total;
	}, sink);

	std::println("\nSOML-only edits replacing one item with a complete fixture:");
	std::println("| Fixture | Inline parent | Multiline parent |");
	std::println("|---|---:|---:|");
	const std::array<soml::path_segment, 2> path{std::string{"target"}, std::size_t{0}};
	for (const std::string_view fixture : fixtures) {
		const std::optional<soml::value> replacement{soml::parse(read_file(std::format("bench/fixtures/{}.soml", fixture))).value()};
		const soml::value expected = soml::object{{"target", soml::array{*replacement, 1}}};
		std::print("| {} |", fixture);
		for (const std::string_view source : {"target: [0, 1]", "target: [\n\t0\n\t1\n]"}) {
			const auto edited = soml::edit(source, path, replacement);
			if (!edited || soml::parse(*edited) != std::expected<soml::value, soml::error>{expected}) {
				std::println(stderr, "Edit fixture validation failed: {}", fixture);
				return 1;
			}
			std::print(" {} |", format_time(measure([&] {
				sink += soml::edit(source, path, replacement).value().size();
			})));
		}
		std::println("");
	}

	std::println("\nSOML-only integer, whole-float, and object workloads:");
	std::println("| Operation | Time |");
	std::println("|---|---:|");
	soml::array integers;
	soml::array whole_floats;
	std::vector<soml::member> repeated_members;
	for (std::int64_t index = 0; index < 4096; index++) {
		integers.push_back(index % 2 == 0 ? std::numeric_limits<std::int64_t>::min() + index : index);
		whole_floats.push_back(1'000'000'000'000'000.0 + static_cast<double>(index * 1000));
		repeated_members.push_back({std::to_string(index % 128), index});
	}
	const soml::value integer_document = std::move(integers);
	const soml::value float_document = std::move(whole_floats);
	const soml::object ordered{repeated_members};
	const soml::object equal = ordered;
	if (ordered != equal || ordered.size() != 128) {
		std::println(stderr, "Object workload validation failed");
		return 1;
	}
	for (std::int64_t index = 0; index < 128; index++) {
		const auto* item = ordered.find(std::to_string(index));
		if (item == nullptr || item->as<std::int64_t>() != 3968 + index) {
			std::println(stderr, "Object workload value validation failed: {}", index);
			return 1;
		}
	}
	if (soml::detail::edit_distance("max-connections", "max-connecitons") != 1) {
		std::println(stderr, "Key distance workload validation failed");
		return 1;
	}
	for (const auto& [name, document] : {std::pair{"Write 4096 integers", &integer_document}, std::pair{"Write 4096 whole floats", &float_document}}) {
		const auto written = soml::write(*document);
		if (!written || soml::parse(*written) != *document) {
			std::println(stderr, "Numeric workload validation failed: {}", name);
			return 1;
		}
		std::println("| {} | {} |", name, format_time(measure([&] {
			sink += soml::write(*document).value().size();
		})));
	}
	std::println("| Compact 4096 members to 128 | {} |", format_time(measure([&] {
		sink += soml::object{repeated_members}.size();
	})));
	std::println("| Compare 128 ordered members | {} |", format_time(measure([&] {
		sink += ordered == equal ? 1 : 0;
	})));
	std::vector<soml::member> reordered_members{ordered.begin(), ordered.end()};
	std::swap(reordered_members[126], reordered_members[127]);
	const soml::object reordered{std::move(reordered_members)};
	if (ordered != reordered) {
		std::println(stderr, "Reordered object workload validation failed");
		return 1;
	}
	std::println("| Compare 126 matching members and a reordered suffix | {} |", format_time(measure([&] {
		sink += ordered == reordered ? 1 : 0;
	})));
	std::println("| Key distance with a transposition | {} |", format_time(measure([&] {
		sink += soml::detail::edit_distance("max-connections", "max-connecitons");
	})));

	std::println("\nSOML-only validation and diagnostic workloads:");
	std::println("| Operation | Time |");
	std::println("|---|---:|");
	const std::string escaped = "'" + std::string(1'000'000, 'a');
	const std::string long_line(1'000'000, 'a');
	const soml::error first_character{.message = {}, .offset = 0, .line = 1, .column = 1, .path = {}};
	if (soml::detail::classify_string(escaped) != soml::detail::string_kind::escaped || first_character.code_frame(long_line) != "> 1 | " + std::string(100, 'a') + "…\n    | ^") {
		std::println(stderr, "Validation and diagnostic workload validation failed");
		return 1;
	}
	std::println("| Classify 1 MB after an early quote | {} |", format_time(measure([&] {
		sink += static_cast<std::size_t>(soml::detail::classify_string(escaped));
	})));
	std::println("| Frame the start of a 1 MB line | {} |", format_time(measure([&] {
		sink += first_character.code_frame(long_line).size();
	})));

	return sink == 0 ? 1 : 0;
}
