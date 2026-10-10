/*
libFuzzer harness. Every input must be read without a crash, and every document that is read must write in canonical form, read back to an equal value, and write again to the same bytes, in member order and in canonical form.
*/
#include <soml.hpp>

#include <cstdlib>
#include <deque>
#include <map>
#include <print>

namespace {

// A struct with a field of each kind, so that decoding any document exercises the converters. Unknown keys are skipped.
struct everything {
	bool flag = false;
	std::int8_t small = 0;
	std::uint64_t large = 0;
	float ratio = 0.0F;
	double number = 0.0;
	std::string name;
	std::optional<std::vector<std::int32_t>> numbers;
	std::array<std::string, 2> pair;
	std::deque<soml::value> items;
	std::map<std::string, std::chrono::milliseconds> timeouts;
	std::chrono::sys_seconds time{};
	std::chrono::sys_time<std::chrono::nanoseconds> precise{};
	std::chrono::duration<double> length{};
	soml::date date{std::chrono::year{2000}, std::chrono::month{1}, std::chrono::day{1}};
	soml::instant moment;
	soml::object rest;
};

} // namespace

template<>
inline constexpr auto soml::fields<everything> = std::tuple{
	soml::field{"a", &everything::flag},
	soml::field{"b", &everything::small},
	soml::field{"c", &everything::large},
	soml::field{"d", &everything::ratio},
	soml::field{"e", &everything::number},
	soml::field{"f", &everything::name},
	soml::field{"g", &everything::numbers},
	soml::field{"h", &everything::pair},
	soml::field{"i", &everything::items},
	soml::field{"j", &everything::timeouts},
	soml::field{"k", &everything::time},
	soml::field{"l", &everything::precise},
	soml::field{"m", &everything::length},
	soml::field{"n", &everything::moment},
	soml::field{"o", &everything::rest},
	soml::field{"p", &everything::date},
};

namespace {

void require(bool condition, std::string_view message) {
	if (!condition) {
		std::println(stderr, "{}", message);
		std::abort();
	}
}

void require_valid_utf8(std::string_view text) {
	for (std::size_t position = 0; position < text.size();) {
		const std::size_t length = soml::detail::utf8_sequence_length(text, position);
		require(length != 0, "A diagnostic has invalid UTF-8");
		position += length;
	}
}

/*
Traverses a path known to exist in the independently constructed expected value.
*/
soml::value& value_at(soml::value& root, std::span<const soml::path_segment> path) {
	soml::value* current = &root;
	for (const auto& segment : path) {
		if (const auto* key = std::get_if<std::string>(&segment)) {
			current = current->find(*key);
		} else {
			current = &(*current->get_if<soml::array>())[std::get<std::size_t>(segment)];
		}
	}
	return *current;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	const std::string text(reinterpret_cast<const char*>(data), size); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
	const bool ascii_representable = text.find('\r') == std::string::npos && std::ranges::all_of(text, [](unsigned char byte) {
		return byte < 0x80;
	});
	const std::array<soml::value, 2> constructed{
		soml::object{{"value", text}},
		soml::object{{text, 1}},
	};

	for (const soml::value& item : constructed) {
		const auto written = soml::write(item);
		require(!ascii_representable || written.has_value(), "Writing representable ASCII fails");

		if (written) {
			require(soml::parse(*written) == item, "Writing arbitrary string bytes changes the value");
		} else {
			require(written.error().code == soml::error_code::invalid_value, "Writing arbitrary string bytes fails for another reason");
			require_valid_utf8(written.error().description());
		}
	}

	const auto document = soml::parse(text);

	if (!document) {
		const soml::error& error = document.error();
		require(error.code == soml::error_code::syntax || error.code == soml::error_code::too_deep, "A reading error has another code");
		require(error.offset <= size && error.line >= 1 && error.column >= 1, "A reading error has no position");
		require_valid_utf8(error.description());
		require_valid_utf8(error.code_frame(text));
		return 0;
	}

	require(document->is_object() || document->is_array(), "A document is not a collection");

	for (const auto writer : std::array<std::expected<std::string, soml::error> (*)(const soml::value&), 2>{soml::write, soml::write_canonical}) {
		const auto written = writer(*document);
		require(written.has_value(), "A document that was read cannot be written");
		const auto again = soml::parse(*written);
		require(again.has_value(), "A written document cannot be read");
		require(*again == *document, "A written document reads back to another value");
		const auto rewritten = writer(*again);
		require(rewritten.has_value() && *rewritten == *written, "Writing again gives other bytes");
	}

	const auto formatted = soml::format(text);
	require(formatted.has_value(), "A valid document cannot be formatted");
	require(soml::parse(*formatted) == document, "Formatting changes the value");
	require(soml::format(*formatted) == formatted, "Formatting is not idempotent");

	// Replacing sampled top-level values exercises source spans, comments, and compact containers. Removing it must preserve every other member or item.
	std::vector<soml::path_segment> path;

	if (const auto* members = document->get_if<soml::object>()) {
		for (const auto& member : members->members().first(std::min<std::size_t>(members->size(), 8))) {
			path.emplace_back(member.key);
			const auto changed = soml::edit(text, path, nullptr);
			require(changed.has_value(), "Replacing an object member failed");
			auto expected = *document;
			*expected.find(member.key) = nullptr;
			require(soml::parse(*changed) == expected, "Replacing an object member changes another value");
			const auto removed = soml::edit(text, path, std::nullopt);
			require(removed.has_value(), "Removing an object member failed");
			expected.get_if<soml::object>()->erase(member.key);
			require(soml::parse(*removed) == expected, "Removing an object member changes another value");
			path.clear();
		}
	} else {
		const auto& items = *document->get_if<soml::array>();

		for (std::size_t index = 0; index < std::min<std::size_t>(items.size(), 8); index++) {
			path.emplace_back(index);
			const auto changed = soml::edit(text, path, nullptr);
			require(changed.has_value(), "Replacing an array item failed");
			auto expected = *document;
			(*expected.get_if<soml::array>())[index] = nullptr;
			require(soml::parse(*changed) == expected, "Replacing an array item changes another value");
			const auto removed = soml::edit(text, path, std::nullopt);
			require(removed.has_value(), "Removing an array item failed");
			auto& remaining = *expected.get_if<soml::array>();
			remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(index));
			require(soml::parse(*removed) == expected, "Removing an array item changes another value");
			path.clear();
		}
	}

