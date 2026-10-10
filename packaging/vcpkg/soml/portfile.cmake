# This overlay packages its working checkout. Disable binary caching and reinstall after changing the sources.
get_filename_component(SOURCE_PATH "${CURRENT_PORT_DIR}/../../.." ABSOLUTE)
set(VCPKG_BUILD_TYPE release)

vcpkg_cmake_configure(
	SOURCE_PATH "${CURRENT_PORT_DIR}"
	OPTIONS "-DSOML_SOURCE_DIR=${SOURCE_PATH}"
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME soml CONFIG_PATH lib/cmake/soml)
# The header-only package has no libraries after moving the CMake configuration.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/license")
