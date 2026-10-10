#include <soml.hpp>

// A header consumer shares the same value type and linkage with a module consumer.
soml::value header_document() {
	return soml::parse("port: 8080").value();
}
