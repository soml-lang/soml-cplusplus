/*
Allocation failure: the global `operator new` fails at the Nth allocation, for every N, while a document is read, written, and decoded. Each run must succeed or throw `std::bad_alloc`, never crash, and free everything it allocated. This is its own program, because it replaces `operator new` for the whole program.
*/
#include <soml.hpp>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <new>
#include <print>
#include <unordered_map>

namespace {

std::size_t allocation_count = 0;
std::size_t allocated_bytes = 0;
std::size_t live_allocations = 0;
std::size_t failing_allocation = std::numeric_limits<std::size_t>::max();

void* allocate(std::size_t size) {
	if (allocation_count++ == failing_allocation) {
		throw std::bad_alloc{};
	}

	void* pointer = std::malloc(size == 0 ? 1 : size);

	if (pointer == nullptr) {
		throw std::bad_alloc{};
	}

	live_allocations++;
	allocated_bytes += size;
	return pointer;
}

void release(void* pointer) noexcept {
	if (pointer != nullptr) {
		live_allocations--;
		std::free(pointer);
	}
}

struct server {
	std::string host;
	std::vector<int> ports;
	std::map<std::string, std::chrono::seconds> timeouts;
	std::optional<std::string> owner;
};

enum class selection { first };

} // namespace

void* operator new(std::size_t size) {
	return allocate(size);
}

void* operator new[](std::size_t size) {
	return allocate(size);
}

void operator delete(void* pointer) noexcept {
	release(pointer);
}

