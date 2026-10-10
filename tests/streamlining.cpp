#include <soml.hpp>

#include <print>

namespace {

struct test_results {
	std::size_t count = 0;
	std::size_t failures = 0;

	void check(std::string_view name, bool condition) {
		count++;
		if (!condition) {
			std::println(stderr, "FAIL: {}", name);
			failures++;
		}
	}
};

constexpr bool native_writers() {
	const soml::object object{{"z", soml::array{1, "two"}}, {"a", true}};
	const soml::array array{object, soml::duration{1}, soml::instant{0, 1}};
	for (const bool canonical : {false, true}) {
		const auto check = [&](const auto& native) {
			const soml::value wrapped{native};
			const auto expected = canonical ? soml::write_canonical(wrapped) : soml::write(wrapped);
			const auto actual = canonical ? soml::write_canonical(native) : soml::write(native);
			std::string output = "# prefix\n";
			const auto appended = canonical ? soml::write_canonical(native, output) : soml::write(native, output);
			return expected && actual == expected && appended && output == "# prefix\n" + *expected;
		};
		if (!check(object) || !check(array) || !check(soml::object{}) || !check(soml::array{})) {
			return false;
		}
	}
	return true;
}

static_assert(native_writers());

void native_validation(test_results& results) {
	const std::array<soml::value, 5> invalid{
		std::numeric_limits<double>::quiet_NaN(),
		std::string{"\xFF"},
		std::string{"carriage\rreturn"},
		soml::instant{0, -1},
		soml::instant{soml::instant::max().seconds + 1, 0},
	};
	for (const auto& item : invalid) {
		const auto check = [&](const auto& native) {
			const soml::value wrapped{native};
			for (const bool canonical : {false, true}) {
				const auto expected = canonical ? soml::write_canonical(wrapped) : soml::write(wrapped);
				const auto actual = canonical ? soml::write_canonical(native) : soml::write(native);
				std::string output = "unchanged";
				const auto appended = canonical ? soml::write_canonical(native, output) : soml::write(native, output);
				results.check("native writers preserve validation and error paths", !expected && actual == expected && !appended && appended.error() == expected.error() && output == "unchanged");
			}
		};
		check(soml::array{item});
		check(soml::object{{"invalid", item}});
	}

	soml::object duplicate{{"a", 1}, {"b", 2}};
	(duplicate.begin() + 1)->key = "a";
	const auto duplicate_result = soml::write(duplicate);
	results.check("native object writing still validates duplicate keys", !duplicate_result && duplicate_result.error().code == soml::error_code::invalid_value);

	soml::value nested = soml::array{};
	for (std::size_t level = 1; level < soml::max_depth; level++) {
		nested = soml::array{std::move(nested)};
	}
	const soml::array too_deep_array{nested};
	const soml::object too_deep_object{{"nested", nested}};
	const auto array_result = soml::write(too_deep_array);
	const auto object_result = soml::write(too_deep_object);
	results.check("native arrays include their own root in the nesting limit", !array_result && array_result.error().code == soml::error_code::too_deep);
	results.check("native objects include their own root in the nesting limit", !object_result && object_result.error().code == soml::error_code::too_deep);
}

void aliased_destinations(test_results& results) {
	for (const bool canonical : {false, true}) {
		soml::array array{std::string(100, 'x')};
		const auto expected_array = canonical ? soml::write_canonical(array) : soml::write(array);
		auto& array_destination = *array.front().get_if<std::string>();
		const std::string array_prefix = array_destination;
		const auto array_result = canonical ? soml::write_canonical(array, array_destination) : soml::write(array, array_destination);
		results.check("native array append supports a destination inside the array", array_result && array_destination == array_prefix + *expected_array);

		soml::object object{{"z", std::string(100, 'y')}, {"a", 1}};
		const auto expected_object = canonical ? soml::write_canonical(object) : soml::write(object);
		auto& object_destination = *object.find("z")->get_if<std::string>();
		const std::string object_prefix = object_destination;
		const auto object_result = canonical ? soml::write_canonical(object, object_destination) : soml::write(object, object_destination);
		results.check("native object append supports a destination inside the object", object_result && object_destination == object_prefix + *expected_object);
	}
}

} // namespace

int main() {
	test_results results;
	results.check("native writers preserve output at runtime", native_writers());
	native_validation(results);
	aliased_destinations(results);
	std::println("{} streamlining checks, {} failures", results.count, results.failures);
	return results.failures == 0 ? 0 : 1;
}
