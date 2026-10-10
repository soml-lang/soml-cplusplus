/*
Canonical ordering is checked against independently sorted members; a preallocated writer exposes unnecessary scratch allocations without counting output growth.
*/
#include <soml.hpp>

#include <cstdlib>
#include <map>
#include <new>
#include <print>

namespace {

std::size_t allocation_count = 0;

struct configuration {
	int port = 80;
};

enum class severity { info };

void* allocate(std::size_t size) {
	allocation_count++;
	if (void* result = std::malloc(std::max(size, 1UZ))) {
		return result;
	}
	throw std::bad_alloc{};
}

} // namespace

template<>
inline constexpr auto soml::fields<configuration> = std::tuple{soml::field{"port", &configuration::port}};

template<>
inline constexpr auto soml::enum_names<severity> = std::array{std::pair{severity::info, std::string_view{"info"}}};

void* operator new(std::size_t size) {
	return allocate(size);
}

void* operator new[](std::size_t size) {
	return allocate(size);
}

void operator delete(void* pointer) noexcept {
	std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
	std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
	std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
	std::free(pointer);
}

int main() {
	int failures = 0;
	const auto check = [&](bool condition, std::string_view message) {
		if (!condition) {
			std::println(stderr, "{}", message);
			failures++;
		}
	};
	for (const char32_t scalar : {U'\u0080', U'\u0085', U'\u009B', U'\u009F', U'\u2028', U'\u2029'}) {
		std::string control;
		soml::detail::append_utf8(control, scalar);
		const std::string payload = "prefix" + control + "[INFO] forged entry";
		const auto unknown_key = soml::decode<configuration>("'" + payload + "': 1");
		check(!unknown_key && unknown_key.error().message.contains(control) && unknown_key.error().path.contains(control) && !unknown_key.error().description().contains(control), "unknown-key descriptions must neutralize Unicode controls and line separators while preserving original diagnostics");
		const auto unknown_enum = soml::decode<std::vector<severity>>("['" + payload + "']");
		check(!unknown_enum && unknown_enum.error().message.contains(control) && !unknown_enum.error().description().contains(control), "unknown-enum descriptions must neutralize Unicode controls and line separators from parsed strings");
		const std::string invalid = "value: ['" + payload + "', 'not an int']";
		const auto mismatched = soml::decode<std::map<std::string, std::vector<int>>>(invalid);
		const auto code_frame = mismatched.error().code_frame(invalid);
		check(!code_frame.contains(control) && std::ranges::count(code_frame, '\n') == 1, "code frames must neutralize source-derived Unicode line separators without introducing extra diagnostic lines");
	}
	for (const std::string_view ordinary : {"\u00A0", "\u00BF", "\u2027", "\u202F", "\u2065", "\u206A", "العربية", "עברית", "😀"}) {
		const soml::error failure{.message = std::string{ordinary}, .path = std::string{ordinary}};
		check(failure.description() == std::string{ordinary} + ": " + std::string{ordinary}, "diagnostic sanitization must preserve neighboring code points and ordinary Unicode scripts");
	}
	const soml::error diagnostic{.message = std::string(120, 'm'), .offset = 0, .line = 123456, .column = 789012, .path = "servers[42].port"};
	const std::size_t before_description = allocation_count;
	const auto description = diagnostic.description();
	const std::size_t description_allocations = allocation_count - before_description;
	check(description == "servers[42].port: " + diagnostic.message + " at line 123456, column 789012" && description_allocations == 1, "diagnostic descriptions must reserve their complete common output before appending");
	const soml::error middle{.message = {}, .offset = 500, .line = 1, .column = 501, .path = {}};
	const std::string long_line(1000, 'x');
	const std::size_t before_frame = allocation_count;
	const auto frame = middle.code_frame(long_line);
	const std::size_t frame_allocations = allocation_count - before_frame;
	check(frame == "> 1 | …" + std::string(100, 'x') + "…\n    | " + std::string(51, ' ') + '^' && frame_allocations <= 4, "long ASCII code frames must reserve their bounded line and caret buffers");

	for (const std::size_t count : {2UZ, 16UZ, 17UZ, 128UZ}) {
		soml::object ordered;
		std::string expected;
		for (std::size_t index = 0; index < count; index++) {
			const auto key = std::format("k{:03}", index);
			ordered.insert_or_assign(key, static_cast<std::int64_t>(index));
			expected += key + ": " + std::to_string(index) + '\n';
		}
		for (const bool canonical : {false, true}) {
			std::string output;
			output.reserve(expected.size());
			soml::detail::writer serializer{output, canonical};
			const std::size_t before = allocation_count;
			const bool succeeded = serializer.write_document(ordered);
			const std::size_t allocations = allocation_count - before;
			check(succeeded && output == expected && allocations == 0, "ordered object writing must validate uniqueness without scratch allocations");
		}
		std::vector<soml::member> batch{ordered.begin(), ordered.end()};
		const auto* storage = batch.data();
		const std::size_t before_batch = allocation_count;
		const soml::object moved_batch{std::move(batch)};
		const std::size_t batch_allocations = allocation_count - before_batch;
		check(moved_batch == ordered && moved_batch.members().data() == storage && batch_allocations == 0, "moving an ordered member batch must retain its allocation without sorting scratch storage");
		std::ranges::reverse(ordered);
		check(soml::write_canonical(ordered) == expected, "unordered objects must retain canonical order");
		check(ordered == moved_batch && moved_batch == ordered, "large reordered objects must compare equal with either side already sorted");
		auto changed = ordered;
		changed.begin()->value = -1;
		check(changed != moved_batch && moved_batch != changed, "large reordered comparisons must still check the value belonging to each key");
		changed.begin()->value = ordered.begin()->value;
		changed.begin()->key = "different";
		check(changed != moved_batch && moved_batch != changed, "large reordered comparisons must detect replacement keys");
		for (std::size_t duplicate = 1; duplicate < count; duplicate++) {
			auto invalid = ordered;
			(invalid.begin() + static_cast<std::ptrdiff_t>(duplicate))->key = invalid.begin()->key;
			for (const bool canonical : {false, true}) {
				const auto written = canonical ? soml::write_canonical(invalid) : soml::write(invalid);
				check(!written && written.error().code == soml::error_code::invalid_value, "both layouts must reject adjacent and separated duplicate keys after mutation");
			}
		}
	}

	std::array<int, 6> permutation{0, 1, 2, 3, 4, 5};
	const std::array<std::string, 6> keys{"", "a", "a-b", "a.b", "é", std::string{"a\0b", 3}};
	std::vector<soml::member> sorted;
	for (const auto& key : keys) {
		sorted.emplace_back(key, 1);
	}
	std::ranges::sort(sorted, {}, &soml::member::key);
	const auto canonical = soml::write_canonical(soml::object{sorted});
	do {
		soml::object members;
		for (const int index : permutation) {
			members.insert_or_assign(keys[static_cast<std::size_t>(index)], 1);
		}
		check(soml::write_canonical(members) == canonical && soml::parse(*canonical) == soml::value{members}, "every key permutation must serialize to the same canonical document and round-trip");
	} while (std::ranges::next_permutation(permutation).found);

	std::string source;
	for (int index = 0; index < 64; index++) {
		source += "'quoted member " + std::to_string(index) + std::string(30, 'x') + "': 1\n";
	}
	source += "target: 1\n";
	const auto document = soml::parse(source).value();
	const auto tree = soml::detail::scan_source(source, document);
	const std::size_t before = allocation_count;
	const auto* found = tree.root.find(document, "target");
	const std::size_t allocations = allocation_count - before;
	check(found == &tree.root.children.back() && allocations == 0, "bare diagnostic paths must skip unrelated quoted keys without allocating");
	check(tree.root.find(document, "[\"quoted member 1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"]") == &tree.root.children[1], "quoted diagnostic paths must still resolve exact quoted keys");
	for (const std::size_t indentation : {0UZ, 1UZ, 100'000UZ}) {
		const std::string prefix(indentation, ' ');
		const std::string blank(indentation + 3, ' ');
		const std::string block = "'''\n" + prefix + "first\n\n " + '\n' + blank + '\n' + prefix + "  last\n" + prefix + "'''";
		std::string output;
		soml::detail::append_reindented_block_string(output, block, 2);
		check(output == "'''\n\t\tfirst\n\n \n" + blank + "\n\t\t  last\n\t\t'''", "reindentation must skip known indentation while retaining short and long blank content lines");
		check(soml::parse("value: " + block) == soml::parse("value: " + output), "reindentation must preserve parsed block string content");
	}
	const std::string wide_blank = std::string(100'000, ' ') + "\t\n";
	const std::string removable = "[\n\t1\n" + wide_blank + wide_blank + "]";
	check(soml::edit(removable, {0UZ}, std::nullopt) == "[]", "removing a final item must collapse a container with long blank lines");
	const std::string retained = "[\n\t1\n" + wide_blank + wide_blank + "\t2\n]";
	const auto removed = soml::edit(retained, {0UZ}, std::nullopt);
	check(removed && soml::parse(*removed) == soml::value{soml::array{2}}, "blank-line scanning must stop before and preserve the next item's indentation");

	return failures == 0 ? 0 : 1;
}
