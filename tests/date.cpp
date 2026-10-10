#include <soml.hpp>

#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

constexpr bool constexpr_calendar_date() {
	const soml::date expected{std::chrono::year{1582}, std::chrono::month{10}, std::chrono::day{10}};
	const auto parsed = soml::parse("[1582-10-10]");
	if (!parsed || (*parsed)[0].as<soml::date>() != expected) {
		return false;
	}
	const auto written = soml::write_canonical(*parsed);
	return written && *written == "[\n\t1582-10-10\n]\n";
}

static_assert(constexpr_calendar_date());

int main() {
	int failures = 0;
	const auto check = [&](bool condition) {
		if (!condition) {
			failures++;
		}
	};

	for (const std::string text : {"0001-01-01", "9999-12-31", "1582-10-10", "2011-12-30", "2000-02-29", "2026-11-01"}) {
		const auto parsed = soml::parse("day: " + text + "# keep\ntext: '" + text + "'");
		check(parsed.has_value());
		if (!parsed) {
			continue;
		}
		const auto native = (*parsed)["day"].as<soml::date>();
		check(native.has_value());
		check((*parsed)["day"].type() == soml::type::date);
		check(!(*parsed)["text"].as<soml::date>());
		check((*parsed)["text"].as<std::string>() == text);
		check(soml::write_canonical(*parsed) == "day: " + text + "\ntext: '" + text + "'\n");
		const auto typed = soml::decode<std::vector<soml::date>>("[" + text + "]");
		check(typed && typed->size() == 1 && typed->front() == native);
		if (native) {
			check(soml::encode(std::vector{*native}) == "[\n\t" + text + "\n]\n");
		}
	}

	for (const std::string text : {"1900-02-29", "2026-02-30", "0000-01-01", "2026-1-01", "10000-01-01", "2026-11-01T", "2026-11-01Z", "2026-11-01foo", "2026-11-01T12:00:00"}) {
		check(!soml::parse("day: " + text));
	}
	for (const std::string text : {"2026-11-01 12:00:00Z", "2026-11-01\t12:00:00Z", "2026-11-01  12:00:00Z", "2026-11-01 4:00:00Z"}) {
		const auto parsed = soml::parse("day: " + text);
		check(!parsed && parsed.error().message.starts_with("The date and time separator"));
	}
	check(!soml::decode<std::vector<soml::date>>("['2026-11-01']"));
	check(!soml::decode<std::vector<std::string>>("[2026-11-01]"));

	for (const auto date : {soml::date{}, soml::date{std::chrono::year{-1}, std::chrono::month{1}, std::chrono::day{1}}, soml::date{std::chrono::year{0}, std::chrono::month{1}, std::chrono::day{1}}, soml::date{std::chrono::year{10000}, std::chrono::month{1}, std::chrono::day{1}}, soml::date{std::chrono::year{1900}, std::chrono::month{2}, std::chrono::day{29}}}) {
		check(!soml::to_value(date));
		const auto written = soml::write_canonical(soml::array{soml::value{date}});
		check(!written && written.error().code == soml::error_code::invalid_value && written.error().path == "[0]");
	}
	const auto mismatch = soml::decode<std::vector<int>>("[1582-10-10]");
	check(!mismatch && mismatch.error().message == "Expected an int, but found the date 1582-10-10");

	// Every month-end candidate over a complete Gregorian leap-year cycle, including invalid slots.
	for (int year = 1; year <= 400; year++) {
		for (unsigned month = 1; month <= 12; month++) {
			const unsigned last_day = month == 2 ? (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0) ? 29U : 28U) : (month == 4 || month == 6 || month == 9 || month == 11 ? 30U : 31U);
			for (unsigned day = 28; day <= 32; day++) {
				const soml::date calendar{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
				const std::string text = std::format("{:04}-{:02}-{:02}", year, month, day);
				const auto parsed = soml::parse("[" + text + "]");
				check(parsed.has_value() == (day <= last_day));
				check(soml::to_value(calendar).has_value() == (day <= last_day));
				if (parsed) {
					check((*parsed)[0].as<soml::date>() == calendar);
					check(soml::write_canonical(*parsed) == "[\n\t" + text + "\n]\n");
				}
			}
		}
	}

	const std::string source = "day: 1582-10-10 # keep\ntext: '1582-10-10'\n";
	const soml::date calendar{std::chrono::year{2000}, std::chrono::month{2}, std::chrono::day{29}};
	check(soml::edit(source, {"day"}, calendar) == "day: 2000-02-29 # keep\ntext: '1582-10-10'\n");
	check(soml::format("[1582-10-10,2011-12-30/* keep */]") == "[1582-10-10, 2011-12-30 /* keep */]\n");
	const auto typed = soml::decode<std::map<std::string, std::optional<soml::date>>>("day: 2000-02-29\nnone: null");
	check(typed && typed->at("day") == calendar && !typed->at("none"));
	check(!soml::parse("day: 2026-11-01" + std::string(100'000, 'x')));
	return failures == 0 ? 0 : 1;
}
