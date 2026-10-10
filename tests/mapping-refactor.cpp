#include <soml.hpp>

#include <map>
#include <print>
#include <cfenv>
#include <cmath>

namespace {

struct configuration {
	std::string name;
	std::vector<std::optional<std::string>> labels;
	soml::array consumed;
	std::vector<int> numbers;
};

struct wrapped {
	int number;
};

struct readonly_sequence {
	using value_type = std::string;
	std::vector<std::string> items;

	auto begin() const {
		return items.begin();
	}

	auto end() const {
		return items.end();
	}

	void push_back(std::string item) {
		items.push_back(std::move(item));
	}
};

enum class mode { first, second };

#if defined(__cpp_impl_reflection)
enum class empty_mode {};
static_assert(!soml::value{"unknown"}.as<empty_mode>());
static_assert(!soml::to_value(static_cast<empty_mode>(0)));
#endif

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

} // namespace

template<>
inline constexpr auto soml::fields<configuration> = std::tuple{
	soml::field{"name", &configuration::name},
	soml::field{"labels", &configuration::labels},
	soml::field{"consumed", &configuration::consumed},
	soml::field{"numbers", &configuration::numbers},
};

template<>
inline constexpr auto soml::enum_names<mode> = std::array{
	std::pair{mode::first, std::string_view{"first"}},
	std::pair{mode::first, std::string_view{"alias"}},
	std::pair{mode::second, std::string_view{"second"}},
};

template<>
struct soml::converter<wrapped> {
	static constexpr std::expected<wrapped, soml::error> from_value(const soml::value& source, const soml::decode_options& options) {
		return soml::from_value<int>(source, options).transform([](int number) {
			return wrapped{number};
		});
	}
};

