# SOML for C++

> A reader and writer for [SOML](https://soml.sh), a config format for humans, in one C++26 header

> [!NOTE]
> The format is a draft.

## Highlights

- One file, [`soml.hpp`](include/soml.hpp), with only standard-library dependencies
- Strict validation of duplicate keys, numeric and temporal ranges, UTF-8, control characters, and nesting
- Reference-compatible errors for every invalid [conformance case](tests/conformance), with code frames
- Exact int64 values, correctly rounded floats, calendar dates, and nanosecond instants and durations
- Typed decoding and encoding of structs, containers, `std::optional`, enums, and `std::chrono` types, with C++26 static reflection where the compiler has it
- Constant-expression support for checking configs with `static_assert`
- Errors return `std::expected`; allocation and size-limit failures can throw `std::bad_alloc` or `std::length_error`, and custom converter exceptions propagate
- Bounds-checked parsing with a 100-level nesting limit, tested with sanitizers, allocation failures, fuzzing, and differential checks

## Install

Copy [`include/soml.hpp`](include/soml.hpp) into your project, or use CMake:

```cmake
include(FetchContent)
FetchContent_Declare(soml GIT_REPOSITORY https://github.com/soml-lang/soml-cplusplus GIT_TAG main)
FetchContent_MakeAvailable(soml)
target_link_libraries(app PRIVATE soml::soml)
```

Requires C++26 (`-std=c++26` or `-std=c++2c`), a 64-bit target, and a standard library with floating-point `std::from_chars` and `std::to_chars` (macOS 26 or later on Apple platforms). CMake integration requires CMake 4.3 or later. Tested with Apple Clang 21 (Xcode 27), LLVM Clang 23, and GCC 16, using libc++ and libstdc++. Static reflection requires GCC 16 with `-freflection`.

### Named module

With CMake, Ninja, and a compiler with module scanning, enable `SOML_MODULE` before adding the library and link `soml::module`:

```cmake
set(SOML_MODULE ON CACHE BOOL "Build the SOML module")
FetchContent_MakeAvailable(soml)
target_link_libraries(app PRIVATE soml::module)
```

```cpp
#include <string>
import soml;

int main() {
	const auto document = soml::parse("port: 8080");
	return document && (*document)["port"].value_or(0) == 8080 ? 0 : 1;
}
```

Build with `cmake -G Ninja -S . -B build/modules -DSOML_MODULE=ON -DCMAKE_CXX_COMPILER=clang++`. Use LLVM Clang with `clang-scan-deps`, or GCC with module scanning support; tested with LLVM 23 and GCC 16. Apple Clang supports the header, but CMake cannot scan its modules. The library and consumers require C++26. Include standard headers before `import soml;`; GCC reflection also requires `#include <meta>` before importing and `-freflection` for both library and consumer.

With `SOML_INSTALL=ON`, installation exports both targets and the module source, letting CMake build the consumer's compiler-specific interface. Header and module translation units can be linked together; `soml::fields`, `soml::enum_names`, and `soml::converter` remain customizable.

### Package managers

Local recipes package the header target `soml::soml`; they are not published to a registry. Use CMake directly for the named module.

For Conan 2, create the package and run its consumer test:

```sh
conan profile detect
conan create . -s compiler.cppstd=26
```

Consumers require `soml/0.1.0`, select C++26, and use Conan's `CMakeDeps` and `CMakeToolchain` generators. In CMake, call `find_package(soml CONFIG REQUIRED)` and link `soml::soml`.

For vcpkg, run from this checkout:

```sh
vcpkg install soml --overlay-ports=packaging/vcpkg --no-binarycaching
```

Keep this overlay in its repository location: it packages the current checkout. After changing the source, remove and reinstall `soml`; keep binary caching disabled because the source lives outside the overlay port directory. Configure consumers with the vcpkg toolchain, call `find_package(soml CONFIG REQUIRED)`, and link `soml::soml` with a C++26 compiler.

## Usage

### Read

```cpp
#include <soml.hpp>

auto document = soml::parse(R"(
# The edge service.
name: 'api-gateway'
replicas: 3
launch-date: 2027-09-19
timeout: 1m30s
postgres: {host: 'db.internal', port: 5432}
)");

if (!document) {
	std::println(stderr, "{}", document.error());
	return 1;
}

std::println("{}", (*document)["name"].value_or("unnamed"));
//=> api-gateway

std::println("{}", (*document)["postgres"]["port"].value_or(0));
//=> 5432

std::println("{}", (*document)["timeout"].as<std::chrono::seconds>()->count());
//=> 90

std::println("{}", (*document)["missing"]["deeper"].value_or(8080));
//=> 8080
```

`soml::parse` also accepts `std::vector<char>`, reading its exact size without a terminating NUL.

The result is a `soml::value` holding an object or array. It owns its strings, so the input can be freed after parsing.

`value["key"]` and `value[index]` return a `soml::view`, empty for missing entries or mismatched types, so lookups chain without intermediate checks. `as<T>()` returns `std::optional<T>`; `value_or(fallback)` returns the value or fallback. Conversions are strict: ints and floats stay distinct, and `as<std::uint8_t>()` is empty for 300.

Views, pointers from `find` and `get_if`, and `as<std::string_view>()` borrow the owner's storage. Keep it alive and avoid replacing or moving referenced values or strings. Adding members or items can invalidate references when storage reallocates; erasing an element invalidates references to it and following elements.

`get_if<T>()` gives a pointer to the value as one of its types, or `nullptr`:

```cpp
const soml::value document = *soml::parse("a: 1\nb: [true, 'x']");

for (const auto& [key, item] : *document.get_if<soml::object>()) {
	std::println("{}: {}", key, item.type());
}
//=> a: int
//   b: array
```

Members are in the order they appear in the text.

### Write

```cpp
const soml::value document = soml::object{
	{"name", "api-gateway"},
	{"replicas", 3},
	{"timeout", soml::duration{90s}},
	{"labels", soml::array{"prod", "eu-west"}},
};

std::print("{}", *soml::write(document));
//=> name: 'api-gateway'
//   replicas: 3
//   timeout: 1m30s
//   labels: [
//   	'prod'
//   	'eu-west'
//   ]
```

`soml::write` preserves member order and otherwise follows canonical form: one entry per line, tabs, canonical spellings, and no commas or comments.

`soml::write_canonical` also sorts every object's keys, giving equal values identical bytes for hashing, signing, or comparison:

```cpp
std::print("{}", *soml::write_canonical(document));
//=> labels: [
//   	'prod'
//   	'eu-west'
//   ]
//   name: 'api-gateway'
//   replicas: 3
//   timeout: 1m30s
```

Both return `std::expected<std::string, soml::error>` and validate UTF-8, carriage returns, NaN, dates, instants, duplicate keys, nesting, and the collection root. Errors name the path, such as `servers[1].weight: NaN is not a SOML value`.

Pass a `soml::array` or `soml::object` directly to write it without copying it into a `soml::value` first. This also works with the append overloads.

Append with `soml::write(document, output)` or `soml::write_canonical(document, output)`, returning `std::expected<void, soml::error>`. Both render before appending, leaving the destination unchanged on invalid values or allocation failure:

```cpp
std::string output = "# Generated configuration\n";
auto appended = soml::write(document, output);
```

### Formatting and editing

`soml::format(text)` preserves comments, member order, literal spelling, and an unbraced root. It applies the spec's formatting rules: tabs, one space between tokens, no trailing whitespace, and one final LF. Inline containers stay inline.

```cpp
auto formatted = soml::format("port:   0x1F90  # HTTP\n");
//=> "port: 0x1F90 # HTTP\n"

auto changed = soml::edit("port: 8080 # HTTP\n", {"port"}, 9090);
//=> "port: 9090 # HTTP\n"

auto added = soml::edit("server: {host: 'localhost'}\n", {"server", "port"}, 8080);
//=> "server: {host: 'localhost', port: 8080}\n"

auto removed = soml::edit("a: 1\nb: 2\n", {"a"}, std::nullopt);
//=> "b: 2\n"
```

Both return `std::expected<std::string, soml::error>`. Editing changes the requested member or item and its owned comments and commas. New values use the writer's spelling and stay inline in an inline parent. Use `nullptr` to write SOML `null`, or `std::nullopt` to remove a value. Compare the result with the input to detect a change.

A path contains owning string keys and `std::size_t` indexes, such as `{"servers", std::size_t{0}, "port"}`. Pass a stored `std::vector<soml::path_segment>` or other contiguous range as a span. Missing members and intermediate objects are added. Setting an index at an array's size appends; larger indexes fail. Empty paths, keys on arrays, indexes on objects, and paths through scalars fail. Removing a missing value leaves the source unchanged.

Formatting and editing are runtime APIs. They validate input with the parser, then record source spans separately, so ordinary parsing incurs no layout cost.

### Structs and other types

```cpp
struct postgres {
	std::string host;
	std::uint16_t port = 5432;
};

struct config {
	std::string name;
	int replicas = 1;
	std::chrono::seconds timeout{30};
	std::vector<std::string> labels;
	postgres database;
};

auto result = soml::decode<config>(R"(
name: 'api-gateway'
timeout: 1m30s
labels: ['prod', 'eu-west']
database: {host: 'db.internal'}
)");

std::println("{} {} {} {}", result->replicas, result->timeout.count(), result->database.host, result->database.port);
//=> 1 90 db.internal 5432

std::print("{}", *soml::encode(*result));
//=> name: 'api-gateway'
//   replicas: 1
//   timeout: 1m30s
//   labels: [
//   	'prod'
//   	'eu-west'
//   ]
//   database: {
//   	host: 'db.internal'
//   	port: 5432
//   }
```

Missing members keep their defaults. Unknown keys are errors; pass `{.ignore_unknown_keys = true}` to skip them. Typo suggestions are best effort for keys up to 200 bytes, keeping diagnostics bounded for untrusted input. Values that do not fit also fail. Errors name the path and source position:

```cpp
std::println("{}", soml::decode<config>("database: {port: 70000}").error());
//=> database.port: The int 70000 is outside the range of the type, 0 to 65535 at line 1, column 18

std::println("{}", soml::decode<config>("replicas: 3\nreplica: 4").error());
//=> replica: Unknown key replica, which the type has no field for. Did you mean replicas? at line 2, column 1
```

`soml::encode_canonical` writes canonical form, and `soml::from_value` and `soml::to_value` convert between a type and a `soml::value`. Decoding from text requires a movable or copyable result; `from_value` also supports immovable results.

`from_value<T>(std::move(document))` and `to_value(std::move(source))` transfer owned strings and containers, including those nested in typed containers, optionals, and structs. `decode` transfers ownership from its temporary parsed document. `encode(std::move(source))` and `encode_canonical(std::move(source))` also allow ownership transfer during conversion. Lvalues and const rvalues are copied. Moved sources remain valid with unspecified values, including after a failed conversion; a native type mismatch leaves the source unchanged.

Rvalue encoding can also move values reached through reference members. Pass an lvalue when those values should retain their contents.

With GCC 16 and `-freflection`, structs need nothing more. Annotations change the keys:

```cpp
struct [[=soml::kebab_case]] server {
	int max_connections = 100; // Read from `max-connections`.
	[[=soml::key("log")]] level log_level = level::info;
};
```

Without static reflection, list the fields once:

```cpp
template<>
inline constexpr auto soml::fields<config> = std::tuple{
	soml::field{"name", &config::name},
	soml::field{"replicas", &config::replicas},
	soml::field{"timeout", &config::timeout},
	soml::field{"labels", &config::labels},
	soml::field{"database", &config::database},
};
```

Enums are read and written by name: from reflection, or from `soml::enum_names`:

```cpp
template<>
inline constexpr auto soml::enum_names<level> = std::array{
	std::pair{level::debug, "debug"sv},
	std::pair{level::info, "info"sv},
};
```

Any other type gets a `soml::converter` specialization with `from_value` and `to_value`. The built-in types:

| C++ | SOML |
|---|---|
| `bool` | bool |
| Every integer type, except the character types | int, range-checked both ways |
| `double`, `float`, `long double` | float, never an int |
| `std::nullptr_t` | null |
| `std::string` | string (`std::string_view` is written, and `value.as<std::string_view>()` reads one in place) |
| `soml::date` (`std::chrono::year_month_day`) | date, with strict calendar validation and years 0001 through 9999 |
| `std::chrono::duration` | duration, rounded to the nearest nanosecond with ties to even, then range-checked; built-in integer counts up to 64 bits |
| `std::chrono::sys_time` | instant, range-checked; built-in integer counts up to 64 bits; precision rules below |
| `soml::instant`, `soml::value`, `soml::object` | as themselves |
| `std::optional<T>` | null, or a missing member |
| `std::vector`, `std::deque`, `std::list`, `std::array` | array (a `std::array` needs exactly its size) |
| `std::map`, `std::unordered_map`, `std::flat_map` with string keys | object |
| Enums | string |
| Structs | object |

Sequence and map decoding require default-constructible, move-constructible containers with move-constructible elements. Mapped values must also be move-assignable. Map decoding rejects distinct SOML keys when the target map treats them as equivalent.

`soml::to_value(float)` and typed encoding use the float's shortest round-trip decimal, so `0.1f` writes as `0.1`. Direct `soml::value{0.1f}` construction preserves the exact widened binary value. Decoding into `float` uses ordinary C++ narrowing and may depend on the floating-point rounding mode. A wider `long double` decodes binary64 exactly and narrows when encoded: precision may be lost, finite magnitudes above `double`'s maximum and nonzero values that round to zero are rejected. NaN is rejected; infinities are supported.

A `soml::date` preserves its year, month, and day without conversion through an instant. Quoted date text stays a string. Native `year_month_day` values can be invalid; typed encoding and every writer reject invalid dates and years outside 0001 through 9999.

Integer representations for `std::chrono::duration` and `std::chrono::sys_time` must be built-in integer types of at most 64 bits. Custom periods whose nanosecond conversion ratios overflow `std::intmax_t` are unsupported. Decoding an integral duration count requires an exact conversion, so `1.5s` is an error for `std::chrono::seconds`. Floating duration counts may lose precision when decoding. Encoding rounds to the nearest nanosecond with ties to even, then checks the range. Floating counts are first scaled using `long double` arithmetic, which may lose precision. Constructing a `soml::value` directly accepts only `soml::duration`; use `soml::to_value` for other duration types so overflow is refused. Decoding an instant into an integral `sys_time` count must be exact; a floating count may lose precision. Encoding `sys_time` requires an integral count and floors subnanosecond times to nanoseconds, including negative times. Encoded durations and instants must fit their SOML ranges.

### Errors

```cpp
const std::string_view text = "name: 'api'\nport: 80 80\n";
const soml::error error = soml::parse(text).error();

std::println("{}", error);
//=> Expected a line break before the next entry, but found “8” at line 2, column 10

std::println("{}", error.code_frame(text));
//=>   1 | name: 'api'
//   > 2 | port: 80 80
//       |          ^
```

A `soml::error` has:

- `code`: `syntax` or `too_deep` when reading, `invalid_value` or `too_deep` when writing, `type_mismatch`, `out_of_range`, or `unknown_key` when decoding into a type, and `invalid_value` or `out_of_range` when encoding one.
- `message`: what is wrong, in the reference implementation's words.
- `offset`, `line`, and `column` for a parsing or `decode` error: the byte offset, and the 1-based line and column, with the column counted in Unicode scalar values. Decoding positions point to the value, or the key for an unknown key. They are 0 for `from_value` and writing errors, or unresolved or ambiguous paths.
- `path` for a writing or decoding error, such as `servers[1].port`.

`description()` gives the message with its path and position when available, which is also what `std::format` gives. C1 and bidirectional controls and Unicode line and paragraph separators appear as U+FFFD without changing the original `message` or `path`. `code_frame(text)` shows up to three lines with a caret; invalid UTF-8, controls other than tabs and line feeds, bidirectional marks, and Unicode line and paragraph separators appear as U+FFFD.

The first error in the text is reported, except that whole-input checks run first: invalid UTF-8, then a byte order mark, then a raw control character.

### Compile time

`soml::parse`, `soml::write`, `soml::write_canonical`, `soml::decode`, and `soml::encode` work in constant expressions. Their allocated values cannot escape the expression, but extracted scalars can:

```cpp
static_assert(soml::parse("port: 8080")->find("port")->as<int>() == 8080);
static_assert(!soml::parse("port: 0x1g"));
```

The conformance suite also runs in constant expressions. For documents near the 100-level nesting limit, pass `-fconstexpr-depth=4096`.

With `#embed`, a config file in the source tree can be checked when the program is built:

```cpp
constexpr char8_t config_text[] = {
#embed "config.soml"
};

static_assert(soml::parse(std::u8string_view{config_text, sizeof(config_text)}).has_value());
```

Use `char8_t` because `#embed` emits bytes from 0 to 255, which may not fit a `char`. For configs larger than a few kilobytes, raise the constexpr limit: `-fconstexpr-steps=100000000` with Clang or `-fconstexpr-ops-limit=1000000000` with GCC. Clang currently warns that C++ `#embed` is an extension.

## Types

| SOML | C++ |
|---|---|
| null | `std::nullptr_t` |
| bool | `bool` |
| int | `std::int64_t` |
| float | `double`, finite or infinite |
| string | `std::string`, UTF-8 |
| date | `soml::date`, an alias for `std::chrono::year_month_day`: Gregorian components, with no time or timezone |
| instant | `soml::instant`: `seconds` since 1970-01-01T00:00:00Z and `nanoseconds`, with conversions to and from `std::chrono::sys_time`, which cannot hold the years 0001 to 9999 in nanoseconds |
| duration | `soml::duration`, which is `std::chrono::nanoseconds` |
| array | `soml::array`, which is `std::vector<soml::value>` |
| object | `soml::object`: members in order, with `find`, `contains`, `operator[]`, `insert_or_assign`, and `erase` |

`soml::value` is a `std::variant` of these, in this order, which `variant()` gives for `std::visit`. Equality is the spec's: members in any order, and `0.0` equal to `-0.0`.

For large objects, move a `std::vector<soml::member>` into `soml::object` for O(n log n) construction. Duplicate keys keep their first position and last value, as with the initializer-list constructor. Individual `insert_or_assign` calls use linear lookup.

```cpp
std::vector<soml::member> members{{"host", "localhost"}, {"port", 8080}};
soml::object settings{std::move(members)};
```

## Untrusted input

Parsing bounds-checks input, limits nesting to 100 levels, and takes linear or n log n time and linear memory. Limit the size of untrusted files. Duplicate-key checks sort keys, avoiding hash flooding. A value uses 32 bytes with libc++ or 40 with libstdc++; a member uses 56 or 72. Long strings and containers may allocate separately. At the nesting limit, reading and writing use about 24 KB of stack in optimized builds or 130 KB in unoptimized builds.

A float below 10^-307 takes about 0.5 µs with libc++: the library converts it itself because libc++'s `std::from_chars` can take up to a millisecond.

Diagnostic messages display invisible and unassigned characters by their Unicode 17.0 code points. The character tables are regenerated with `python3 tools/generate-unicode.py` from checksum-verified Unicode data.

Word-sized scanning uses bounded `std::memcpy`; `reinterpret_cast` only views byte and UTF-8 buffers through `char`, which may alias their storage. Time conversions use checked 128-bit arithmetic. These operations are tested with sanitizers. With `-fno-exceptions`, standard-library allocation failure terminates instead of throwing.

## Limitations

- **A struct with a base class** is not read by static reflection, which would leave out the base's members. Give it `soml::fields`.
- **A `std::chrono::sys_time` with a floating-point count** can be read from an instant, but not written, because its value may not be a whole number of nanoseconds.
- **Object lookup is a linear scan**, which is the fastest choice for the small objects of a config file. Copy a huge object into a map to look it up often.
- **`-ffast-math`**: the compiler may then assume that no value is NaN, so the writer may not detect a NaN in a value built in code.

## Performance

Processor time per document on an Apple M4 Pro, Apple Clang 21 with `-O2`, for the shared [benchmark fixtures](https://github.com/soml-lang/soml/tree/main/benchmark), each with the same data in SOML, JSON, and TOML: `service` (3 KB), `platform` (35 KB, a quarter comments), and `earthquakes` (733 KB, instants and floats). Reading builds each library's tree, and writing indents it as a person would. simdjson reads into an immutable tape and has no such writer.

| | soml.hpp, SOML | [nlohmann/json](https://github.com/nlohmann/json), JSON | [simdjson](https://github.com/simdjson/simdjson), JSON | [toml++](https://github.com/marzer/tomlplusplus), TOML |
|---|---:|---:|---:|---:|
| Read service | 7.6 µs | 14.8 µs | 925 ns | 40.0 µs |
| Write service | 4.0 µs | 7.7 µs | – | 13.5 µs |
| Read platform | 54.4 µs | 103.8 µs | 5.9 µs | 382.2 µs |
| Write platform | 23.4 µs | 51.3 µs | – | 82.6 µs |
| Read earthquakes | 2.07 ms | 3.91 ms | 262.2 µs | 10.55 ms |
| Write earthquakes | 1.57 ms | 2.03 ms | – | 5.42 ms |

Measured on 2026-10-27. Each result is the fastest of five runs of at least 100 ms of processor time.

Reading also checks every rule of the spec, such as UTF-8, duplicate keys, and the ranges of ints, floats, dates, instants, and durations. Run it with `cmake -S . -B build/bench -DSOML_BENCH_COMPARE=ON && cmake --build build/bench --target soml-bench && build/bench/soml-bench`, which downloads the other libraries at pinned releases and checks them by hash.

The benchmark also decodes complete service and earthquake fixtures into nested structs with owning strings and vectors, durations, instants, and optional members. It verifies every source value before timing and uses `soml::fields`, requiring no reflection:

| Fixture | Parse and decode | Convert an already parsed value | Struct to value |
|---|---:|---:|---:|
| service | 9.1 µs | 1.6 µs | 1.9 µs |
| earthquakes | 2.73 ms | 763.2 µs | 427.0 µs |

Editing replaces one array item with a complete fixture, preserving the other item. The benchmark validates the result before timing:

| Fixture | Inline parent | Multiline parent |
|---|---:|---:|
| service | 3.8 µs | 5.0 µs |
| platform | 20.0 µs | 26.7 µs |
| earthquakes | 1.45 ms | 1.71 ms |

To benchmark without downloading comparison libraries, use `-DSOML_BENCH=ON`.

The standalone benchmark also validates and times integer arrays, whole-float arrays, duplicate-heavy object batches, ordered object equality, key distance, ASCII string classification after an early quote, and code-frame windows on long lines. Bulk object construction compacts duplicates in place, saving an allocation while retaining the input vector's capacity.

| Operation | Time |
|---|---:|
| Write 4096 integers | 49.5 µs |
| Write 4096 whole floats | 85.8 µs |
| Compact 4096 members to 128 | 288.8 µs |
| Compare 128 ordered members | 490 ns |
| Compare 126 matching members and a reordered suffix | 505 ns |
| Key distance with a transposition | 27 ns |
| Classify 1 MB after an early quote | 64.8 µs |
| Frame the start of a 1 MB line | 478 ns |

The hot paths skip already checked work: scanners jump directly to the first special byte of each word, short exponent floats use bounded arithmetic, and ordered objects avoid sorting and scratch allocations. Typed decoding moves owned strings and containers out of its temporary document, and optional reads avoid constructing errors that would be discarded.

## Development

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

The [CI workflow](.github/workflows/test.yml) tests Apple Clang, LLVM, GCC reflection, sanitizers, installed header/module consumers, and Conan/vcpkg recipes on the Xcode 27 runner.

Tests cover the APIs, README examples, each allocation failing in turn, and conformance (requires Node.js). The `improvements` suite adds deterministic generated cases for integer rendering, batch construction, object equality, key distance, and nested edits, alongside focused ownership, Unicode, and formatting checks. The `hardening` suite checks malformed UTF-8, conversion identities, unusual edit keys, temporal boundaries, and diagnostic controls. The `refinement` suite checks exact diagnostic windows, mixed valid and invalid UTF-8 escaping, fractional spellings, enum bounds, and bounded typo suggestions. Allocation tests also verify storage reuse and allocation budgets. Other configurations:

```sh
# AddressSanitizer, UndefinedBehaviorSanitizer, and the standard library's hardening
cmake -S . -B build/sanitize -DCMAKE_BUILD_TYPE=Debug -DSOML_SANITIZE=ON

# GCC, with static reflection
cmake -S . -B build/gcc -DCMAKE_CXX_COMPILER=g++-16

# libFuzzer, with Homebrew's LLVM on macOS
cmake -S . -B build/fuzz -DSOML_FUZZ=ON -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ && cmake --build build/fuzz --target soml-fuzz
mkdir -p build/fuzz/corpus && find tests/conformance -name '*.soml' | while read -r file; do cp "$file" "build/fuzz/corpus/$(echo "$file" | tr / _)"; done
build/fuzz/soml-fuzz -dict=fuzz/soml.dict -max_len=4096 build/fuzz/corpus

# Differential testing against ../soml-javascript (needs Node.js 26 or later)
SOML_TAGGED=build/soml-tagged node tests/differential.js 20000 1
```

## Conformance suite

[`tests/conformance`](tests/conformance) is a copy of the suite in the [`soml` spec repository](https://github.com/soml-lang/soml/tree/main/conformance):

- `valid/**/name.soml` must parse to the value in `name.json`, and serialize with `soml::write_canonical` to exactly `name.canonical.soml`.
- `invalid/**/name.soml` must be rejected, at the line and column, and with the message, in `tests/conformance/invalid-reasons.json`.
- `valid/**/name.formatted.soml` must match `soml::format` exactly; formatting must preserve values and be idempotent.
- `edit/` checks exact edited text and invalid paths through `soml::edit`.

To update the copy, check out the spec repository next to this one and run `../soml/sync-conformance.sh tests/conformance`.
