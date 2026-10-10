module;

#include <soml.hpp>

export module soml;

// Re-export the header's declarations without changing their linkage or duplicating their implementation.
export namespace soml {
	using soml::max_depth;
	using soml::error_code;
	using soml::error;
	using soml::type;
	using soml::to_string;
	using soml::date;
	using soml::duration;
	using soml::instant;
	using soml::array;
	using soml::object;
	using soml::member;
	using soml::value;
	using soml::view;
	using soml::parse;
	using soml::write;
	using soml::write_canonical;
	using soml::is_bare_key;
	using soml::format;
	using soml::path_segment;
	using soml::edit;
	using soml::decode_options;
	using soml::converter;
	using soml::fields;
	using soml::field;
	using soml::enum_names;
	using soml::from_value;
	using soml::to_value;
	using soml::decode;
	using soml::encode;
	using soml::encode_canonical;
	using soml::decodable;
	using soml::encodable;

#if defined(__cpp_impl_reflection)
	using soml::key_annotation;
	using soml::key;
	using soml::kebab_case_annotation;
	using soml::kebab_case;
#endif
}
