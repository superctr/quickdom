# Copyright 2026 Ian Karlsson
# quickdom: a DOM for QuickJS. The headless core (DOM, styles, layout, text, paint commands) is the library
# `quickdom`; the OpenGL backend and the PUGL window are optional libraries beside it.
cmake_minimum_required(VERSION 3.23)
project(quickdom VERSION 0.1.0 LANGUAGES C CXX)

option(QUICKDOM_FONTS "Build the default fonts (Noto Sans, DejaVu) into the library" ON)
option(QUICKDOM_TOOLS "Build qdom-run, the headless runner" ${PROJECT_IS_TOP_LEVEL})
option(QUICKDOM_GL "Build quickdom_gl, the nanovg/OpenGL 2.1 backend (needs the system's GL)" ${PROJECT_IS_TOP_LEVEL})
option(QUICKDOM_PUGL "Build quickdom_pugl, the PUGL window (needs quickdom_gl; X11 on Linux)" ${PROJECT_IS_TOP_LEVEL})
set(QUICKDOM_QUICKJS_TARGET "" CACHE STRING "A QuickJS-ng library target of the host's (default: qjs if it exists, else thirdparty/quickjs)")
set(QUICKDOM_NANOVG_TARGET "" CACHE STRING "A nanovg library target of the host's (default: thirdparty/nanovg)")
set(QUICKDOM_PUGL_TARGET "" CACHE STRING "A PUGL library target of the host's, with its GL backend (default: thirdparty/pugl)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# for a shared object (a plugin) that links these libraries statically
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
	set(QUICKDOM_WARNINGS /W4 /wd4100)
else()
	set(QUICKDOM_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# QuickJS-ng: the host's, when it has one, so that one engine is linked
if(QUICKDOM_QUICKJS_TARGET)
	set(QUICKDOM_QJS ${QUICKDOM_QUICKJS_TARGET})
elseif(TARGET qjs)
	set(QUICKDOM_QJS qjs)
else()
	set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
	add_subdirectory(thirdparty/quickjs EXCLUDE_FROM_ALL)
	set(QUICKDOM_QJS qjs)
	if(NOT MSVC)
		target_compile_options(qjs PRIVATE -w)
	endif()
endif()

# files built into the library as byte arrays: the JavaScript prelude, the default fonts
function(quickdom_embed output name base)
	set(files ${ARGN})
	set(paths ${files})
	list(TRANSFORM paths PREPEND ${base}/)
	string(REPLACE ";" "," list "${files}")
	add_custom_command(OUTPUT ${output}
		COMMAND ${CMAKE_COMMAND} -DBASE=${base} "-DFILES=${list}" -DNAME=${name} -DOUTPUT=${output}
		        -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
		DEPENDS ${paths} ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
		COMMENT "embedding ${name}")
endfunction()

set(QUICKDOM_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated)
quickdom_embed(${QUICKDOM_GENERATED}/prelude.c qdom_prelude ${PROJECT_SOURCE_DIR}/src/js prelude.js)
set(QUICKDOM_FONT_FILES NotoSans-Regular.ttf NotoSans-Bold.ttf NotoSans-Italic.ttf NotoSans-BoldItalic.ttf
	DejaVuSans.ttf DejaVuSans-Bold.ttf DejaVuSansMono.ttf DejaVuSansMono-Bold.ttf
	NotoSansSymbols2-Regular.ttf NotoColorEmoji.ttf)
if(QUICKDOM_FONTS)
	quickdom_embed(${QUICKDOM_GENERATED}/fonts.c qdom_fonts ${PROJECT_SOURCE_DIR}/fonts ${QUICKDOM_FONT_FILES})
	set(QUICKDOM_FONT_SOURCES ${QUICKDOM_GENERATED}/fonts.c)
else()
	set(QUICKDOM_FONT_SOURCES src/text/no_fonts.c)
endif()

add_library(quickdom STATIC
	src/core/names.cpp
	src/css/values.cpp
	src/css/properties.cpp
	src/css/selectors.cpp
	src/css/stylesheet.cpp
	src/css/style.cpp
	src/dom/node.cpp
	src/dom/events.cpp
	src/dom/dom.cpp
	src/text/fonts.cpp
	src/layout/layout.cpp
	src/layout/flex.cpp
	src/layout/grid.cpp
	src/layout/table.cpp
	src/layout/inline.cpp
	src/paint/paint.cpp
	src/paint/picture.cpp
	src/input/field.cpp
	src/input/input.cpp
	src/js/bindings.cpp
	src/js/bindings_node.cpp
	src/js/bindings_window.cpp
	src/js/bindings_canvas.cpp
	src/quickdom.cpp
	${QUICKDOM_GENERATED}/prelude.c
	${QUICKDOM_FONT_SOURCES})
target_include_directories(quickdom
	PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
	PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/thirdparty/stb)
# nanovg's stb_image for <img>'s pictures, its header only
set_source_files_properties(src/paint/picture.cpp PROPERTIES INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/thirdparty/nanovg/src)
target_link_libraries(quickdom PUBLIC ${QUICKDOM_QJS})
target_compile_options(quickdom PRIVATE ${QUICKDOM_WARNINGS})
if(NOT MSVC)
	target_link_libraries(quickdom PUBLIC m)
endif()

# quickdom_gl: the commands drawn with nanovg in the host's GL context (glvnd's libOpenGL where it is)
if(QUICKDOM_GL)
	set(OpenGL_GL_PREFERENCE GLVND)
	find_package(OpenGL OPTIONAL_COMPONENTS EGL)
	if(NOT OpenGL_FOUND)
		message(WARNING "quickdom: no OpenGL found, quickdom_gl is not built")
		set(QUICKDOM_GL OFF)
	endif()
endif()
if(QUICKDOM_GL)
	if(QUICKDOM_NANOVG_TARGET)
		set(QUICKDOM_NVG ${QUICKDOM_NANOVG_TARGET})
	else()
		# its own stb_image left out: the core decodes pictures
		add_library(quickdom_nanovg STATIC thirdparty/nanovg/src/nanovg.c)
		target_include_directories(quickdom_nanovg PUBLIC ${PROJECT_SOURCE_DIR}/thirdparty/nanovg/src)
		target_compile_definitions(quickdom_nanovg PRIVATE NVG_NO_STB)
		if(NOT MSVC)
			target_compile_options(quickdom_nanovg PRIVATE -w)
		endif()
		set(QUICKDOM_NVG quickdom_nanovg)
	endif()
	add_library(quickdom_gl STATIC src/gl/gl.cpp src/gl/loader.cpp)
	target_include_directories(quickdom_gl PUBLIC ${PROJECT_SOURCE_DIR}/include PRIVATE ${PROJECT_SOURCE_DIR}/src)
	if(TARGET OpenGL::OpenGL)
		set(QUICKDOM_GL_LIB OpenGL::OpenGL)
	else()
		set(QUICKDOM_GL_LIB OpenGL::GL)
	endif()
	target_link_libraries(quickdom_gl PUBLIC quickdom PRIVATE ${QUICKDOM_NVG} ${QUICKDOM_GL_LIB})
	target_compile_options(quickdom_gl PRIVATE ${QUICKDOM_WARNINGS})
endif()

# quickdom_pugl: a document in a PUGL view, drawn by quickdom_gl (Linux: X11 and GLX)
if(QUICKDOM_PUGL AND NOT QUICKDOM_GL)
	message(WARNING "quickdom: quickdom_pugl needs quickdom_gl, it is not built")
	set(QUICKDOM_PUGL OFF)
endif()
if(QUICKDOM_PUGL AND NOT WIN32)
	find_package(X11)
	if(NOT X11_FOUND OR NOT TARGET OpenGL::GLX)
		message(WARNING "quickdom: no X11 or GLX found, quickdom_pugl is not built")
		set(QUICKDOM_PUGL OFF)
	endif()
endif()
if(QUICKDOM_PUGL)
	if(QUICKDOM_PUGL_TARGET)
		set(QUICKDOM_PUGL_LIB ${QUICKDOM_PUGL_TARGET})
	else()
		if(WIN32)
			add_library(quickdom_pugl_view STATIC thirdparty/pugl/src/common.c thirdparty/pugl/src/internal.c
				thirdparty/pugl/src/win.c thirdparty/pugl/src/win_gl.c)
			target_include_directories(quickdom_pugl_view PUBLIC ${PROJECT_SOURCE_DIR}/thirdparty/pugl/include)
			target_compile_definitions(quickdom_pugl_view PUBLIC PUGL_STATIC PRIVATE PUGL_INTERNAL)
			target_link_libraries(quickdom_pugl_view PUBLIC ${QUICKDOM_GL_LIB} gdi32 user32 shell32 shlwapi dwmapi)
		else()
			add_library(quickdom_pugl_view STATIC thirdparty/pugl/src/common.c thirdparty/pugl/src/internal.c
				thirdparty/pugl/src/x11.c thirdparty/pugl/src/x11_gl.c)
			target_include_directories(quickdom_pugl_view PUBLIC ${PROJECT_SOURCE_DIR}/thirdparty/pugl/include)
			target_compile_definitions(quickdom_pugl_view PUBLIC PUGL_STATIC
				PRIVATE PUGL_INTERNAL USE_XRANDR=0 _POSIX_C_SOURCE=200809L)
			# the pointer's shapes, when libXcursor is there
			if(X11_Xcursor_FOUND)
				target_link_libraries(quickdom_pugl_view PUBLIC X11::Xcursor)
			else()
				target_compile_definitions(quickdom_pugl_view PRIVATE USE_XCURSOR=0)
			endif()
			target_link_libraries(quickdom_pugl_view PUBLIC X11::X11 X11::Xext OpenGL::GLX ${QUICKDOM_GL_LIB})
		endif()
		if(NOT MSVC)
			target_compile_options(quickdom_pugl_view PRIVATE -w)
		endif()
		set(QUICKDOM_PUGL_LIB quickdom_pugl_view)
	endif()
	add_library(quickdom_pugl STATIC src/pugl/window.cpp src/pugl/keys.cpp src/pugl/x11.cpp)
	target_include_directories(quickdom_pugl PUBLIC ${PROJECT_SOURCE_DIR}/include PRIVATE ${PROJECT_SOURCE_DIR}/src)
	target_link_libraries(quickdom_pugl PUBLIC quickdom quickdom_gl ${QUICKDOM_PUGL_LIB} PRIVATE ${QUICKDOM_GL_LIB})
	if(NOT WIN32)
		target_link_libraries(quickdom_pugl PRIVATE X11::X11)
	endif()
	target_compile_options(quickdom_pugl PRIVATE ${QUICKDOM_WARNINGS})
endif()

if(QUICKDOM_TOOLS)
	add_executable(qdom-run tools/qdom_run.cpp tools/shots.cpp tools/png_write.cpp)
	target_link_libraries(qdom-run PRIVATE quickdom)
	target_compile_options(qdom-run PRIVATE ${QUICKDOM_WARNINGS})
	# PNGs read and written with nanovg's copies of stb_image and stb_image_write
	target_include_directories(qdom-run PRIVATE ${PROJECT_SOURCE_DIR}/thirdparty/nanovg/src
		${PROJECT_SOURCE_DIR}/thirdparty/nanovg/example)
	# --shot: the page drawn by quickdom_gl in an EGL surfaceless context
	if(QUICKDOM_GL AND TARGET OpenGL::EGL)
		target_sources(qdom-run PRIVATE tools/offscreen.cpp)
		target_link_libraries(qdom-run PRIVATE quickdom_gl OpenGL::EGL ${QUICKDOM_GL_LIB})
		target_compile_definitions(qdom-run PRIVATE QDOM_RUN_OFFSCREEN)
	endif()
	# --window, --shot-window
	if(QUICKDOM_PUGL)
		target_link_libraries(qdom-run PRIVATE quickdom_pugl)
		target_compile_definitions(qdom-run PRIVATE QDOM_RUN_WINDOW)
	endif()
endif()
