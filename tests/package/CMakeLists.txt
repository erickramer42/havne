# tests/package/CMakeLists.txt: an outside project using the installed
# core package (ADR-0035 §6), configured by tests/package_check.sh with
# CMAKE_PREFIX_PATH at the deb's /usr or an unpacked tarball. Not part
# of the main tree.
cmake_minimum_required(VERSION 3.21)
project(ecuio_consumer LANGUAGES C)

find_package(ecuio REQUIRED)
add_executable(consumer consumer.c)
target_link_libraries(consumer PRIVATE ecuio::core)
