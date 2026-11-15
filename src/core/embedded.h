// Copyright 2026 Ian Karlsson
//! \file
//! Files built into the library by cmake/embed.cmake: the JavaScript prelude and the default fonts.
#ifndef QUICKDOM_EMBEDDED_H
#define QUICKDOM_EMBEDDED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A built-in file: its name, its bytes (a NUL after them) and their count.
typedef struct qdom_embedded
{
	const char *name;
	const unsigned char *data;
	size_t size;
} qdom_embedded_t;

extern const qdom_embedded_t qdom_prelude[];
extern const size_t qdom_prelude_count;
extern const qdom_embedded_t qdom_fonts[];
extern const size_t qdom_fonts_count;

#ifdef __cplusplus
}
#endif

#endif
