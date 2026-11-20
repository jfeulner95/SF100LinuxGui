/* Hardware-free tests of the sf100core public API. Needs DPCMD_CHIPDB (set by ctest). */
#include "sf100core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

static int g_fail = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

static void test_crc(void)
{
    CHECK(sf_crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u);
    CHECK(sf_crc32(NULL, 0) == 0);
}

static void test_chipdb(void)
{
    int i, n = sf_chipdb_count();
    int found = 0;
    sf_chip_info c;

    CHECK(n > 1000);
    CHECK(sf_chipdb_get(-1, &c) == SF_ERR_ARG);
    CHECK(sf_chipdb_get(n, &c) == SF_ERR_ARG);
    for (i = 0; i < n; i++) {
        if (sf_chipdb_get(i, &c) != SF_OK)
            break;
        if (strcmp(c.type_name, "W25Q64") == 0) {
            found = 1;
            CHECK(c.size_bytes == 8u * 1024 * 1024);
            CHECK(strcmp(c.ic_type, "SPI_NOR") == 0);
            CHECK(strcmp(c.manufacturer, "Winbond") == 0);
            CHECK(c.page_size == 256);
            CHECK(c.sector_size >= 4096);
            CHECK(c.jedec_id == 0xEF4017u);
            CHECK(c.vcc_mv == 3300);
            break;
        }
    }
    CHECK(found);
}

static void test_load_file(void)
{
    static const char* files[3] = { TEST_DATA_DIR "/test16.bin", TEST_DATA_DIR "/test16.hex", TEST_DATA_DIR "/test16.s19" };
    uint8_t* d[3] = { NULL, NULL, NULL };
    size_t l[3] = { 0, 0, 0 };
    int i;

    for (i = 0; i < 3; i++) {
        CHECK(sf_load_file(files[i], &d[i], &l[i]) == SF_OK);
        CHECK(l[i] == 16);
    }
    if (d[0] && d[1] && d[2] && l[0] == 16 && l[1] == 16 && l[2] == 16) {
        for (i = 0; i < 16; i++)
            CHECK(d[0][i] == (uint8_t)(i * 0x11));
        CHECK(memcmp(d[0], d[1], 16) == 0);
        CHECK(memcmp(d[0], d[2], 16) == 0);
    }
    for (i = 0; i < 3; i++)
        sf_free(d[i]);

    d[0] = NULL;
    CHECK(sf_load_file(TEST_DATA_DIR "/does-not-exist.bin", &d[0], &l[0]) == SF_ERR_FILE);
    CHECK(d[0] == NULL);
}

typedef struct {
    int n_error;
    int saw_no_programmer;
} log_seen;

static void on_log(void* user, sf_log_level level, const char* text)
{
    log_seen* s = (log_seen*)user;

    if (level == SF_LOG_ERROR) {
        s->n_error++;
        if (strstr(text, "No programmer found"))
            s->saw_no_programmer = 1;
    }
}

static void test_no_hardware(void)
{
    uint8_t buf[4];
    log_seen seen = { 0, 0 };

    CHECK(sf_read(buf, 0, 4) == SF_ERR_NOT_OPEN);
    CHECK(sf_erase_chip() == SF_ERR_NOT_OPEN);
    sf_close(); /* safe when not open */

    sf_set_log_handler(on_log, &seen);
    {
        /* CI has no programmer; a runner without usbfs fails libusb_init instead (SF_ERR_USB) */
        int rc = sf_open(1);
        CHECK(rc == SF_ERR_NO_DEVICE || rc == SF_ERR_USB);
    }
    sf_set_log_handler(NULL, NULL);
    CHECK(seen.n_error > 0);
    CHECK(seen.saw_no_programmer);

    sf_close();
    CHECK(sf_read(buf, 0, 4) == SF_ERR_NOT_OPEN);
    CHECK(sf_strerror(SF_ERR_NO_DEVICE) != NULL);
}

int main(void)
{
    test_crc();
    test_chipdb();
    test_load_file();
    test_no_hardware();
    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
