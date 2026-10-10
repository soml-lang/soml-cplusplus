#include <soml.hpp>

#include <map>
#include <print>
#include <random>

namespace {

struct test_case {
	std::string_view name;
	bool (*run)();
};

struct mutable_sequence : std::vector<int> {
	auto begin() {
		return std::vector<int>::begin();
	}

	auto end() {
		return std::vector<int>::end();
	}
};

struct nondefault_sequence : std::vector<int> {
	nondefault_sequence() = delete;

	explicit nondefault_sequence(int item) : std::vector<int>{item} {}
};

struct mutable_map : std::map<std::string, int> {
	auto begin() {
		return std::map<std::string, int>::begin();
	}

	auto end() {
		return std::map<std::string, int>::end();
	}
};

struct nondefault_map : std::map<std::string, int> {
	nondefault_map() = delete;

	explicit nondefault_map(int item) : std::map<std::string, int>{{"item", item}} {}
};

struct nondefault_record {
	explicit nondefault_record(int value) : port(value) {}
	int port;
};

struct nonmovable_record {
	int port = 0;
	nonmovable_record() = default;
	nonmovable_record(nonmovable_record&&) = delete;
	nonmovable_record(const nonmovable_record&) = delete;
};

struct copy_only_record {
	int port = 0;
	copy_only_record() = default;
	copy_only_record(const copy_only_record&) = default;
};

struct readonly_record {
	const int port = 80;
};

struct recursive_record {
	std::vector<recursive_record> children;
};

#if defined(__cpp_impl_reflection)
struct reflected_readonly_record {
	const int port = 80;
};

struct reflected_recursive_record {
	std::vector<reflected_recursive_record> children;
};
#endif

struct nonmovable_sequence : std::vector<int> {
	nonmovable_sequence() = default;
	nonmovable_sequence(nonmovable_sequence&&) = delete;
	nonmovable_sequence(const nonmovable_sequence&) = delete;
};

struct nonmovable_map : std::map<std::string, int> {
	nonmovable_map() = default;
	nonmovable_map(nonmovable_map&&) = delete;
	nonmovable_map(const nonmovable_map&) = delete;
};

struct copy_only_sequence : std::vector<int> {
	copy_only_sequence() = default;
	copy_only_sequence(const copy_only_sequence&) = default;
};

struct copy_only_map : std::map<std::string, int> {
	copy_only_map() = default;
	copy_only_map(const copy_only_map&) = default;
};

struct derived_object : soml::object {
	using soml::object::object;
	int members_ = 0;
};

struct derived_value : soml::value {
	using soml::value::value;
	int storage_ = 0;

	template<class T>
	T* get_if() {
		return nullptr;
	}
};

} // namespace

template<>
inline constexpr auto soml::fields<nondefault_record> = std::tuple{soml::field{"port", &nondefault_record::port}};

template<>
inline constexpr auto soml::fields<nonmovable_record> = std::tuple{soml::field{"port", &nonmovable_record::port}};

template<>
inline constexpr auto soml::fields<copy_only_record> = std::tuple{soml::field{"port", &copy_only_record::port}};

template<>
inline constexpr auto soml::fields<readonly_record> = std::tuple{soml::field{"port", &readonly_record::port}};

template<>
inline constexpr auto soml::fields<recursive_record> = std::tuple{soml::field{"children", &recursive_record::children}};

template<>
struct soml::converter<char> {
	static constexpr std::expected<char, soml::error> from_value(const soml::value&, const soml::decode_options&) {
		return 'x';
	}
};