void operator delete[](void* pointer) noexcept {
	release(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
	release(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
	release(pointer);
}

template<>
inline constexpr auto soml::fields<server> = std::tuple{
	soml::field{"host", &server::host},
	soml::field{"ports", &server::ports},
	soml::field{"timeouts", &server::timeouts},
	soml::field{"owner", &server::owner},
};

template<>
inline constexpr auto soml::enum_names<selection> = std::array{std::pair{selection::first, std::string_view{"first"}}};

int main() {
	std::string text;

	{
		std::ifstream file{"tests/fixtures/example.soml", std::ios::binary};
		text.assign(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
	}

	// An invalid document too, whose diagnosis allocates messages and suggestions.
	const std::string invalid = "a: 1\nb: [1,000]\n";
	const std::string typed = "host: 'a long host name that needs its own allocation'\nports: [80, 443]\ntimeouts: {read: 30s, write: 1m}\nextra: 1";
	int failures = 0;

	for (const auto& source : std::array<soml::value, 5>{true, 0, "unknown selection", soml::duration{1000}, soml::instant{0, 1}}) {
		const std::size_t before_optional = allocation_count;
		const bool rejected = !source.as<selection>() && !source.as<std::chrono::seconds>() && !source.as<std::chrono::duration<std::uint8_t, std::nano>>() && !source.as<std::chrono::sys_seconds>() && !source.as<std::optional<selection>>() && !source.as<std::optional<std::chrono::seconds>>();
		if (!rejected || allocation_count != before_optional) {
			std::println(stderr, "failed optional enum and time conversions must not allocate discarded diagnostics");
			failures++;
		}
	}

	const std::string json_plain(8190, 'x');
	const std::size_t before_json_quote = allocation_count;
	const auto json_quoted = soml::detail::json_quote(json_plain);
	const std::size_t json_quote_allocations = allocation_count - before_json_quote;
	if (json_quoted != '\"' + json_plain + '\"' || json_quote_allocations != 1) {
		std::println(stderr, "quoting long ordinary JSON strings must allocate only their final buffer");
		failures++;
	}

	// Speculative key and root-value probes transfer saved diagnostics instead of copying them.
	const std::size_t before_speculative_error = allocation_count;
	const auto speculative_error = soml::parse("a:\n  25:00:00");
	if (speculative_error || speculative_error.error().message != "A time of day is a string, so it must be quoted" || speculative_error.error().offset != 5 || speculative_error.error().line != 2 || speculative_error.error().column != 3 || allocation_count - before_speculative_error > 2) {
		std::println(stderr, "speculative parsing must preserve diagnostics without copying them");
		failures++;
	}

	// Finishing a root container reuses the parser's storage instead of allocating another complete result vector.
	std::string root_array_source = "[";
	std::string root_object_source;
	for (int index = 0; index < 1024; index++) {
		root_array_source += index == 0 ? "1" : ",1";
		root_object_source += "k" + std::to_string(index) + ": 1\n";
	}
	root_array_source += ']';
	const std::size_t before_root_array = allocated_bytes;
	const auto root_array = soml::parse(root_array_source);
	if (!root_array || root_array->size() != 1024 || allocated_bytes - before_root_array > 2048 * sizeof(soml::value)) {
		std::println(stderr, "root arrays must reuse their parser storage");
		failures++;
	}
	const std::size_t before_root_object = allocated_bytes;
	const auto root_object = soml::parse(root_object_source);
	if (!root_object || root_object->size() != 1024 || allocated_bytes - before_root_object > 2048 * (sizeof(soml::member) + sizeof(std::size_t)) + 1024 * sizeof(std::size_t)) {
		std::println(stderr, "root objects must reuse their parser storage");
		failures++;
	}

	// Encoding an existing DOM should have the same allocation cost as writing it directly.
	soml::array native_items;
	for (int index = 0; index < 128; index++) {
		native_items.emplace_back(std::string(256, 'x'));
	}
	const soml::value native_document{std::move(native_items)};
	soml::object native_members;
	for (int index = 0; index < 128; index++) {
		native_members.insert_or_assign(std::to_string(index), std::string(256, 'x'));
	}
	const soml::value native_object_document{native_members};
	const auto check_native_writing = [&](const auto& native) {
		for (const bool canonical : {false, true}) {
			const std::size_t before_encode = allocation_count;
			const auto encoded = canonical ? soml::encode_canonical(native) : soml::encode(native);
			const std::size_t encode_allocations = allocation_count - before_encode;
			const std::size_t before_write = allocation_count;
			const auto written = canonical ? soml::write_canonical(native) : soml::write(native);
			if (written != encoded || allocation_count - before_write != encode_allocations) {
				std::println(stderr, "writing native collections must not allocate an intermediate copy");
				failures++;
			}
			std::string destination;
			const std::size_t before_append = allocation_count;
			const auto appended = canonical ? soml::write_canonical(native, destination) : soml::write(native, destination);
			if (!appended || destination != *encoded || allocation_count - before_append != encode_allocations) {
				std::println(stderr, "appending native collections must transfer the rendered storage without copying the collection");
				failures++;
			}
		}
	};
	check_native_writing(*native_document.get_if<soml::array>());
	check_native_writing(native_members);
	const std::string native_source = soml::write(native_document).value();
	const std::size_t before_native_parse = allocation_count;
	const auto native_parsed = soml::parse(native_source);
	const std::size_t native_parse_allocations = allocation_count - before_native_parse;
	const std::size_t before_native_decode = allocation_count;
	const auto native_decoded = soml::decode<soml::value>(native_source, {.ignore_unknown_keys = true});
	if (native_decoded != native_parsed || allocation_count - before_native_decode != native_parse_allocations) {
		std::println(stderr, "native DOM decoding must not allocate an intermediate copy");
		failures++;
	}
	const std::size_t before_native_array_decode = allocation_count;
	const auto native_array_decoded = soml::decode<soml::array>(native_source);
	if (!native_array_decoded || *native_array_decoded != *native_parsed->get_if<soml::array>() || allocation_count - before_native_array_decode != native_parse_allocations) {
		std::println(stderr, "native array decoding must transfer the parsed storage");
		failures++;
	}
	const std::optional<soml::value> large_replacement{native_document};
	const std::size_t before_invalid_scalar = allocated_bytes;
	const auto invalid_scalar_path = soml::edit("{}", {"missing", "nested", std::size_t{0}}, 1);
	const std::size_t invalid_scalar_bytes = allocated_bytes - before_invalid_scalar;
	const std::size_t before_invalid_large = allocated_bytes;
	const auto invalid_large_path = soml::edit("{}", {"missing", "nested", std::size_t{0}}, large_replacement);
	if (invalid_large_path != invalid_scalar_path || invalid_large_path || allocated_bytes - before_invalid_large != invalid_scalar_bytes) {
		std::println(stderr, "invalid missing-object paths must reject before copying replacements");
		failures++;
	}
	const std::size_t before_path_parse = allocated_bytes;
	static_cast<void>(soml::parse(root_array_source));
	const std::size_t path_parse_bytes = allocated_bytes - before_path_parse;
	const std::array<soml::path_segment, soml::max_depth + 1> excessive_path{};
	for (const auto path : {std::span<const soml::path_segment>{}, std::span<const soml::path_segment>{excessive_path}}) {
		const std::size_t before_invalid_path = allocated_bytes;
		const auto invalid_path = soml::edit(root_array_source, path, 1);
		if (invalid_path || allocated_bytes - before_invalid_path > path_parse_bytes + 1024) {
			std::println(stderr, "invalid edit paths must reject before allocating source layout");
			failures++;
		}
	}
	for (const bool canonical : {false, true}) {
		const std::size_t before_write = allocation_count;
		const auto written = canonical ? soml::write_canonical(native_document) : soml::write(native_document);
		const std::size_t write_allocations = allocation_count - before_write;
		const std::size_t before_encode = allocation_count;
		const auto encoded = canonical ? soml::encode_canonical<soml::value>(native_document) : soml::encode<soml::value>(native_document);
		if (encoded != written || allocation_count - before_encode != write_allocations) {
			std::println(stderr, "native DOM encoding must not allocate an intermediate copy");
			failures++;
		}
		const std::size_t before_array_encode = allocation_count;
		const auto array_encoded = canonical ? soml::encode_canonical(*native_document.get_if<soml::array>()) : soml::encode(*native_document.get_if<soml::array>());
		if (array_encoded != written || allocation_count - before_array_encode != write_allocations) {
			std::println(stderr, "native array encoding must borrow its storage");
			failures++;
		}
		const std::size_t before_object_write = allocation_count;
		const auto object_written = canonical ? soml::write_canonical(native_object_document) : soml::write(native_object_document);
		const std::size_t object_write_allocations = allocation_count - before_object_write;
		const std::size_t before_object_encode = allocation_count;
		const auto object_encoded = canonical ? soml::encode_canonical(native_members) : soml::encode(native_members);
		if (object_encoded != object_written || allocation_count - before_object_encode != object_write_allocations) {
			std::println(stderr, "native object encoding must borrow its storage");
			failures++;
		}
		std::string destination;
		const std::size_t before_append = allocation_count;
		const auto appended = canonical ? soml::write_canonical(native_document, destination) : soml::write(native_document, destination);
		if (!appended || destination != *written || allocation_count - before_append != write_allocations) {
			std::println(stderr, "writing to an empty destination must reuse the rendered buffer");
			failures++;
		}
	}

	const soml::value container_mismatch{std::string(1000, 'x')};
	const soml::value float_out_of_range{std::numeric_limits<double>::max()};
	const std::size_t before_float_mismatches = allocation_count;
	if (container_mismatch.as<float>() || container_mismatch.as<long double>() || float_out_of_range.as<float>() || soml::value{1}.as<float>() || allocation_count != before_float_mismatches) {
		std::println(stderr, "checked float extraction must not allocate discarded diagnostics");
		failures++;
	}
	const std::array<soml::value, 3> integer_mismatches{container_mismatch, std::numeric_limits<std::int64_t>::max(), -1};
	const std::size_t before_integer_mismatches = allocation_count;
	if (integer_mismatches[0].as<int>() || integer_mismatches[1].as<int>() || integer_mismatches[2].as<unsigned int>() || allocation_count != before_integer_mismatches) {
		std::println(stderr, "checked integer extraction must not allocate discarded diagnostics");
		failures++;
	}
	const std::size_t before_container_mismatch = allocation_count;
	if (container_mismatch.as<soml::array>() || container_mismatch.as<soml::object>() || allocation_count != before_container_mismatch) {
		std::println(stderr, "mismatched container views must not allocate discarded diagnostics");
		failures++;
	}

	// Canonical singleton objects have no ordering work and need no scratch allocation.
	const soml::value singleton = soml::object{{"a", 1}};
	const std::size_t before_singleton = allocation_count;
	const auto canonical_singleton = soml::write_canonical(singleton);
	if (canonical_singleton != "a: 1\n" || allocation_count != before_singleton) {
		std::println(stderr, "canonical singleton writing must not allocate");
		failures++;
	}

	// Moving a small unique batch must reuse its storage, even when key fingerprints collide.
	for (const std::size_t size : {0UZ, 1UZ, 16UZ}) {
		std::vector<soml::member> members;
		for (std::size_t index = 0; index < size; index++) {
			members.push_back({std::format("k{:02}z", index), static_cast<std::int64_t>(index)});
		}
		const std::size_t before = allocation_count;
		const soml::object result{std::move(members)};
		if (allocation_count != before || result.size() != size) {
			std::println(stderr, "moving {} unique members allocated or lost members", size);
			failures++;
		}
	}

	std::map<std::string, int> source_map;
	for (int index = 0; index < 128; index++) {
		source_map.emplace(std::to_string(index), index);
	}
	const std::size_t before_map = allocation_count;
	const auto encoded_map = soml::to_value(source_map);
	if (!encoded_map || encoded_map->size() != source_map.size() || allocation_count != before_map + 1) {
		std::println(stderr, "encoding a sized map must allocate member storage once");
		failures++;
	}

	// Duplicate batches need scratch indexes, but must keep the input member allocation when compacting.
	for (const std::size_t size : {16UZ, 128UZ}) {
		std::vector<soml::member> members;
		for (std::size_t index = 0; index < size; index++) {
			members.push_back({std::to_string(index % 4), static_cast<std::int64_t>(index)});
		}
		const auto* storage = members.data();
		const std::size_t before = allocation_count;
		const soml::object compacted{std::move(members)};
		if (compacted.members().data() != storage || allocation_count != before + 1 || compacted.size() != 4 || compacted.find("0")->as<std::int64_t>() != static_cast<std::int64_t>(size - 4)) {
			std::println(stderr, "compacting a {}-member batch must reuse member storage and keep its final values", size);
			failures++;
		}
	}

	const soml::object ordered = *encoded_map->get_if<soml::object>();
	auto changed = ordered;
	changed.begin()->value = -1;
	const std::size_t before_equality = allocation_count;
	if (ordered != *encoded_map->get_if<soml::object>() || ordered == changed || allocation_count != before_equality) {
		std::println(stderr, "comparing large objects with matching key order must not allocate");
		failures++;
	}
	auto reordered = ordered;
	std::ranges::reverse(reordered);
	const std::size_t before_reordered_equality = allocation_count;
	if (ordered != reordered || allocation_count != before_reordered_equality + 1) {
		std::println(stderr, "comparing reordered large objects must allocate scratch storage once");
		failures++;
	}

	// Locating a conversion failure must not copy each unrelated bare key into a temporary path.
	std::string lookup_source;
	std::string lookup_key;
	for (int index = 0; index < 64; index++) {
		lookup_key = "server-name-" + std::to_string(index) + std::string(30, 'x');
		lookup_source += lookup_key + ": 1\n";
	}
	const auto lookup_document = soml::parse(lookup_source).value();
	const auto lookup_tree = soml::detail::scan_source(lookup_source, lookup_document);
	const std::size_t before_lookup = allocation_count;
	if (lookup_tree.root.find(lookup_document, lookup_key) != &lookup_tree.root.children.back() || lookup_tree.root.find(lookup_document, "missing-key") != nullptr || allocation_count != before_lookup) {
		std::println(stderr, "locating bare-key paths must not allocate temporary key copies");
		failures++;
	}

	const std::size_t before_distance = allocation_count;
	if (soml::detail::edit_distance("port", "prot") != 1 || allocation_count != before_distance + 1) {
		std::println(stderr, "edit distance must allocate its three rows together");
		failures++;
	}
	const std::size_t before_trivial_distance = allocation_count;
	if (soml::detail::edit_distance("port", "port") != 0 || soml::detail::edit_distance("", "port") != 4 || soml::detail::edit_distance("port", "") != 4 || allocation_count != before_trivial_distance) {
		std::println(stderr, "equal and empty key distances must not allocate");
		failures++;
	}

	const std::string part(1000, 'x');
	// Huge malformed tokens must only allocate their bounded diagnostic, with no token-sized scratch copy.
	const std::string long_unit(100'000, 'm');
	const std::size_t before_unit = allocation_count;
	const auto unit_message = soml::detail::describe_unit(long_unit, {});
	if (allocation_count != before_unit + 1 || unit_message.size() >= 200) {
		std::println(stderr, "unknown duration units must only allocate their bounded message");
		failures++;
	}
	const std::string long_hex = "0x" + std::string(100'000, '0') + "a";
	const std::size_t before_hex = allocation_count;
	const auto hex_message = soml::detail::describe_bad_number(long_hex, long_hex);
	if (allocation_count != before_hex + 1 || hex_message != "Hexadecimal digits are uppercase") {
		std::println(stderr, "huge lowercase radix literals must only allocate their bounded message");
		failures++;
	}

	// Obvious float range failures and zero bypass underscore stripping, even with enormous mantissas and exponents.
	for (const bool is_zero : {false, true}) {
		std::string mantissa = is_zero ? "0.0" : "1.1";
		for (int index = 0; index < 50'000; index++) {
			mantissa += is_zero ? "_0" : "_1";
		}
		for (const bool is_negative : {false, true}) {
			for (const bool is_exponent_negative : {false, true}) {
				const std::string literal = (is_negative ? "-" : "") + mantissa + (is_exponent_negative ? "e-999_999_999_999_999" : "e999_999_999_999_999");
				const std::size_t before_float = allocation_count;
				double number = 1;
				const auto status = soml::detail::parse_float(literal, number);
				auto expected = soml::detail::float_status::ok;
				if (!is_zero) {
					expected = is_exponent_negative ? soml::detail::float_status::too_small : soml::detail::float_status::too_large;
				}
				if (soml::detail::classify_number(literal) != soml::detail::number_kind::floating || status != expected || allocation_count != before_float || (is_zero && std::bit_cast<std::uint64_t>(number) != 0)) {
					std::println(stderr, "extreme underscore-separated floats must classify their range without allocating");
					failures++;
				}
			}
		}
	}

	const std::string long_octal = "0" + std::string(100'000, '7');
	const std::size_t before_octal_bytes = allocated_bytes;
	const auto octal_message = soml::detail::describe_bad_number(long_octal, long_octal);
	if (allocated_bytes - before_octal_bytes > 1024 || octal_message.size() >= 200) {
		std::println(stderr, "leading-zero diagnoses must not copy huge octal suggestions");
		failures++;
	}

	std::string thousands = "[1,000";
	for (int index = 0; index < 25'000; index++) {
		thousands += ",111";
	}
	thousands += "]";
	const std::size_t before_thousands_bytes = allocated_bytes;
	const auto thousands_result = soml::parse(thousands);
	if (thousands_result || allocated_bytes - before_thousands_bytes > 1024 || thousands_result.error().message.size() >= 200) {
		std::println(stderr, "thousands-separator diagnoses must not copy huge corrected numbers");
		failures++;
	}

	const std::string wide_indentation(100'000, ' ');
	const std::string indented_block = "value: '''\n" + wide_indentation + "x\n" + wide_indentation + "'''";
	const std::size_t before_block_bytes = allocated_bytes;
	const auto block_result = soml::parse(indented_block);
	if (!block_result || (*block_result)["value"].as<std::string_view>() != "x" || allocated_bytes - before_block_bytes > 4096) {
		std::println(stderr, "block strings must not reserve storage for stripped indentation");
		failures++;
	}

	const std::string long_fraction = "1." + std::string(100'000, '0');
	for (const std::string& spaced : {"[" + long_fraction + " h]", "[1 " + long_unit + "]"}) {
		const std::size_t before_spaced_bytes = allocated_bytes;
		const auto result = soml::parse(spaced);
		if (result || allocated_bytes - before_spaced_bytes > 2048 || result.error().message.size() >= 300) {
			std::println(stderr, "spaced-unit diagnoses must not copy huge numeric or unit tokens");
			failures++;
		}
	}
	const std::string spaced_instant = "[2026-09-19 14:00:00." + std::string(100'000, '0') + "Z]";
	const std::size_t before_instant_bytes = allocated_bytes;
	const auto instant_result = soml::parse(spaced_instant);
	if (instant_result || allocated_bytes - before_instant_bytes > 1024 || instant_result.error().message.size() >= 300) {
		std::println(stderr, "spaced-instant diagnoses must not copy huge time tokens");
		failures++;
	}
	const std::string microseconds = "[" + long_fraction + "µs]";
	const std::size_t before_microseconds_bytes = allocated_bytes;
	const auto microseconds_result = soml::parse(microseconds);
	if (microseconds_result || allocated_bytes - before_microseconds_bytes > 1024 || microseconds_result.error().message.size() >= 200) {
		std::println(stderr, "microsecond diagnoses must not copy huge numeric tokens");
		failures++;
	}

	for (const bool huge_parent : {false, true}) {
		const std::string parent = huge_parent ? std::string(100'000, 'a') : "a";
		const std::string child = huge_parent ? "b" : std::string(100'000, 'b');
		const std::string missing_value = parent + ":\n  " + child + ": 1";
		const std::size_t before_key_bytes = allocated_bytes;
		const auto key_result = soml::parse(missing_value);
		// Parsing the keys owns their text once for the parent and once for the child during the diagnosis.
		if (key_result || allocated_bytes - before_key_bytes > parent.size() + child.size() + 4096 || key_result.error().message.size() >= 400) {
			std::println(stderr, "missing-value diagnoses must not format huge parent or child keys");
			failures++;
		}
	}

	const std::string delimiter(100'000, '\'');
	const std::string unterminated_block = "[" + delimiter + "\nx" + delimiter;
	const std::size_t before_delimiter_bytes = allocated_bytes;
	const auto delimiter_result = soml::parse(unterminated_block);
	if (delimiter_result || allocated_bytes - before_delimiter_bytes > 1024 || delimiter_result.error().message.size() >= 300) {
		std::println(stderr, "unterminated-block diagnoses must not copy huge closing delimiters");
		failures++;
	}

	const std::size_t before_concat = allocation_count;
	const auto joined = soml::detail::concat(part, part, part, part);
	if (joined.size() != 4000 || allocation_count != before_concat + 1) {
		std::println(stderr, "concatenation must reserve the complete output once");
		failures++;
	}
	const std::string indented_source = std::string(100'000, ' ') + "a: 1";
	const std::size_t before_indentation_bytes = allocated_bytes;
	const auto indented_edit = soml::edit(indented_source, {"a"}, 2);
	const std::size_t indentation_bytes = allocated_bytes - before_indentation_bytes;
	if (!indented_edit || indented_edit->size() != indented_source.size() || !indented_edit->starts_with(std::string_view{indented_source}.substr(0, 100'000)) || !indented_edit->ends_with("a: 2") || indentation_bytes > indented_source.size() + 2048) {
		std::println(stderr, "editing a value must borrow its indentation and preserve every source space");
		failures++;
	}
	const std::size_t before_indented_insert = allocated_bytes;
	const auto indented_insert = soml::edit(indented_source, {"b"}, 2);
	const std::size_t insertion_bytes = allocated_bytes - before_indented_insert;
	if (!indented_insert || !indented_insert->ends_with("b: 2") || insertion_bytes > indented_source.size() + indented_insert->size() + 2048) {
		std::println(stderr, "inserting a member must borrow the existing indentation");
		failures++;
	}

	const soml::value empty_object = soml::object{};
	const std::size_t before_empty_map = allocation_count;
	const auto empty_map = soml::from_value<std::unordered_map<std::string, int>>(empty_object);
	if (!empty_map || !empty_map->empty() || allocation_count != before_empty_map) {
		std::println(stderr, "decoding an empty hash map must not allocate buckets");
		failures++;
	}

	// Source locations must add no allocations to successful decoding.
	const std::size_t before_conversion = allocation_count;
	{
		auto parsed = soml::parse(typed);
		const auto converted = soml::from_value<server>(std::move(*parsed), {.ignore_unknown_keys = true});
		if (!converted || converted->ports.size() != 2) {
			std::println(stderr, "parse and conversion failed");
			failures++;
		}
	}
	const std::size_t conversion_allocations = allocation_count - before_conversion;
	const std::size_t before_decode = allocation_count;
	const auto decoded = soml::decode<server>(typed, {.ignore_unknown_keys = true});
	if (!decoded || decoded->ports.size() != 2 || allocation_count - before_decode != conversion_allocations) {
		std::println(stderr, "successful decoding must allocate exactly as much as parse and conversion");
		failures++;
	}

	const auto run = [&](const char* name, auto&& operation) {
		const std::size_t live_before = live_allocations;
		std::size_t total = 0;
		bool has_completed = false;

		for (std::size_t failing = 0; !has_completed; failing++) {
			allocation_count = 0;
			failing_allocation = failing;

			try {
				operation();
				has_completed = true;
				total = failing;
			} catch (const std::bad_alloc&) {
				// Expected while an allocation fails.
			}

			failing_allocation = std::numeric_limits<std::size_t>::max();

			if (live_allocations != live_before) {
				std::println(stderr, "{}: {} allocations leaked when allocation {} failed", name, live_allocations - live_before, failing);
				failures++;
				return;
			}
		}

		std::println("✔ {}: every one of {} allocations failed in turn", name, total);
	};

	run("read", [&] {
		static_cast<void>(soml::parse(text));
	});

	run("read an invalid document", [&] {
		static_cast<void>(soml::parse(invalid));
	});

	const soml::value document = *soml::parse(text);

	run("write", [&] {
		static_cast<void>(soml::write(document));
	});

	run("write canonical form", [&] {
		static_cast<void>(soml::write_canonical(document));
	});

	// Failed allocations while rendering or appending must preserve the caller's string.
	for (const bool canonical : {false, true}) {
		std::string output = "# existing content that exceeds the small-string buffer\n";
		const std::string original = output;
		const std::string expected = original + (canonical ? soml::write_canonical(document).value() : soml::write(document).value());

		run(canonical ? "append canonical" : "append", [&] {
			try {
				const auto result = canonical ? soml::write_canonical(document, output) : soml::write(document, output);

				if (!result || output != expected) {
					std::abort();
				}
			} catch (const std::bad_alloc&) {
				if (output != original) {
					std::abort();
				}

				throw;
			}

		});
		run(canonical ? "empty canonical destination" : "empty destination", [&] {
			std::string destination;
			try {
				const auto result = canonical ? soml::write_canonical(document, destination) : soml::write(document, destination);
				if (!result || destination != std::string_view{expected}.substr(original.size())) {
					std::abort();
				}
			} catch (const std::bad_alloc&) {
				if (!destination.empty()) {
					std::abort();
				}
				throw;
			}
		});
	}

	run("bulk object", [&] {
		static_cast<void>(soml::object{std::vector<soml::member>{{"a long member key", 1}, {"another long member key", 2}, {"a long member key", 3}}});
	});

	run("compacting a large duplicate batch", [&] {
		std::vector<soml::member> members;
		for (int index = 0; index < 100; index++) {
			members.push_back({std::to_string(index % 4), soml::array{index, "a long nested value that owns an allocation"}});
		}
		const soml::object compacted{std::move(members)};
		if (compacted.size() != 4 || compacted.find("0")->operator[](std::size_t{0}).as<int>() != 96) {
			std::abort();
		}
	});

	run("key suggestions", [&] {
		static_cast<void>(soml::decode<server>("hots: 'localhost'"));
	});

	run("decode", [&] {
		static_cast<void>(soml::decode<server>(typed, {.ignore_unknown_keys = true}));
		static_cast<void>(soml::decode<server>(typed));
		static_cast<void>(soml::decode<server>("ports: [80, 'bad']\nowner: '''\n\tblock string\n'''"));
	});

	run("format", [&] {
		static_cast<void>(soml::format(text));
	});

	run("edit", [&] {
		static_cast<void>(soml::edit(typed, {"ports", std::size_t{0}}, 8080));
		static_cast<void>(soml::edit(typed, {"owner"}, "a new owner with a long name"));
		static_cast<void>(soml::edit(typed, {"ports"}, std::nullopt));
	});

	run("insert a nested replacement", [&] {
		const auto result = soml::edit("host: 'localhost'\n", {"new", "inner"}, document);
		if (!result || (*soml::parse(*result))["new"]["inner"]->size() != document.size()) {
			std::abort();
		}
	});

	run("encode", [&] {
		static_cast<void>(soml::encode(server{.host = "a long host name that needs its own allocation", .ports = {1, 2}, .timeouts = {{"read", std::chrono::seconds{1}}}, .owner = "someone"}));
	});

	run("encode a large map", [&] {
		static_cast<void>(soml::to_value(source_map));
	});

	const auto inline_replacement = soml::parse(text).value();
	run("edit an inline container", [&] {
		const auto result = soml::edit("values: [0, 1]", {"values", std::size_t{0}}, inline_replacement);
		if (!result || (*soml::parse(*result))["values"][1].as<int>() != 1) {
			std::abort();
		}
	});

	// Equal prefixes have already been checked, so a small reordered suffix needs no sorting storage.
	std::vector<soml::member> prefix_members;
	for (int index = 0; index < 128; index++) {
		prefix_members.push_back({std::to_string(index), index});
	}
	const soml::object prefix_left{prefix_members};
	std::swap(prefix_members[126], prefix_members[127]);
	const soml::object prefix_right{std::move(prefix_members)};
	const std::size_t before_prefix_equality = allocation_count;
	const bool prefix_equal = prefix_left == prefix_right;
	if (!prefix_equal || allocation_count != before_prefix_equality) {
		std::println(stderr, "A small reordered suffix should compare without allocating");
		failures++;
	}

	return failures == 0 ? 0 : 1;
}