	// Follow one input-selected path through mixed containers, and change its leaf independently of the editor.
	auto nested_expected = *document;
	soml::value* leaf = &nested_expected;
	path.clear();

	while (true) {
		const std::size_t selection = data[path.size() % size];

		if (auto* members = leaf->get_if<soml::object>(); members != nullptr && !members->empty()) {
			auto& member = *(members->begin() + static_cast<std::ptrdiff_t>(selection % members->size()));
			path.emplace_back(member.key);
			leaf = &member.value;
		} else if (auto* items = leaf->get_if<soml::array>(); items != nullptr && !items->empty()) {
			const std::size_t index = selection % items->size();
			path.emplace_back(index);
			leaf = &(*items)[index];
		} else {
			break;
		}
	}

	if (path.size() > 1) {
		const auto changed = soml::edit(text, path, nullptr);
		require(changed.has_value(), "Replacing a nested leaf failed");
		*leaf = nullptr;
		require(soml::parse(*changed) == nested_expected, "Replacing a nested leaf changes another value");
	}

	if (!path.empty()) {
		auto expected = *document;
		auto& parent = value_at(expected, std::span{path}.first(path.size() - 1));
		if (const auto* key = std::get_if<std::string>(&path.back())) {
			parent.get_if<soml::object>()->erase(*key);
		} else {
			auto& items = *parent.get_if<soml::array>();
			items.erase(items.begin() + static_cast<std::ptrdiff_t>(std::get<std::size_t>(path.back())));
		}
		const auto removed = soml::edit(text, path, std::nullopt);
		require(removed.has_value(), "Removing a nested leaf failed");
		require(soml::parse(*removed) == expected, "Removing a nested leaf changes another value");
		path.pop_back();
	}

	// Insert a string into the sampled parent, including empty root containers. The original document itself exercises escaping without adding nesting.
	auto inserted_expected = *document;
	auto& parent = value_at(inserted_expected, path);
	if (auto* members = parent.get_if<soml::object>()) {
		std::string key{"new\0key", 7};
		while (members->contains(key)) {
			key += '_';
		}
		members->insert_or_assign(key, text);
		path.emplace_back(std::move(key));
	} else {
		auto& items = *parent.get_if<soml::array>();
		path.emplace_back(items.size());
		items.emplace_back(text);
	}
	const auto inserted = soml::edit(text, path, text);
	require(inserted.has_value(), "Inserting a nested item failed");
	require(soml::parse(*inserted) == inserted_expected, "Inserting a nested item changes another value");

	// Decoding never crashes, and what it reads writes back.
	if (const auto decoded = soml::decode<everything>(text, {.ignore_unknown_keys = true})) {
		const auto encoded = soml::to_value(*decoded);
		require(encoded.has_value() || encoded.error().code != soml::error_code::syntax, "A decoded struct cannot be encoded");
	}

	static_cast<void>(soml::from_value<std::vector<soml::value>>(*document));
	static_cast<void>(soml::from_value<std::map<std::string, soml::value>>(*document));
	return 0;
}
