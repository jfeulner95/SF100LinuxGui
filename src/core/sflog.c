/* NOT compiled with sf_hook.h: the real libc stdio functions are used here. */
#include "sflog.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static sf_log_fn s_fn = NULL;
static void* s_user = NULL;

void sflog_set_handler(sf_log_fn fn, void* user)
{
    s_fn = fn;
    s_user = user;
}

static sf_log_level level_of(const char* s)
{
    while (*s == '\n' || *s == ' ')
        s++;
    if (strncmp(s, "Error", 5) == 0)
        return SF_LOG_ERROR;
    if (strncmp(s, "Warning", 7) == 0)
        return SF_LOG_WARN;
    return SF_LOG_INFO;
}

void sf_printf(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_fn)
        s_fn(s_user, level_of(buf), buf);
    else
        fputs(buf, stdout);
}

void sf_eprintf(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_fn)
        s_fn(s_user, SF_LOG_ERROR, buf);
    else
        fputs(buf, stderr);
}

/* ---- stdio hooks for the engine (see sf_hook.h) ---- */

/* Deliver text to the handler if f is stdout/stderr and a handler is set; returns 1 if consumed. */
static int to_handler(FILE* f, const char* text)
{
    if (s_fn == NULL)
        return 0;
    if (f == stdout)
        s_fn(s_user, level_of(text), text);
    else if (f == stderr)
        s_fn(s_user, SF_LOG_ERROR, text);
    else
        return 0;
    return 1;
}

static int hook_vfprintf(FILE* f, const char* fmt, va_list ap)
{
    char buf[1024];
    int n;
    va_list ap2;

    if (s_fn == NULL || (f != stdout && f != stderr))
        return vfprintf(f, fmt, ap);
    va_copy(ap2, ap);
    n = vsnprintf(buf, sizeof(buf), fmt, ap2);
    va_end(ap2);
    to_handler(f, buf);
    return n;
}

int sf_hook_printf(const char* fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = hook_vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int sf_hook_fprintf(FILE* f, const char* fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = hook_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int sf_hook_fputs(const char* s, FILE* f)
{
    if (to_handler(f, s))
        return 0;
    return fputs(s, f);
}

int sf_hook_puts(const char* s)
{
    char buf[1024];

    if (s_fn == NULL)
        return puts(s);
    snprintf(buf, sizeof(buf), "%s\n", s);
    to_handler(stdout, buf);
    return 0;
}

int sf_hook_putchar(int c)
{
    char buf[2];

    if (s_fn == NULL)
        return putchar(c);
    buf[0] = (char)c;
    buf[1] = '\0';
    to_handler(stdout, buf);
    return (unsigned char)c;
}

void sf_hook_perror(const char* s)
{
    char buf[1024];
    int e = errno;

    if (s_fn == NULL) {
        errno = e;
        perror(s);
        return;
    }
    if (s != NULL && *s != '\0')
        snprintf(buf, sizeof(buf), "%s: %s\n", s, strerror(e));
    else
        snprintf(buf, sizeof(buf), "%s\n", strerror(e));
    to_handler(stderr, buf);
}
