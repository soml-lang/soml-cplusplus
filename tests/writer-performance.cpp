#include <soml.hpp>

#include <print>

namespace {

enum class named_value { primary, secondary, unnamed };

} // namespace

template<>
inline constexpr auto soml::enum_names<named_value> = std::array{
	std::pair{named_value::primary, std::string_view{"canonical"}},
	std::pair{named_value::primary, std::string_view{"alias"}},
	std::pair{named_value::secondary, std::string_view{"a\0b", 3}},
};

static_assert(soml::to_value(named_value::primary)->as<std::string_view>() == "canonical");
static_assert(soml::from_value<named_value>(soml::value{"alias"}) == named_value::primary);
static_assert(soml::from_value<named_value>(soml::value{std::string_view{"a\0b", 3}}) == named_value::secondary);
static_assert(!soml::from_value<named_value>(soml::value{"a"}));
static_assert(!soml::to_value(named_value::unnamed));
static_assert(soml::detail::edit_distance("prefix-ab-suffix", "prefix-ba-suffix") == 1);

namespace {

/*
An independent full-matrix optimal string alignment implementation, retaining every row so transpositions cannot be lost by row rotation or trimming.
*/
std::size_t distance_oracle(std::string_view first, std::string_view second) {
	const std::size_t columns = second.size() + 1;
	std::vector<std::size_t> distances((first.size() + 1) * columns);
	const auto cell = [&](std::size_t row, std::size_t column) -> std::size_t& {
		return distances[row * columns + column];
	};
	for (std::size_t row = 0; row <= first.size(); row++) {
		cell(row, 0) = row;
	}
	for (std::size_t column = 0; column <= second.size(); column++) {
		cell(0, column) = column;
	}
	for (std::size_t row = 1; row <= first.size(); row++) {
		for (std::size_t column = 1; column <= second.size(); column++) {
			cell(row, column) = std::min({cell(row - 1, column) + 1, cell(row, column - 1) + 1, cell(row - 1, column - 1) + (first[row - 1] == second[column - 1] ? 0 : 1)});
			if (row > 1 && column > 1 && first[row - 1] == second[column - 2] && first[row - 2] == second[column - 1]) {
				cell(row, column) = std::min(cell(row, column), cell(row - 2, column - 2) + 1);
			}
		}
	}
	return cell(first.size(), second.size());
}

} // namespace

int main() {
	std::vector<std::string> words{std::string{}};
	std::size_t layer_start = 0;
	for (std::size_t length = 0; length < 5; length++) {
		const std::size_t layer_end = words.size();
		for (std::size_t index = layer_start; index < layer_end; index++) {
			for (const char character : std::string_view{"abc"}) {
				words.push_back(words[index] + character);
			}
		}
		layer_start = layer_end;
	}
	std::size_t checked = 0;
	for (const auto& first : words) {
		for (const auto& second : words) {
			const std::size_t expected = distance_oracle(first, second);
			if (soml::detail::edit_distance(first, second) != expected) {
				std::println(stderr, "Distance mismatch for {:?} and {:?}: expected {}", first, second, expected);
				return 1;
			}
			checked++;
		}
	}
	const std::string prefix(5000, 'p');
	const std::string suffix(5000, 's');
	for (const auto& [first, second] : std::array{
		std::pair{"ab", "ba"},
		std::pair{"aab", "aba"},
		std::pair{"abb", "bab"},
		std::pair{"", "a"},
		std::pair{"ab", "a"},
	}) {
		if (soml::detail::edit_distance(prefix + first + suffix, prefix + second + suffix) != distance_oracle(first, second)) {
			std::println(stderr, "Common-prefix and common-suffix distance mismatch");
			return 1;
		}
	}
	std::println("{} exhaustive edit-distance comparisons passed", checked);
	return 0;
}
