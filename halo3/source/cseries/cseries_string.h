#pragma once
#ifndef __CSERIES_STRING_H__
#define __CSERIES_STRING_H__

/* ---------- headers */

#include <stdint.h>

#include <stdarg.h>
#include <stddef.h>

/* ---------- constants */

enum e_csstrtok_delimiter_mode
{
    _cstrtok_delimiter_mode_default,
    _cstrtok_delimiter_mode_skip,
    k_cstrtok_delimiter_mode_count,
};

/* ---------- definitions */

struct csstrtok_data
{
    char* next_string; // 0x0
};

/* ---------- prototypes */

extern bool ascii_islower(int32_t character_code);
extern bool ascii_isupper(int32_t character_code);
extern bool ascii_isdigit(int32_t character_code);
extern bool ascii_isalnum(int32_t character_code);
extern bool ascii_isspace(int32_t character_code);
extern bool ascii_isspace(char character_code);
extern int32_t ascii_tolower(int32_t character_code);
extern int32_t ascii_toupper(int32_t character_code);
extern char* ascii_strnupr(char* string, size_t count);
extern char* ascii_strnlwr(char* string, size_t count);
extern unsigned int ascii_stristr(char const* look_inside, char const* look_for);
extern int32_t ascii_strnicmp(char const* s1, char const* s2, size_t maximum_character_comparisons);
extern int32_t ascii_stricmp(char const* s1, char const* s2);
extern int32_t csstricmp(char const* s1, char const* s2);
extern int32_t csstrnicmp(char const* s1, char const* s2, size_t count);
extern char* csstristr(char const* haystack, char const* needle);
extern char* csstrnzcpy(char* s1, char const* s2, size_t s1_size);
extern char* csstrnzcat(char* s1, char const* s2, size_t s1_size);
extern char* csstrnzncat(char* s1, char const* s2, size_t s1_size, size_t count);
extern char* csstrnupr(char* s, unsigned int size);
extern char* csstrnlwr(char* s, unsigned int size);
extern char* csnzappendf(char* buffer, size_t size, char const* format, ...);
extern char* csnzprintf(char* buffer, size_t size, char const* format, ...);
extern char* csstrtok(char* s, char const* delimiters, e_csstrtok_delimiter_mode delimiter_mode, csstrtok_data* data);

extern "C"
{
    extern int strcmp_debug(const char* s1, const char* s2);
    extern int strncmp_debug(const char* s1, const char* s2, size_t size);
    extern size_t strlen_debug(const char* s);
    extern char* strstr_debug(const char* haystack, const char* needle);
    extern char* strchr_debug(const char* haystack, int needle);
    extern char* strrchr_debug(const char* haystack, int needle);
}

/* ---------- globals */

/* ---------- public code */

#endif // __CSERIES_STRING_H__