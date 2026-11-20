/* Engine output funnel: sf_printf/sf_eprintf behave like printf/fprintf(stderr,...)
 * unless a log handler was installed (sf_set_log_handler in sf100core.h).
 * The sf_hook_* functions are what sf_hook.h maps the engine's stdio calls to. */
#ifndef SFLOG_H
#define SFLOG_H

#include "sf100core.h"
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define SF_PRINTF_FMT(a, b) __attribute__((format(__printf__, a, b)))
#else
#define SF_PRINTF_FMT(a, b)
#endif

void sf_printf(const char* fmt, ...) SF_PRINTF_FMT(1, 2);
void sf_eprintf(const char* fmt, ...) SF_PRINTF_FMT(1, 2);
void sflog_set_handler(sf_log_fn fn, void* user);

int sf_hook_printf(const char* fmt, ...) SF_PRINTF_FMT(1, 2);
int sf_hook_fprintf(FILE* f, const char* fmt, ...) SF_PRINTF_FMT(2, 3);
int sf_hook_fputs(const char* s, FILE* f);
int sf_hook_puts(const char* s);
int sf_hook_putchar(int c);
void sf_hook_perror(const char* s);

#endif
