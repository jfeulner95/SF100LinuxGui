/* Force-included (-include) into the ENGINE sources only (external/SF100Linux/*.c), never into
 * sf100core.c, sflog.c, main.c or the tests. It redirects the engine's stdio output to sflog.c so a
 * log handler (sf_set_log_handler) can capture it without patching the engine. Without a handler the
 * sf_hook_* functions behave exactly like the libc call they replace. */
#ifndef SF_HOOK_H
#define SF_HOOK_H
#include <stdio.h>
#undef printf
#undef fprintf
#undef fputs
#undef puts
#undef putchar
#undef perror
int sf_hook_printf(const char* fmt, ...) __attribute__((format(__printf__, 1, 2)));
int sf_hook_fprintf(FILE* f, const char* fmt, ...) __attribute__((format(__printf__, 2, 3)));
int sf_hook_fputs(const char* s, FILE* f);
int sf_hook_puts(const char* s);
int sf_hook_putchar(int c);
void sf_hook_perror(const char* s);
#define printf(...) sf_hook_printf(__VA_ARGS__)
#define fprintf(...) sf_hook_fprintf(__VA_ARGS__)
#define fputs(s, f) sf_hook_fputs(s, f)
#define puts(s) sf_hook_puts(s)
#define putchar(c) sf_hook_putchar(c)
#define perror(s) sf_hook_perror(s)
#endif
