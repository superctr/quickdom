# Copyright 2026 Ian Karlsson
# Files built into the library as byte arrays:
#   cmake -DBASE=<dir> -DFILES=<name,name...> -DNAME=<symbol> -DOUTPUT=<file.c> -P embed.cmake
# defines NAME[] ({ name, data, size }, one per file, NUL after each file's bytes) and NAME_count.
string(REPLACE "," ";" FILES "${FILES}")
set(body "#include <stddef.h>\n#include \"core/embedded.h\"\n")
set(table "")
set(n 0)
foreach(f ${FILES})
	file(READ "${BASE}/${f}" hex HEX)
	string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
	string(REGEX REPLACE "(0x[0-9a-f][0-9a-f],){24}" "\\0\n" bytes "${bytes}")
	string(APPEND body "static const unsigned char file${n}[] = {\n${bytes}0x00\n};\n")
	string(APPEND table "\t{ \"${f}\", file${n}, sizeof(file${n}) - 1 },\n")
	math(EXPR n "${n} + 1")
endforeach()
string(APPEND body "const qdom_embedded_t ${NAME}[] = {\n${table}};\nconst size_t ${NAME}_count = ${n};\n")
file(WRITE "${OUTPUT}.tmp" "${body}")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
