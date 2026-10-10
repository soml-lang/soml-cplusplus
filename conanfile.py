import os

from conan import ConanFile
from conan.tools.files import copy


class SomlConan(ConanFile):
	name = "soml"
	version = "0.1.0"
	license = "MIT"
	description = "A strict C++26 reader and writer for SOML, a config format for humans"
	package_type = "header-library"
	exports_sources = "include/soml.hpp", "license"
	no_copy_source = True

	def package(self):
		copy(self, "soml.hpp", os.path.join(self.source_folder, "include"), os.path.join(self.package_folder, "include"))
		copy(self, "license", self.source_folder, os.path.join(self.package_folder, "licenses"))

	def package_info(self):
		self.cpp_info.bindirs = []
		self.cpp_info.libdirs = []
		self.cpp_info.set_property("cmake_file_name", "soml")
		self.cpp_info.set_property("cmake_target_name", "soml::soml")