namespace {

void transfers(test_results& results) {
	const std::string text(256, 'x');
	{
		soml::value source = soml::array{text};
		const char* pointer = (*source.get_if<soml::array>())[0].get_if<std::string>()->data();
		const auto converted = soml::from_value<std::vector<std::string>>(std::move(source));
		results.check("dynamic sequences transfer their owned string allocations", converted && converted->front() == text && converted->front().data() == pointer);
	}
	{
		soml::value source = soml::array{text};
		const char* pointer = (*source.get_if<soml::array>())[0].get_if<std::string>()->data();
		const auto converted = soml::from_value<std::array<std::string, 1>>(std::move(source));
		results.check("fixed arrays transfer their owned string allocations", converted && converted->front() == text && converted->front().data() == pointer);
	}
	{
		soml::value source = soml::object{{"key", text}};
		const char* pointer = source.find("key")->get_if<std::string>()->data();
		const auto converted = soml::from_value<std::map<std::string, std::string>>(std::move(source));
		results.check("maps transfer their owned mapped strings", converted && converted->at("key") == text && converted->at("key").data() == pointer);
	}
	{
		soml::value source = text;
		const char* pointer = source.get_if<std::string>()->data();
		const auto converted = soml::from_value<std::optional<std::string>>(std::move(source));
		results.check("optionals transfer their owned strings", converted && *converted && **converted == text && (*converted)->data() == pointer);
	}
	{
		soml::value source = soml::object{{"name", text}, {"labels", soml::array{text, nullptr}}};
		const char* name_pointer = source.find("name")->get_if<std::string>()->data();
		const char* label_pointer = (*source.find("labels")->get_if<soml::array>())[0].get_if<std::string>()->data();
		const auto converted = soml::from_value<configuration>(std::move(source));
		results.check("structs transfer owned strings through nested optional sequences", converted && converted->name == text && converted->name.data() == name_pointer && converted->labels.size() == 2 && converted->labels[0]->data() == label_pointer && !converted->labels[1]);
	}
	{
		const soml::value source = soml::array{text};
		const auto original = source;
		const auto converted = soml::from_value<std::vector<std::string>>(source);
		const auto const_rvalue = soml::from_value<std::vector<std::string>>(std::move(source));
		results.check("const and lvalue conversions retain their source ownership", source == original && converted && const_rvalue == converted && converted->front().data() != source.get_if<soml::array>()->front().get_if<std::string>()->data());
	}
	const auto custom = soml::decode<std::vector<wrapped>>("[1, 2]");
	results.check("custom converters accepting a const reference remain supported", custom && custom->size() == 2 && custom->at(1).number == 2);
}

void failure_positions(test_results& results) {
	for (const std::string_view tail : {"numbers: [1, 'bad']", "unexpected: {a: [true]}", "labels: ['ok', 3]"}) {
		const std::string text = "name: 'a string whose allocation will be transferred'\nconsumed: [{large: [1, 2, 3]}, 'also moved']\n" + std::string{tail};
		const auto parsed = soml::parse(text);
		const auto const_failure = soml::from_value<configuration>(*parsed);
		const auto decoded = soml::decode<configuration>(text);
		results.check("failed decoding retains paths after earlier values were consumed", !const_failure && !decoded && decoded.error().code == const_failure.error().code && decoded.error().message == const_failure.error().message && decoded.error().path == const_failure.error().path && decoded.error().line == 3 && decoded.error().column > 0);
	}
}

void encodes_by_moving(test_results& results) {
	const std::string text(256, 'y');
	{
		std::vector<std::string> source{text};
		const char* pointer = source.front().data();
		const auto converted = soml::to_value(std::move(source));
		results.check("encoding dynamic sequences transfers string allocations", converted && (*converted)[0].as<std::string_view>()->data() == pointer);
	}
	{
		std::array<std::string, 1> source{text};
		const char* pointer = source.front().data();
		const auto converted = soml::to_value(std::move(source));
		results.check("encoding fixed arrays transfers string allocations", converted && (*converted)[0].as<std::string_view>()->data() == pointer);
	}
	{
		std::map<std::string, std::string> source{{"key", text}};
		const char* pointer = source.at("key").data();
		const auto converted = soml::to_value(std::move(source));
		results.check("encoding maps transfers their mapped string allocations", converted && (*converted)["key"].as<std::string_view>()->data() == pointer);
	}
	{
		std::optional<std::string> source{text};
		const char* pointer = source->data();
		const auto converted = soml::to_value(std::move(source));
		results.check("encoding optionals transfers string allocations", converted && converted->as<std::string_view>()->data() == pointer);
	}
	{
		configuration source{.name = text, .labels = {text, std::nullopt}, .consumed = {}, .numbers = {1, 2}};
		const char* name_pointer = source.name.data();
		const char* label_pointer = source.labels.front()->data();
		const auto converted = soml::to_value(std::move(source));
		results.check("encoding structs forwards ownership through nested fields", converted && (*converted)["name"].as<std::string_view>()->data() == name_pointer && (*converted)["labels"][0].as<std::string_view>()->data() == label_pointer);
	}
	const configuration source{.name = text, .labels = {text}, .consumed = {}, .numbers = {1, 2}};
	const auto expected = soml::encode(source);
	auto movable = source;
	results.check("rvalue encoding preserves ordinary output", soml::encode(std::move(movable)) == expected);
	movable = source;
	results.check("rvalue encoding preserves canonical output", soml::encode_canonical(std::move(movable)) == soml::encode_canonical(source));
	const auto converted = soml::to_value(source);
	results.check("encoding lvalues still retains the source allocations", converted && source.name == text && (*converted)["name"].as<std::string_view>()->data() != source.name.data());
}

void optional_reads(test_results& results) {
	const std::array<soml::value, 8> sources{nullptr, true, 3, 1.5, "alias", "unknown", soml::duration{1}, soml::instant{0, 1}};
	for (const auto& source : sources) {
		const auto check = [&]<class T>() {
			const auto optional = source.as<T>();
			const auto converted = soml::from_value<T>(source);
			results.check("optional reads agree with checked conversions", optional.has_value() == converted.has_value() && (!optional || *optional == *converted));
		};
		check.template operator()<mode>();
		check.template operator()<std::chrono::seconds>();
		check.template operator()<std::chrono::duration<std::uint8_t, std::nano>>();
		check.template operator()<std::chrono::sys_seconds>();
		check.template operator()<std::optional<mode>>();
		check.template operator()<std::optional<std::optional<std::chrono::seconds>>>();
	}
	results.check("enum aliases keep their canonical encoding name", soml::to_value(mode::first)->as<std::string_view>() == "first");
	const auto failure = soml::from_value<mode>(soml::value{"other"});
	results.check("enum lists preserve all names in metadata order", !failure && failure.error().message == "Unknown value \"other\". Expected one of: first, alias, second");
}

void rounding_and_readonly_sequences(test_results& results) {
	readonly_sequence sequence{{std::string(256, 'z')}};
	const auto expected = soml::to_value(sequence);
	results.check("rvalue sequences can expose read-only elements", soml::to_value(std::move(sequence)) == expected && sequence.items.front() == std::string(256, 'z'));
	const int original_rounding = std::fegetround();
	std::fesetround(FE_TONEAREST);
	for (const float number : {0.1F, -0.3F, 1.234567F, std::numeric_limits<float>::min(), std::numeric_limits<float>::max(), std::numeric_limits<float>::denorm_min()}) {
		const double nearest = *soml::to_value(number)->get_if<double>();
		for (const int rounding : {FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO, FE_TONEAREST}) {
			std::fesetround(rounding);
			const double converted = *soml::to_value(number)->get_if<double>();
			results.check("shortest float encoding is independent of the floating-point rounding mode", std::bit_cast<std::uint64_t>(converted) == std::bit_cast<std::uint64_t>(nearest));
		}
	}
	std::fesetround(original_rounding);
}

void duration_rounding(test_results& results) {
	using picoseconds = std::chrono::duration<std::int64_t, std::pico>;
	using thirds = std::chrono::duration<std::int64_t, std::ratio<1, 3'000'000'000>>;
	for (const std::int64_t count : {-3'500, -2'501, -2'500, -2'499, -1'500, -501, -500, -499, -1, 0, 1, 499, 500, 501, 1'500, 2'499, 2'500, 2'501, 3'500}) {
		const auto expected = std::chrono::round<std::chrono::nanoseconds>(picoseconds{count});
		results.check("integer picoseconds round to the nearest nanosecond with ties to even", soml::to_value(picoseconds{count}) == soml::value{expected});
		results.check("floating nanoseconds use the same ties-to-even rule", soml::to_value(std::chrono::duration<double, std::nano>{static_cast<double>(count) / 1000}) == soml::value{expected});
	}
	for (const std::int64_t count : {-5, -4, -2, -1, 0, 1, 2, 4, 5}) {
		results.check("an odd conversion denominator rounds to the nearest whole nanosecond", soml::to_value(thirds{count}) == soml::value{std::chrono::round<std::chrono::nanoseconds>(thirds{count})});
	}
	using three_halves = std::chrono::duration<std::int64_t, std::ratio<3, 2'000'000'000>>;
	results.check("rounding at the lower limit retains its even count", soml::to_value(three_halves{-6'148'914'691'236'517'205}) == soml::value{soml::duration::min()});
	results.check("rounding above the odd upper limit fails instead of saturating", !soml::to_value(three_halves{6'148'914'691'236'517'205}));
}

void native_converters(test_results& results) {
	const std::string text(256, 'n');
	soml::value source = text;
	const char* pointer = source.get_if<std::string>()->data();
	const auto moved = soml::converter<std::string>::from_value(std::move(source), {});
	results.check("native converters own their move behavior", moved && *moved == text && moved->data() == pointer);
	source = text;
	const auto before = source;
	const auto mismatch = soml::from_value<soml::array>(std::move(source));
	results.check("native mismatch leaves source unchanged", !mismatch && source == before);
	const soml::value constant = text;
	const auto copied = soml::converter<soml::value>::from_value(std::move(constant), {});
	results.check("native converters copy const rvalues", copied && *copied == constant && copied->get_if<std::string>()->data() != constant.get_if<std::string>()->data());
	std::string string = text;
	pointer = string.data();
	const auto encoded = soml::converter<std::string>::to_value(std::move(string));
	results.check("native converter encoding transfers ownership", encoded && encoded->get_if<std::string>()->data() == pointer);
}

void arithmetic_and_diagnostics(test_results& results) {
	for (std::int64_t number = -10'000; number <= 10'000; number++) {
		for (const std::int64_t denominator : {1, 2, 3, 1000}) {
			results.check("chrono floor division matches mathematical floor", soml::detail::floor_divide(number, denominator) == static_cast<soml::detail::int128>(std::floor(static_cast<double>(number) / static_cast<double>(denominator))));
		}
	}
	const auto ranges = [&]<std::integral Integer>() {
		const auto minimum = static_cast<soml::detail::int128>(std::numeric_limits<Integer>::min());
		const auto maximum = static_cast<soml::detail::int128>(std::numeric_limits<Integer>::max());
		results.check("128-bit range checks retain both endpoints and reject their neighbors", soml::detail::fits_integer<Integer>(minimum) && soml::detail::fits_integer<Integer>(maximum) && !soml::detail::fits_integer<Integer>(minimum - 1) && !soml::detail::fits_integer<Integer>(maximum + 1));
	};
	ranges.template operator()<std::int8_t>();
	ranges.template operator()<std::uint8_t>();
	ranges.template operator()<std::int16_t>();
	ranges.template operator()<std::uint16_t>();
	ranges.template operator()<std::int32_t>();
	ranges.template operator()<std::uint32_t>();
	ranges.template operator()<std::int64_t>();
	ranges.template operator()<std::uint64_t>();
	for (char32_t scalar = 0; scalar < 128; scalar++) {
		const bool expected = std::ranges::any_of(soml::detail::diagnostic_code_point_ranges, [scalar](const auto& range) {
			return scalar >= range.first && scalar <= range.second;
		});
		results.check("ASCII diagnostic properties agree with the Unicode table", soml::detail::is_shown_by_code_point(scalar) == expected && soml::detail::character_name(scalar).empty());
	}
	for (const std::size_t index : {std::size_t{0}, std::size_t{10}, std::numeric_limits<std::size_t>::max()}) {
		results.check("array paths preserve full-width indices", soml::detail::path_index(index) == std::format("[{}]", index));
	}
	const std::array<std::pair<soml::value, std::string_view>, 15> descriptions{{
		{nullptr, "null"}, {true, "the bool true"}, {-7, "the int -7"}, {1.25, "the float 1.25"}, {"text", "the string 'text'"},
		{soml::date{std::chrono::year{2026}, std::chrono::month{11}, std::chrono::day{1}}, "the date 2026-11-01"},
		{soml::duration{1}, "the duration 0.000000001s"}, {soml::instant{0, 1}, "the instant 1970-01-01T00:00:00.000000001Z"}, {soml::array{}, "an array"}, {soml::object{}, "an object"},
		{std::string{"\xFF"}, "the string \"\\u00ff\""}, {std::string{"\r"}, "the string \"\\r\""}, {std::numeric_limits<double>::quiet_NaN(), "NaN"}, {soml::instant{0, -1}, "an invalid instant"}, {soml::date{std::chrono::year{1900}, std::chrono::month{2}, std::chrono::day{29}}, "an invalid date"},
	}};
	for (const auto& [source, expected] : descriptions) {
		results.check("type mismatch descriptions retain their precise spelling", soml::detail::describe_value(source) == expected);
	}
	const auto failure = soml::detail::make_error(soml::error_code::out_of_range, "outside");
	results.check("constructed errors inherit the type's unset position defaults", failure.code == soml::error_code::out_of_range && failure.message == "outside" && failure.offset == 0 && failure.line == 0 && failure.column == 0 && failure.path.empty());
}

static_assert(std::constructible_from<soml::value, soml::duration>);
static_assert(!std::constructible_from<soml::value, std::chrono::seconds>);
static_assert(!std::constructible_from<soml::value, std::chrono::duration<std::int64_t, std::pico>>);
static_assert(soml::decode<std::vector<std::string>>("['one', 'two']")->at(1) == "two");
static_assert(soml::decode<std::array<std::optional<std::string>, 2>>("['one', null]")->at(0) == "one");
static_assert((*soml::to_value(std::vector<bool>{true, false}))[1].as<bool>() == false);

} // namespace

int main() {
	test_results results;
	transfers(results);
	failure_positions(results);
	encodes_by_moving(results);
	optional_reads(results);
	rounding_and_readonly_sequences(results);
	duration_rounding(results);
	native_converters(results);
	arithmetic_and_diagnostics(results);
	std::println("{} mapping refactor checks, {} failures", results.count, results.failures);
	return results.failures == 0 ? 0 : 1;
}