namespace {

static_assert(!soml::decodable<nondefault_record>);
static_assert(!soml::decodable<nonmovable_record>);
static_assert(!soml::decodable<nonmovable_sequence>);
static_assert(!soml::decodable<nonmovable_map>);
static_assert(soml::encodable<nondefault_record>);
static_assert(soml::encodable<nonmovable_record>);
static_assert(soml::encodable<nonmovable_sequence>);
static_assert(soml::encodable<nonmovable_map>);
static_assert(soml::decodable<copy_only_record>);
static_assert(!soml::decodable<readonly_record>);
static_assert(soml::encodable<readonly_record>);
static_assert(soml::decodable<recursive_record>);
#if defined(__cpp_impl_reflection)
static_assert(!soml::decodable<reflected_readonly_record>);
static_assert(soml::encodable<reflected_readonly_record>);
static_assert(soml::decodable<reflected_recursive_record>);
static_assert(soml::encode(reflected_readonly_record{}) == "port: 80\n");
static_assert(soml::decode<reflected_recursive_record>("children: [{children: []}]")->children.size() == 1);
#endif
static_assert(soml::decodable<copy_only_sequence>);
static_assert(soml::decodable<copy_only_map>);
static_assert(std::same_as<decltype(soml::view{}.value_or(nullptr)), std::nullptr_t>);
static_assert(soml::view{}.value_or(nullptr) == nullptr);
static_assert([] {
	char fallback[] = "fallback";
	const soml::value text{"stored"};
	return soml::view{text}.value_or(fallback) == "stored" && soml::view{text}.value_or("fallback") == "stored";
}());

static_assert(!soml::encodable<mutable_sequence>);
static_assert(soml::decodable<mutable_sequence>);
static_assert(!soml::decodable<nondefault_sequence>);
static_assert(soml::encodable<nondefault_sequence>);
static_assert(!soml::encodable<mutable_map>);
static_assert(soml::decodable<mutable_map>);
static_assert(!soml::decodable<nondefault_map>);
static_assert(soml::encodable<nondefault_map>);

__extension__ typedef unsigned __int128 wide_unsigned_integer;

template<class Duration>
concept convertible_instant = requires(const soml::instant& time) {
	time.template to_sys_time<Duration>();
};

template<class Representation>
constexpr bool rejects_wide_chrono_representation() {
	using duration = std::chrono::duration<Representation>;
	using time = std::chrono::sys_time<duration>;
	return !soml::encodable<duration> && !soml::decodable<duration> && !soml::encodable<time> && !soml::decodable<time> && !std::constructible_from<soml::value, duration> && !std::constructible_from<soml::value, time> && !std::constructible_from<soml::instant, time> && !convertible_instant<duration>;
}

static_assert(rejects_wide_chrono_representation<soml::detail::int128>());
static_assert(rejects_wide_chrono_representation<wide_unsigned_integer>());
static_assert(soml::encodable<std::chrono::duration<std::int64_t>> && soml::decodable<std::chrono::duration<std::int64_t>>);
static_assert(soml::encodable<std::chrono::duration<std::uint64_t>> && soml::decodable<std::chrono::duration<std::uint64_t>>);
static_assert(soml::encodable<std::chrono::duration<float>> && soml::decodable<std::chrono::duration<float>>);
static_assert(soml::encodable<std::chrono::duration<double>> && soml::decodable<std::chrono::duration<double>>);
static_assert(soml::encodable<std::chrono::duration<long double>> && soml::decodable<std::chrono::duration<long double>>);
static_assert(convertible_instant<std::chrono::duration<long double>>);
static_assert(soml::decodable<std::chrono::sys_time<std::chrono::duration<long double>>>);

soml::object numbered_object(std::size_t count) {
	std::vector<soml::member> members;
	members.reserve(count);
	for (std::size_t index = 0; index < count; index++) {
		members.push_back({std::to_string(index), static_cast<std::int64_t>(index)});
	}
	return soml::object{std::move(members)};
}

constexpr bool constant_evaluation() {
	const soml::object object{std::vector<soml::member>{{"z", 0}, {"a", 2}, {"z", 3}}};
	std::string output = "prefix\n";
	return object.begin()->key == "z" && object.find("z")->as<int>() == 3 && soml::write_canonical(object, output) && output == "prefix\na: 2\nz: 3\n" && soml::write(soml::array{std::numeric_limits<std::int64_t>::min(), 1'000'000.0}) == "[\n\t-9223372036854775808\n\t1000000.0\n]\n";
}

static_assert(constant_evaluation());
static_assert([] {
	const soml::object original{{"a", 1}, {"b", soml::array{2}}, {"c", 3}};
	const soml::object reordered{{"a", 1}, {"c", 3}, {"b", soml::array{2}}};
	const soml::object changed{{"a", 9}, {"c", 3}, {"b", soml::array{2}}};
	return original == original && original == reordered && reordered == original && original != changed && changed != original && soml::object{} == soml::object{};
}());
static_assert([] {
	const soml::value source{soml::array{1, soml::object{{"z", 3}, {"a", 2}}}};
	return soml::to_value(*source.get_if<soml::array>()) == source && soml::from_value<soml::array>(source) == *source.get_if<soml::array>() && soml::encode<soml::value>(source) == soml::write(source) && soml::encode_canonical<soml::value>(source) == soml::write_canonical(source);
}());
static_assert(soml::decode<soml::value>("z: [1, {a: 2}]", {.ignore_unknown_keys = true}) == soml::parse("z: [1, {a: 2}]"));
static_assert(soml::decode<soml::array>("[1, 2]") == soml::array{1, 2});
static_assert(soml::decode<soml::object>("a: 1")->find("a")->as<int>() == 1);
static_assert(soml::decode<char>("a: 1") == 'x');
static_assert(soml::parse("time: 0001-01-01T00:00:00Z")->find("time")->as<soml::instant>() == soml::instant::min());
static_assert(soml::parse("time: 9999-12-31T23:59:59.999999999Z")->find("time")->as<soml::instant>() == soml::instant::max());
static_assert([] {
	return soml::value{127}.as<std::int8_t>() == 127 && soml::value{-128}.as<std::int8_t>() == -128 && !soml::value{128}.as<std::int8_t>() && !soml::value{-129}.as<std::int8_t>() && !soml::value{-1}.as<unsigned>() && soml::value{255}.as<std::uint8_t>() == 255 && !soml::value{256}.as<std::uint8_t>() && !soml::value{3.0}.as<int>() && !soml::value{"3"}.as<int>() && soml::value{1}.as<char>() == 'x';
}());
static_assert(soml::value{}.as<std::nullptr_t>().has_value() && soml::value{}.as<std::nullptr_t>() == nullptr && !soml::value{false}.as<std::nullptr_t>());
static_assert(soml::value{0.5}.as<float>() == 0.5f && soml::value{0.5}.as<long double>() == 0.5L && !soml::value{1}.as<float>() && !soml::value{"1"}.as<long double>());
static_assert(soml::value{3.40282356e38}.as<float>() == std::numeric_limits<float>::max() && !soml::value{3.40282357e38}.as<float>() && !soml::value{-3.40282357e38}.as<float>());
static_assert(!soml::value{0x1.ffffffp127}.as<float>() && !soml::value{-0x1.ffffffp127}.as<float>() && soml::value{0x1.ffffffp127 - 0x1p75}.as<float>() == std::numeric_limits<float>::max() && soml::value{-(0x1.ffffffp127 - 0x1p75)}.as<float>() == std::numeric_limits<float>::lowest());
static_assert([] {
	const soml::array items{soml::object{{"z", 3}, {"a", 2}}};
	const soml::object members{{"z", items}, {"a", soml::array{}}};
	return soml::encode(items) == soml::write(items) && soml::encode_canonical<soml::array>(items) == soml::write_canonical(items) && soml::encode<soml::object>(members) == soml::write(members) && soml::encode_canonical(members) == soml::write_canonical(members) && soml::encode(soml::array{}) == "[]\n" && soml::encode(soml::object{}) == "{}\n";
}());
static_assert([] {
	std::string text = "native ownership";
	auto string = soml::from_value<std::string>(*soml::to_value<std::string>(std::move(text)));
	soml::array items{std::move(*string)};
	soml::value document{soml::object{{"items", *soml::to_value(std::move(items))}}};
	auto encoded = soml::to_value<soml::value>(std::move(document));
	auto members = soml::from_value<soml::object>(std::move(*encoded));
	auto extracted = soml::from_value<soml::array>(std::move(*members->find("items")));
	return extracted && extracted->front().as<std::string_view>() == "native ownership";
}());
static_assert(std::same_as<decltype(std::declval<soml::value&>().get_if<std::int64_t>()), std::int64_t*>);
static_assert(std::same_as<decltype(std::declval<const soml::value&>().get_if<std::int64_t>()), const std::int64_t*>);
static_assert(std::same_as<decltype(std::declval<soml::value&&>().get_if<std::int64_t>()), std::int64_t*>);
static_assert(std::same_as<decltype(std::declval<const soml::value&&>().get_if<std::int64_t>()), const std::int64_t*>);
static_assert(std::same_as<decltype(std::declval<soml::value&>().variant()), soml::value::storage&>);
static_assert(std::same_as<decltype(std::declval<const soml::value&>().variant()), const soml::value::storage&>);
static_assert(std::same_as<decltype(std::declval<soml::object&>().begin()), soml::object::iterator>);
static_assert(std::same_as<decltype(std::declval<soml::object&>().end()), soml::object::iterator>);
static_assert(std::same_as<decltype(std::declval<const soml::object&>().begin()), soml::object::const_iterator>);
static_assert(std::same_as<decltype(std::declval<const soml::object&>().end()), soml::object::const_iterator>);
static_assert(std::same_as<decltype(std::declval<soml::object&&>().begin()), soml::object::iterator>);
static_assert(std::same_as<decltype(std::declval<const soml::object&&>().end()), soml::object::const_iterator>);
static_assert(std::ranges::range<soml::object> && std::ranges::range<const soml::object>);
static_assert([]<class T>() {
	return std::same_as<decltype(std::declval<T&>().find("a")), soml::value*> && std::same_as<decltype(std::declval<T&&>().find("a")), soml::value*> && std::same_as<decltype(std::declval<const T&>().find("a")), const soml::value*> && std::same_as<decltype(std::declval<const T&&>().find("a")), const soml::value*>;
}.operator()<soml::object>());
static_assert([]<class T>() {
	return std::same_as<decltype(std::declval<T&>().find("a")), soml::value*> && std::same_as<decltype(std::declval<T&&>().find("a")), soml::value*> && std::same_as<decltype(std::declval<const T&>().find("a")), const soml::value*> && std::same_as<decltype(std::declval<const T&&>().find("a")), const soml::value*>;
}.operator()<soml::value>());
static_assert([] {
	derived_object object{{"a", 1}};
	derived_value value{soml::object{{"a", 1}}};
	*object.find("a") = 2;
	*value.find("a") = 2;
	return object.find("a")->as<int>() == 2 && value.find("a")->as<int>() == 2 && object.find("x") == nullptr && value.find("x") == nullptr;
}());
static_assert([] {
	const soml::object object{{"a", 1}, {"b", 2}};
	std::int64_t sum = 0;
	for (const auto& [key, value] : object) {
		if (key.empty()) {
			return false;
		}
		sum += value.as<std::int64_t>().value_or(0);
	}
	return sum == 3;
}());

} // namespace

