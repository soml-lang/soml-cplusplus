import os

from conan import ConanFile
from conan.tools.build import can_run
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout


class SomlTestConan(ConanFile):
	settings = "os", "compiler", "build_type", "arch"
	generators = "CMakeDeps"
	test_type = "explicit"

	def requirements(self):
		self.requires(self.tested_reference_str)

	def layout(self):
		cmake_layout(self)

	def generate(self):
		toolchain = CMakeToolchain(self)
		toolchain.user_presets_path = False
		toolchain.generate()

	def build(self):
		cmake = CMake(self)
		cmake.configure()
		cmake.build()

	def test(self):
		if can_run(self):
			self.run(os.path.join(self.cpp.build.bindir, "soml-package-test"), env="conanrun")
