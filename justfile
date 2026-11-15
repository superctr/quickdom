# Copyright 2026 Ian Karlsson
# quickdom's everyday commands.

build_dir := "build"

# configure (once) and build the library and qdom-run
build:
	cmake -B {{build_dir}} -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build {{build_dir}}
