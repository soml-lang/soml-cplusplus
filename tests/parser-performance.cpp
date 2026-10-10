#include <soml.hpp>

#include <print>

namespace {

constexpr bool plain_integer_boundaries() {
	constexpr std::array<std::int64_t, 8> numbers{
		1'000'000'000'000'000'000,
		-1'000'000'000'000'000'000,
		std::numeric_limits<std::int64_t>::max() - 1,
		std::numeric_limits<std::int64_t>::max(),
		std::numeric_limits<std::int64_t>::min() + 1,
		std::numeric_limits<std::int64_t>::min(),
		0,
		-1,
	};
	for (const auto number : numbers) {
		std::string literal;
		soml::detail::append_integer(literal, number);
		for (const std::string_view separator : {",", " ", "\n", "# comment\n", "/* comment */"}) {
			const auto parsed = soml::parse("[" + literal + std::string{separator} + "]");
			if (!parsed || (*parsed)[0].as<std::int64_t>() != number) {
				return false;
			}
		}
	}
	for (const std::string_view literal : {"9223372036854775808", "-9223372036854775809", "9999999999999999999", "-9999999999999999999", "10000000000000000000", "-0"}) {
		const auto parsed = soml::parse("[" + std::string{literal} + "]");
		if (parsed || parsed.error().offset != 1) {
			return false;
		}
	}
	for (const std::string_view suffix : {".0", "e0", "_0", "s", "false"}) {
		const auto parsed = soml::parse("[1000000000000000000" + std::string{suffix} + "]");
		if (suffix == ".0" || suffix == "e0") {
			if (!parsed || (*parsed)[0].as<double>() != 1e18) {
				return false;
			}
		} else if (parsed) {
			return false;
		}
	}
	return true;
}

constexpr bool duration_whole_parts() {
	constexpr std::array<std::pair<std::string_view, std::int64_t>, 9> cases{{
		{"12h34m56s789ms123us456ns", 45'296'789'123'456},
		{"1_2h3_4m5_6s7_8_9ms1_2_3us4_5_6ns", 45'296'789'123'456},
		{"9223372036854775807ns", std::numeric_limits<std::int64_t>::max()},
		{"-9223372036854775808ns", std::numeric_limits<std::int64_t>::min()},
		{"9223372036s854ms775us807ns", std::numeric_limits<std::int64_t>::max()},
		{"-9223372036s854ms775us808ns", std::numeric_limits<std::int64_t>::min()},
		{"0h0m0s0ms0us0ns", 0},
		{"0.000000000005h", 18},
		{"-1_0s", -10'000'000'000},
	}};
	for (const auto& [literal, count] : cases) {
		const auto parsed = soml::parse("[" + std::string{literal} + "]");
		if (!parsed || (*parsed)[0].as<soml::duration>() != soml::duration{count}) {
			return false;
		}
	}
	for (const std::string_view literal : {"9223372036854775808ns", "-9223372036854775809ns", "18446744073709551616ns", "18_446_744_073_709_551_616ns", "18446744073709551616_0ns", "18446744073709551616h0ns", "0h-0ns"}) {
		if (soml::parse("[" + std::string{literal} + "]")) {
			return false;
		}
	}
	// Grammar errors must retain precedence even after an earlier part overflows.
	const auto wrong_unit = soml::parse("[18446744073709551616h1d]");
	const auto wrong_order = soml::parse("[18446744073709551616s1h]");
	return !wrong_unit && wrong_unit.error().message.contains("there is no day unit") && !wrong_order && wrong_order.error().message.contains("the units are in the order");
}

constexpr bool nested_stack_slices() {
	const auto parsed = soml::parse("before: 'retained before nested collections'\nouter: [1, {left: 'a sufficiently long owned string', inner: [2, {last: 3}], right: 4}, 5]\nafter: {final: 6}");
	if (!parsed) {
		return false;
	}
	const auto& root = *parsed->get_if<soml::object>();
	const auto outer = (*parsed)["outer"];
	return root.size() == 3 && root.begin()->key == "before" && outer[0].as<int>() == 1 && outer[1]["left"].as<std::string_view>() == "a sufficiently long owned string" && outer[1]["inner"][1]["last"].as<int>() == 3 && outer[1]["right"].as<int>() == 4 && outer[2].as<int>() == 5 && (*parsed)["after"]["final"].as<int>() == 6;
}

constexpr bool block_string_kept_lines() {
	for (const std::string_view quotes : {"'''", "\"\"\""}) {
		const auto empty = soml::parse("text: " + std::string{quotes} + "\n \t\n\t \n\t" + std::string{quotes});
		const auto kept = soml::parse("text: " + std::string{quotes} + "\n \t\n\t first\n\t \n\t last\n\t \n\t " + std::string{quotes});
		if (!empty || (*empty)["text"].as<std::string_view>() != "" || !kept || (*kept)["text"].as<std::string_view>() != "first\n\nlast") {
			return false;
		}
	}
	// A later indentation error must still take precedence over an earlier invalid escape.
	const auto invalid = soml::parse("text: \"\"\"\n\t\\q\nbad indentation\n\t\"\"\"");
	return !invalid && invalid.error().message.starts_with("This line does not start with the indentation") && invalid.error().line == 3;
}

static_assert(plain_integer_boundaries());
static_assert(duration_whole_parts());
static_assert(nested_stack_slices());
static_assert(block_string_kept_lines());

} // namespace

int main() {
	const bool passed = plain_integer_boundaries() && duration_whole_parts() && nested_stack_slices() && block_string_kept_lines();
	std::println("Parser integer boundaries, duration overflow precedence, nested stack slices, and kept block lines: {}", passed ? "passed" : "failed");
	return passed ? 0 : 1;
}