int main() {
	const std::array cases{
		test_case{"calendar month and century boundaries round-trip through public instant APIs", [] {
			for (const int year : {1, 4, 100, 400, 1600, 1900, 1969, 1970, 2000, 2026, 2100, 9999}) {
				for (unsigned month = 1; month <= 12; month++) {
					const std::chrono::year_month_day_last last{std::chrono::year{year}, std::chrono::month_day_last{std::chrono::month{month}}};
					for (const unsigned day : {1U, static_cast<unsigned>(last.day())}) {
						const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
						soml::instant expected{std::chrono::sys_days{date} + std::chrono::hours{12} + std::chrono::minutes{34} + std::chrono::seconds{56}};
						expected.nanoseconds = 123'456'789;
						const std::string text = std::format("time: {:04}-{:02}-{:02}T12:34:56.123456789Z\n", year, month, day);
						const auto parsed = soml::parse(text);
						if (!parsed || parsed->find("time")->as<soml::instant>() != expected || soml::write(*parsed) != text || soml::write_canonical(*parsed) != text) {
							return false;
						}
					}
				}
			}
			return true;
		}},
		test_case{"native collection decoding preserves mismatch locations and custom converters", [] {
			const auto items = soml::decode<soml::array>("[1, {name: 'api'}]");
			const auto members = soml::decode<soml::object>("a: [1, 2]");
			const auto wrong_array = soml::decode<soml::array>("a: [1, 2]");
			const auto wrong_object = soml::decode<soml::object>("[1, 2]");
			return items && items->size() == 2 && items->back()["name"].as<std::string_view>() == "api" && members && members->find("a")->size() == 2 && !wrong_array && wrong_array.error().code == soml::error_code::type_mismatch && wrong_array.error().line == 1 && wrong_array.error().column == 1 && !wrong_object && wrong_object.error().code == soml::error_code::type_mismatch && wrong_object.error().line == 1 && wrong_object.error().column == 1 && soml::decode<char>("[1, 2]") == 'x';
		}},
		test_case{"native ownership transfers support deduced and explicit conversion calls", [] {
			const auto address = [](const auto& source) -> const void* {
				using native = std::remove_cvref_t<decltype(source)>;
				if constexpr (std::same_as<native, soml::value>) {
					return source.template get_if<soml::array>()->data();
				} else if constexpr (std::same_as<native, soml::object>) {
					return source.members().data();
				} else {
					return source.data();
				}
			};
			const auto transfers = [&]<class Native>(const Native& original) {
				const auto expected = soml::to_value(original);
				for (const bool explicit_template : {false, true}) {
					auto source = original;
					const void* storage = address(source);
					const auto moved = explicit_template ? soml::to_value<Native>(std::move(source)) : soml::to_value(std::move(source));
					if (moved != expected) {
						return false;
					}
					if constexpr (std::same_as<Native, soml::value>) {
						if (address(*moved) != storage) {
							return false;
						}
					} else if (address(*moved->template get_if<Native>()) != storage) {
						return false;
					}
				}
				auto document = *expected;
				const void* storage = nullptr;
				if constexpr (std::same_as<Native, soml::value>) {
					storage = address(document);
				} else {
					storage = address(*document.template get_if<Native>());
				}
				const auto moved = soml::from_value<Native>(std::move(document));
				return moved && *moved == original && address(*moved) == storage;
			};
			const soml::array items{1, std::string(100, 'x')};
			const soml::object members{{"items", items}};
			const std::string text(100, 'x');
			const soml::value document{items};
			return transfers(items) && transfers(members) && transfers(text) && transfers(document);
		}},
		test_case{"native lvalue and const-rvalue conversions keep independent owners", [] {
			const soml::value source{soml::array{std::string(100, 'x')}};
			const auto copied = soml::to_value(source);
			const auto const_rvalue = soml::to_value(std::move(source));
			const auto extracted = soml::from_value<soml::array>(source);
			const auto extracted_const_rvalue = soml::from_value<soml::array>(std::move(source));
			const auto* original = source.get_if<soml::array>();
			return copied == source && const_rvalue == source && extracted && extracted_const_rvalue && *extracted == *original && *extracted_const_rvalue == *original && copied->get_if<soml::array>()->data() != original->data() && const_rvalue->get_if<soml::array>()->data() != original->data() && extracted->data() != original->data() && extracted_const_rvalue->data() != original->data();
		}},
		test_case{"failed native ownership extraction preserves its source and diagnostic", [] {
			soml::value source{std::string(100, 'x')};
			const auto before = source;
			const char* storage = source.get_if<std::string>()->data();
			const auto expected_array = soml::from_value<soml::array>(source);
			const auto expected_object = soml::from_value<soml::object>(source);
			return soml::from_value<soml::array>(std::move(source)) == expected_array && soml::from_value<soml::object>(std::move(source)) == expected_object && source == before && source.get_if<std::string>()->data() == storage;
		}},
		test_case{"native value decoding preserves parsing errors and ignores struct options", [] {
			for (const std::string_view text : {"z: [1, {a: '😀'}]\na: true", "[]", "", "key: [1,, 2]"}) {
				const auto parsed = soml::parse(text);
				if (soml::decode<soml::value>(text) != parsed || soml::decode<soml::value>(text, {.ignore_unknown_keys = true}) != parsed) {
					return false;
				}
			}
			return true;
		}},
		test_case{"native array conversions preserve owning identity and mismatch errors", [] {
			soml::value source{soml::array{soml::object{{"name", "original"}}, soml::duration{3}, soml::instant{0, 0}}};
			const auto copied = soml::from_value<soml::array>(source);
			const auto encoded = soml::to_value(*source.get_if<soml::array>());
			const auto mismatch = soml::from_value<soml::array>(soml::object{});
			*source.get_if<soml::array>()->front().find("name") = "changed";
			return copied && encoded && *encoded->get_if<soml::array>() == *copied && copied->front()["name"].as<std::string_view>() == "original" && !mismatch && mismatch.error().code == soml::error_code::type_mismatch;
		}},
		test_case{"native value encoding preserves writer validation and canonical ordering", [] {
			const soml::value source = soml::object{{"z", soml::array{3, 2}}, {"a", 1}};
			const soml::value invalid = soml::object{{"nested", soml::array{std::numeric_limits<double>::quiet_NaN()}}};
			return soml::encode(source) == soml::write(source) && soml::encode<soml::value>(source) == soml::write(source) && soml::encode_canonical(source) == soml::write_canonical(source) && soml::encode_canonical<soml::value>(source) == soml::write_canonical(source) && soml::encode(invalid) == soml::write(invalid) && soml::encode_canonical(invalid) == soml::write_canonical(invalid);
		}},
		test_case{"native root encoders preserve nested validation and floating extraction boundaries", [] {
			const soml::array items{soml::object{{"invalid", std::numeric_limits<double>::quiet_NaN()}}};
			const soml::object members{{"invalid", items}};
			const soml::value infinity{std::numeric_limits<double>::infinity()};
			const soml::value nan{std::numeric_limits<double>::quiet_NaN()};
			const auto extracted_nan = nan.as<float>();
			return soml::encode(items) == soml::write(items) && soml::encode_canonical(items) == soml::write_canonical(items) && soml::encode(members) == soml::write(members) && soml::encode_canonical(members) == soml::write_canonical(members) && infinity.as<float>() == std::numeric_limits<float>::infinity() && extracted_nan && std::isnan(*extracted_nan) && std::signbit(*soml::value{-0.0}.as<float>());
		}},
		test_case{"container extraction returns independent owning copies", [] {
			soml::value source = soml::object{{"servers", soml::array{soml::object{{"name", "original"}}}}, {"keep", 9}};
			const auto copied_object = source.as<soml::object>();
			const auto copied_array = source["servers"].as<soml::array>();
			*source.find("servers")->get_if<soml::array>()->front().find("name") = "changed";
			return copied_object && copied_array && copied_object->find("servers")->get_if<soml::array>()->front()["name"].as<std::string_view>() == "original" && copied_array->front()["name"].as<std::string_view>() == "original" && !source.as<soml::array>() && !source["servers"].as<soml::object>();
		}},
		test_case{"copy-only sequences and maps decode without a move constructor", [] {
			const auto sequence = soml::from_value<copy_only_sequence>(soml::array{1, 2, 3});
			const auto map = soml::from_value<copy_only_map>(soml::object{{"first", 1}, {"second", 2}});
			return sequence && static_cast<const std::vector<int>&>(*sequence) == std::vector{1, 2, 3} && map && map->at("first") == 1 && map->at("second") == 2;
		}},
		test_case{"copy-only mapped records decode and nondefault records still encode", [] {
			const auto record = soml::from_value<copy_only_record>(soml::object{{"port", 8080}});
			const auto encoded = soml::to_value(nondefault_record{8080});
			const soml::value expected = soml::object{{"port", 8080}};
			return record && record->port == 8080 && encoded == expected;
		}},
		test_case{"the largest supported chrono counts reject overflow without direct construction", [] {
			using signed_duration = std::chrono::duration<std::int64_t, std::ratio<1'000'000'000>>;
			using unsigned_duration = std::chrono::duration<std::uint64_t, std::ratio<1'000'000'000>>;
			const signed_duration positive{std::numeric_limits<std::int64_t>::max()};
			const signed_duration negative{std::numeric_limits<std::int64_t>::min()};
			const unsigned_duration unsigned_positive{std::numeric_limits<std::uint64_t>::max()};
			if (soml::to_value(positive) || soml::to_value(negative) || soml::to_value(unsigned_positive)) {
				return false;
			}
			static_assert(!std::constructible_from<soml::value, signed_duration>);
			static_assert(!std::constructible_from<soml::value, unsigned_duration>);
			using coarse_duration = std::chrono::duration<std::uint64_t, std::ratio<std::numeric_limits<std::int64_t>::max()>>;
			const std::chrono::sys_time<coarse_duration> time{coarse_duration{std::numeric_limits<std::uint64_t>::max()}};
			const soml::instant instant{time};
			return !instant.is_valid() && !soml::to_value(time) && !soml::write(soml::array{soml::value{time}});
		}},
		test_case{"floating chrono representations retain duration and instant conversions", [] {
			using floating_duration = std::chrono::duration<long double>;
			using floating_time = std::chrono::sys_time<floating_duration>;
			const auto duration = soml::to_value(floating_duration{1.5L});
			const auto decoded_duration = soml::from_value<floating_duration>(soml::duration{1'500'000'000});
			const auto time = soml::instant{1, 500'000'000}.to_sys_time<floating_duration>();
			const auto decoded_time = soml::from_value<floating_time>(soml::instant{1, 500'000'000});
			return duration == soml::value{soml::duration{1'500'000'000}} && decoded_duration && decoded_duration->count() == 1.5L && time && time->time_since_epoch().count() == 1.5L && decoded_time && decoded_time->time_since_epoch().count() == 1.5L;
		}},
		test_case{"ASCII lane checks do not borrow between adjacent bytes", [] {
			for (std::size_t lane = 0; lane < 8; lane++) {
				for (std::uint64_t byte = 0; byte < 128; byte++) {
					const std::uint64_t shift = lane * 8;
					const std::uint64_t word = (0x4141'4141'4141'4141 & ~(0xFFULL << shift)) | (byte << shift);
					for (const std::uint64_t target : {0ULL, 9ULL, 10ULL, 13ULL, 39ULL, 127ULL}) {
						if (soml::detail::ascii_equal_lanes(word, target) != (byte == target ? 0x80ULL << shift : 0)) {
							return false;
						}
					}
					if (soml::detail::ascii_below_lanes(word, 0x20) != (byte < 0x20 ? 0x80ULL << shift : 0)) {
						return false;
					}
					for (const std::uint64_t bound : {0ULL, 48ULL, 58ULL, 65ULL, 91ULL, 97ULL, 123ULL, 128ULL}) {
						const std::uint64_t lane_mask = 0x80ULL << shift;
						const std::uint64_t other_lanes = 65 < bound ? 0x8080'8080'8080'8080 & ~lane_mask : 0;
						if (soml::detail::ascii_below_lanes(word, bound) != (other_lanes | (byte < bound ? lane_mask : 0))) {
							return false;
						}
					}
				}
			}
			return true;
		}},
		test_case{"code frame windows retain exact caret padding at both ends and the middle", [] {
			struct window_case {
				std::size_t caret;
				std::size_t padding;
				bool leading;
				bool trailing;
			};
			const std::array windows{
				window_case{0, 0, false, true}, window_case{50, 50, false, true}, window_case{51, 51, true, true}, window_case{100, 51, true, true}, window_case{150, 51, true, false}, window_case{200, 101, true, false},
			};
			for (const auto& [caret, padding, leading, trailing] : windows) {
				const soml::error error{.message = {}, .offset = caret, .line = 1, .column = caret + 1, .path = {}};
				const std::string expected = "> 1 | " + std::string{leading ? "…" : ""} + std::string(100, 'x') + (trailing ? "…" : "") + "\n    | " + std::string(padding, ' ') + '^';
				if (error.code_frame(std::string(200, 'x')) != expected) {
					return false;
				}
			}
			return true;
		}},
		test_case{"hexadecimal rendering shares case and padding without truncating digits", [] {
			return soml::detail::hex_upper(0, 4) == "0000" && soml::detail::hex_lower(0) == "0" && soml::detail::hex_upper(0xABCDEF01, 2) == "ABCDEF01" && soml::detail::hex_lower(0xABCDEF01) == "abcdef01" && soml::detail::hex_upper(1, 20) == "00000000000000000001";
		}},
		test_case{"mutable-only sequences still decode without requiring const iteration", [] {
			const auto decoded = soml::from_value<mutable_sequence>(soml::array{1, 2, 3});
			return decoded && static_cast<const std::vector<int>&>(*decoded) == std::vector{1, 2, 3};
		}},
		test_case{"sequences without a default constructor still encode", [] {
			return soml::to_value(nondefault_sequence{7}) == soml::value{soml::array{7}};
		}},
		test_case{"mutable-only maps still decode without requiring const iteration", [] {
			const auto decoded = soml::from_value<mutable_map>(soml::object{{"first", 1}, {"second", 2}});
			return decoded && decoded->at("first") == 1 && decoded->at("second") == 2 && decoded->size() == 2;
		}},
		test_case{"maps without a default constructor still encode", [] {
			return soml::to_value(nondefault_map{7}) == soml::value{soml::object{{"item", 7}}};
		}},
		test_case{"integer rendering agrees with charconv for deterministic random int64 values", [] {
			std::mt19937_64 random{42};
			for (int iteration = 0; iteration < 2000; iteration++) {
				const auto number = std::bit_cast<std::int64_t>(random());
				const auto written = soml::write(soml::array{number});
				if (!written || *written != "[\n\t" + std::to_string(number) + "\n]\n" || soml::parse(*written) != soml::value{soml::array{number}}) {
					return false;
				}
			}
			return true;
		}},
		test_case{"integer extrema retain exact spelling", [] {
			return soml::write(soml::array{std::numeric_limits<std::int64_t>::min(), 0, std::numeric_limits<std::int64_t>::max()}) == "[\n\t-9223372036854775808\n\t0\n\t9223372036854775807\n]\n";
		}},
		test_case{"unsigned range diagnostics do not wrap", [] {
			const auto result = soml::to_value(std::numeric_limits<std::uint64_t>::max());
			return !result && result.error().code == soml::error_code::out_of_range && result.error().message == "The integer 18446744073709551615 is outside the 64-bit range of a SOML int";
		}},
		test_case{"whole floats drop only trailing zeros", [] {
			return soml::write(soml::array{10.0, 1000000000000000.0, 1001000.0, 9007199254740991.0}) == "[\n\t10.0\n\t1000000000000000.0\n\t1001000.0\n\t9007199254740991.0\n]\n";
		}},
		test_case{"negative whole floats preserve their sign and float type", [] {
			const soml::value source = soml::array{-1000.0, -1001000.0, -9007199254740991.0};
			const auto written = soml::write(source);
			return written == "[\n\t-1000.0\n\t-1001000.0\n\t-9007199254740991.0\n]\n" && soml::parse(*written) == source;
		}},
		test_case{"bulk construction agrees with sequential assignment across algorithm thresholds", [] {
			std::mt19937 random{43};
			for (const std::size_t count : {0UZ, 1UZ, 16UZ, 17UZ, 128UZ}) {
				std::vector<soml::member> members;
				soml::object expected;
				for (std::size_t index = 0; index < count; index++) {
					const std::string key = std::to_string(random() % 12);
					const soml::value item = soml::array{static_cast<std::int64_t>(index), "owned payload"};
					members.push_back({key, item});
					expected.insert_or_assign(key, item);
				}
				if (soml::write(soml::object{std::move(members)}) != soml::write(expected)) {
					return false;
				}
			}
			return true;
		}},
		test_case{"bulk compaction preserves first positions and last nested values", [] {
			const soml::object object{std::vector<soml::member>{{"z", 0}, {"z", soml::array{1}}, {"a", 2}, {"z", soml::object{{"last", 3}}}, {"b", 4}, {"a", soml::array{5}}}};
			return soml::write(object) == "z: {\n\tlast: 3\n}\na: [\n\t5\n]\nb: 4\n";
		}},
		test_case{"bulk keys with embedded NUL remain distinct", [] {
			const std::string first{"a\0x", 3};
			const std::string second{"a\0y", 3};
			const soml::object object{std::vector<soml::member>{{first, 1}, {second, 2}, {first, 3}}};
			return object.size() == 2 && object.begin()->key == first && object.find(first)->as<int>() == 3 && object.find(second)->as<int>() == 2;
		}},
		test_case{"large all-duplicate batches keep the final value", [] {
			std::vector<soml::member> members;
			for (int index = 0; index < 100; index++) {
				members.push_back({"same", soml::array{index}});
			}
			return soml::write(soml::object{std::move(members)}) == "same: [\n\t99\n]\n";
		}},
		test_case{"ordered large objects compare nested values", [] {
			const auto first = numbered_object(128);
			auto second = first;
			second.insert_or_assign("127", soml::array{127});
			return first == numbered_object(128) && first != second && second != first;
		}},
		test_case{"object comparison keeps every permutation and rejects each changed value", [] {
			std::array order{0, 1, 2, 3, 4};
			soml::object original;
			for (const int index : order) {
				original.insert_or_assign(std::to_string(index), soml::array{index, soml::object{{"nested", index}}});
			}
			do {
				soml::object reordered;
				for (const int index : order) {
					reordered.insert_or_assign(std::to_string(index), *original.find(std::to_string(index)));
				}
				if (original != reordered || reordered != original) {
					return false;
				}
				for (int changed = 0; changed < 5; changed++) {
					auto different = reordered;
					different.insert_or_assign(std::to_string(changed), -1);
					if (original == different || different == original) {
						return false;
					}
				}
			} while (std::ranges::next_permutation(order).found);
			return true;
		}},
		test_case{"large object equality ignores every shuffled position", [] {
			const auto first = numbered_object(128);
			auto second = first;
			std::mt19937 random{44};
			std::shuffle(second.begin(), second.end(), random);
			return first == second && second == first && soml::write_canonical(first) == soml::write_canonical(second);
		}},
		test_case{"large equal-sized objects with different key sets are unequal", [] {
			const auto first = numbered_object(128);
			auto second = first;
			second.begin()[64].key = "different";
			return first != second && second != first;
		}},
		test_case{"object self-comparison still rejects NaN equality", [] {
			auto object = numbered_object(128);
			object.insert_or_assign("0", std::numeric_limits<double>::quiet_NaN());
			return object != object;
		}},
		test_case{"mutable and const lookup refer to the same storage", [] {
			soml::object object{{"key", 1}};
			const auto* original = std::as_const(object).find("key");
			*object.find("key") = 2;
			if (original != object.find("key") || original->as<int>() != 2) {
				return false;
			}
			soml::value value{std::move(object)};
			const auto* stored = std::as_const(value).find("key");
			*value.find("key") = 3;
			return stored == value.find("key") && stored->as<int>() == 3 && value.find("missing") == nullptr && soml::value{1}.find("key") == nullptr;
		}},
		test_case{"mutable character buffers provide string fallbacks", [] {
			char fallback[] = "fallback";
			const soml::value text{"stored"};
			const soml::value number{1};
			return soml::view{}.value_or(fallback) == "fallback" && soml::view{text}.value_or(fallback) == "stored" && soml::view{number}.value_or(fallback) == "fallback";
		}},
		test_case{"null fallbacks keep the null conversion overload", [] {
			const soml::value null{nullptr};
			const soml::value number{1};
			return soml::view{}.value_or(nullptr) == nullptr && soml::view{null}.value_or(nullptr) == nullptr && soml::view{number}.value_or(nullptr) == nullptr;
		}},
		test_case{"readonly mapped fields encode and recursive mapped fields decode", [] {
			const auto decoded = soml::decode<recursive_record>("children: [{children: []}]");
			return soml::encode(readonly_record{}) == "port: 80\n" && decoded && decoded->children.size() == 1 && decoded->children[0].children.empty();
		}},
		test_case{"canonical append sorts nested objects without changing its prefix", [] {
			std::string output = "prefix\n";
			const soml::value source = soml::object{{"z", soml::object{{"b", 2}, {"a", 1}}}, {"a", 0}};
			return soml::write_canonical(source, output) && output == "prefix\na: 0\nz: {\n\ta: 1\n\tb: 2\n}\n" && source.get_if<soml::object>()->begin()->key == "z";
		}},
		test_case{"canonical append can target a nested source string", [] {
			soml::value source = soml::object{{"z", "old"}, {"a", 1}};
			auto& output = *source.find("z")->get_if<std::string>();
			return soml::write_canonical(source, output) && output == "olda: 1\nz: 'old'\n";
		}},
		test_case{"append refuses a mutated duplicate key without changing its destination", [] {
			auto object = numbered_object(17);
			object.begin()[16].key = "0";
			std::string output = "keep";
			const auto result = soml::write_canonical(object, output);
			return !result && result.error().code == soml::error_code::invalid_value && output == "keep";
		}},
		test_case{"code frames retain exactly two context lines across decimal line widths", [] {
			std::string text;
			for (int index = 0; index < 9; index++) {
				text += "# context\n";
			}
			text += "a: +1";
			const auto parsed = soml::parse(text);
			return !parsed && parsed.error().code_frame(text) == "   8 | # context\n   9 | # context\n> 10 | a: +1\n     |    ^";
		}},
		test_case{"code frames count supplementary and combining scalars separately while retaining tabs", [] {
			const auto parsed = soml::parse("\t'😀é': +1");
			return !parsed && parsed.error().column == 9 && parsed.error().code_frame("\t'😀é': +1") == "> 1 | \t'😀é': +1\n    | \t       ^";
		}},
		test_case{"code frames show a final empty line at EOF", [] {
			const auto parsed = soml::parse("a:\n");
			return !parsed && parsed.error().code_frame("a:\n") == "  1 | a:\n> 2 |\n    | ^";
		}},
		test_case{"distance handles asymmetric lengths and empty strings", [] {
			return soml::detail::edit_distance("", "timeout") == 7 && soml::detail::edit_distance("timeout", "") == 7 && soml::detail::edit_distance("a", "aaaa") == 3 && soml::detail::edit_distance("aaaa", "a") == 3 && soml::detail::edit_distance("same", "same") == 0;
		}},
		test_case{"distance preserves adjacent transpositions and restricted overlapping swaps", [] {
			return soml::detail::edit_distance("port", "prot") == 1 && soml::detail::edit_distance("CA", "ABC") == 3 && soml::detail::edit_distance("abcdef", "abdcef") == 1 && soml::detail::edit_distance("abcdef", "badcfe") == 3;
		}},
		test_case{"distance remains symmetric for a deterministic string corpus", [] {
			std::mt19937 random{45};
			for (int iteration = 0; iteration < 500; iteration++) {
				std::string first(random() % 12, 'a');
				std::string second(random() % 12, 'a');
				for (char& character : first) {
					character = static_cast<char>('a' + random() % 4);
				}
				for (char& character : second) {
					character = static_cast<char>('a' + random() % 4);
				}
				if (soml::detail::edit_distance(first, second) != soml::detail::edit_distance(second, first)) {
					return false;
				}
			}
			return true;
		}},
		test_case{"inserting a large nested value preserves all data and siblings", [] {
			const soml::value replacement = numbered_object(128);
			const auto edited = soml::edit("parent: {keep: 9}", {"parent", "added"}, replacement);
			const soml::value expected = soml::object{{"parent", soml::object{{"keep", 9}, {"added", replacement}}}};
			return edited && soml::parse(*edited) == expected;
		}},
		test_case{"missing intermediate objects preserve full replacement ownership", [] {
			const soml::value replacement = soml::array{std::string(200, 'x'), soml::object{{"nested", 1}}};
			const auto edited = soml::edit("keep: true\n", {"new", "inner", "leaf"}, replacement);
			const soml::value expected = soml::object{{"keep", true}, {"new", soml::object{{"inner", soml::object{{"leaf", replacement}}}}}};
			return edited && soml::parse(*edited) == expected && replacement[0].as<std::string>()->size() == 200;
		}},
		test_case{"edit indentation preserves mixed spaces and tabs on every new line", [] {
			const auto edited = soml::edit("root: {\n \tleaf: 0\n}\n", {"root", "leaf"}, soml::object{{"items", soml::array{1, 2}}});
			return edited == "root: {\n \tleaf: {\n \t\titems: [\n \t\t\t1\n \t\t\t2\n \t\t]\n \t}\n}\n";
		}},
		test_case{"edit indentation applies to empty containers and escaped newlines correctly", [] {
			const auto edited = soml::edit("root: {\n\tleaf: 0 # keep\n}\n", {"root", "leaf"}, soml::array{soml::object{}, "a\nb", soml::array{}});
			return edited == "root: {\n\tleaf: [\n\t\t{}\n\t\t\"a\\nb\"\n\t\t[]\n\t] # keep\n}\n";
		}},
		test_case{"new quoted keys use canonical escaping without changing lookup", [] {
			const std::string key = "it's\n\"\\\t";
			const auto edited = soml::edit("{keep: 1}", {key}, 2);
			return edited == "{keep: 1, \"it's\\n\\\"\\\\\\t\": 2}" && soml::parse(*edited)->find(key)->as<int>() == 2;
		}},
		test_case{"new invalid keys report the same path as ordinary writing", [] {
			for (const std::string& key : {std::string{"bad\r"}, std::string{"bad\xff"}}) {
				const auto written = soml::write(soml::object{{key, nullptr}});
				const auto edited = soml::edit("{}", {key}, 1);
				if (written || edited || written.error() != edited.error()) {
					return false;
				}
			}
			return true;
		}},
		test_case{"inserting at maximum depth validates the combined nesting", [] {
			std::vector<soml::path_segment> path(soml::max_depth, std::string{"a"});
			const auto scalar = soml::edit("{}", path, 1);
			const auto container = soml::edit("{}", path, soml::array{});
			return scalar && soml::parse(*scalar).has_value() && !container && container.error().code == soml::error_code::too_deep;
		}},
		test_case{"editing every generated nested leaf preserves all unrelated values", [] {
			const soml::value source = soml::object{{"data", soml::array{numbered_object(17), numbered_object(17)}}, {"keep", "unchanged"}};
			const auto text = soml::write(source).value();
			for (std::size_t index = 0; index < 2; index++) {
				for (int key = 0; key < 17; key++) {
					const std::string name = std::to_string(key);
					const auto edited = soml::edit(text, {"data", index, name}, -key - 1);
					auto expected = source;
					*(*expected.find("data")->get_if<soml::array>())[index].find(name) = -key - 1;
					if (!edited || soml::parse(*edited) != expected) {
						return false;
					}
				}
			}
			return true;
		}},
		test_case{"formatting and editing agree on block strings with quoted delimiters in content", [] {
			const std::string source = "root: {\n  text: ''''\n  a ''' delimiter\n  ''''\n  keep: 1\n}\n";
			const auto formatted = soml::format(source);
			const auto edited = soml::edit(source, {"root", "keep"}, soml::array{2, 3});
			return formatted && soml::parse(*formatted) == soml::parse(source) && soml::format(*formatted) == formatted && edited && (*soml::parse(*edited))["root"]["text"].as<std::string>() == "a ''' delimiter";
		}},
		test_case{"long underscored floats preserve every digit and leave their source unchanged", [] {
			for (const std::string_view exponent : {"", "e-300", "e200"}) {
				const std::string plain = "1.234567890123456789" + std::string(2000, '0') + std::string{exponent};
				std::string separated;
				for (std::size_t index = 0; index < plain.size(); index++) {
					if (index > 0 && soml::detail::is_digit(plain[index - 1]) && soml::detail::is_digit(plain[index])) {
						separated += '_';
					}
					separated += plain[index];
				}
				const std::string text = "[" + separated + ", -" + separated + "]";
				const auto original = text;
				const auto expected = soml::parse("[" + plain + ", -" + plain + "]");
				const auto parsed = soml::parse(text);
				if (!expected || parsed != expected || text != original) {
					return false;
				}
			}
			return true;
		}},
		test_case{"literal source scanning preserves long strings and lookalike escapes", [] {
			for (const std::string& text : {std::string{}, std::string{"\\"}, std::string{"\\u{27}\"\t😀"}, std::string(100'000, 'x') + "\\"}) {
				const std::string source = "text: '" + text + "'\nkeep: 1\n";
				const std::string changed = "text: '" + text + "'\nkeep: 2\n";
				const auto formatted = soml::format(source);
				const auto edited = soml::edit(source, {"keep"}, 2);
				if (formatted != source || edited != changed || soml::parse(*edited)->find("text")->as<std::string>() != text) {
					return false;
				}
			}
			return true;
		}},
	};

	int failures = 0;
	for (const auto& test : cases) {
		if (!test.run()) {
			std::println(stderr, "FAIL: {}", test.name);
			failures++;
		}
	}
	std::println("{} distinct improvement cases, {} failures", cases.size(), failures);
	return failures == 0 ? 0 : 1;
}
