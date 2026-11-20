/*
 * sf100core.h - public API of libsf100core (DediProg SF100/SF600 SPI NOR programmers).
 *
 * Threading contract:
 *   - Single session per process. The legacy engine keeps its state in globals,
 *     so all sf_* calls except sf_cancel() MUST come from one thread (the worker).
 *   - sf_cancel() may be called from any thread; the running operation returns
 *     SF_ERR_CANCELLED as soon as the engine checks the flag.
 *   - Log and progress callbacks are invoked synchronously on the worker thread.
 *     GUI code must marshal them to its UI thread (e.g. Qt::QueuedConnection).
 *
 * Addresses/lengths are byte offsets into the flash. len == 0 means "to end of chip".
 */
#ifndef SF100CORE_H
#define SF100CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SF_OK = 0,
    SF_ERR_NO_DEVICE = -1, /* no programmer on USB */
    SF_ERR_ACCESS = -2, /* USB permission denied (udev rules) */
    SF_ERR_USB = -3, /* USB transfer/communication failure */
    SF_ERR_NOT_OPEN = -4, /* sf_open() not called / failed */
    SF_ERR_NO_CHIP = -5, /* chip not detected / not selected */
    SF_ERR_CHIPDB = -6, /* ChipInfoDb.dedicfg not found or unreadable */
    SF_ERR_ARG = -7, /* invalid argument / range outside chip */
    SF_ERR_CANCELLED = -8,
    SF_ERR_NOT_BLANK = -9, /* blank check found programmed bytes */
    SF_ERR_VERIFY = -10, /* verify mismatch */
    SF_ERR_FAILED = -11, /* operation failed in the engine */
    SF_ERR_NOMEM = -12,
    SF_ERR_FILE = -13, /* file I/O or file format error */
} sf_status;

const char* sf_strerror(int status);

/* ---- hooks ---- */
typedef enum { SF_LOG_DEBUG, SF_LOG_INFO, SF_LOG_WARN, SF_LOG_ERROR } sf_log_level;
/* text: one chunk of engine output (may contain or lack '\n'); NUL-terminated. */
typedef void (*sf_log_fn)(void* user, sf_log_level level, const char* text);
/* done/total in bytes; total == 0 means indeterminate (e.g. chip erase). */
typedef void (*sf_progress_fn)(void* user, uint64_t done, uint64_t total);

/* NULL handler restores the default: log -> stdout/stderr (CLI behaviour), progress -> none. */
void sf_set_log_handler(sf_log_fn fn, void* user);
void sf_set_progress_handler(sf_progress_fn fn, void* user);

/* ---- programmer ---- */
int sf_device_count(void); /* enumerate without opening; <0 on error */
int sf_open(int device); /* 1-based device number; 0 = first device */
void sf_close(void); /* safe to call when not open */

typedef struct {
    char model[16]; /* "SF100", "SF600", "SF600PG2", "SF700" */
    char firmware[16]; /* "7.2.21" */
    uint32_t serial; /* UID as printed by dpcmd */
} sf_programmer_info;
int sf_get_programmer_info(sf_programmer_info* out);

/* ---- settings (applied on the next operation) ---- */
typedef enum { SF_VCC_3V3 = 3300, SF_VCC_2V5 = 2500, SF_VCC_1V8 = 1800, SF_VCC_AUTO = 0 } sf_vcc;
typedef enum {
    SF_CLK_24M = 0, SF_CLK_8M = 1, SF_CLK_12M = 2, SF_CLK_3M = 3,
    SF_CLK_2180K = 4, SF_CLK_1500K = 5, SF_CLK_750K = 6, SF_CLK_375K = 7
} sf_spi_clock; /* values match the engine's clk_* enum */
typedef enum { SF_TARGET_SOCKET1 = 1, SF_TARGET_SOCKET2 = 2, SF_TARGET_CARD = 0, SF_TARGET_SKT = 3 } sf_target; /* = dpcmd --target */

typedef struct {
    sf_vcc vcc; /* SF_VCC_AUTO: try 3.3/2.5/1.8 during detect (dpcmd default) */
    sf_spi_clock clock; /* default SF_CLK_12M */
    sf_target target; /* default SF_TARGET_SOCKET1 */
} sf_config;
void sf_config_default(sf_config* cfg);
int sf_configure(const sf_config* cfg);

/* ---- chip ---- */
typedef struct {
    char type_name[64];
    char manufacturer[64];
    char ic_type[16]; /* "SPI_NOR", "SPI_NAND" */
    uint64_t size_bytes;
    uint32_t page_size;
    uint32_t sector_size; /* erase unit sf_erase_range() uses: max(sector, block) - the engine's segment
                             erase opcode (D8h & co.) clears a whole block. 0 if unknown */
    uint32_t jedec_id; /* 0 if unknown */
    int vcc_mv;
} sf_chip_info;

int sf_detect_chip(sf_chip_info* out); /* read JEDEC ID and look it up */
int sf_select_chip(const char* type_name, sf_chip_info* out); /* manual, like --type */
int sf_get_chip(sf_chip_info* out); /* currently active chip */

/* Chip database (parsed once, cached). Usable without sf_open(). */
int sf_chipdb_count(void); /* <0 on error */
int sf_chipdb_get(int index, sf_chip_info* out);

/* ---- operations (blocking; run them on a worker thread) ---- */
int sf_blank_check(uint64_t addr, uint64_t len); /* SF_OK or SF_ERR_NOT_BLANK */
int sf_erase_chip(void);
int sf_erase_range(uint64_t addr, uint64_t len); /* addr/len multiples of sector_size (len may run to chip end) */
/* Programs WITHOUT erasing first (same as dpcmd --prog): erase or blank-check the area beforehand. */
int sf_program(const uint8_t* data, uint64_t len, uint64_t addr);
int sf_read(uint8_t* out, uint64_t addr, uint64_t len);
/* On mismatch returns SF_ERR_VERIFY and, if first_diff != NULL, the first differing offset (relative to addr). */
int sf_verify(const uint8_t* data, uint64_t len, uint64_t addr, uint64_t* first_diff);

/* Status registers: index 0 = SR1 (05h), 1 = SR2 (35h), 2 = SR3 (15h). Generic JEDEC opcodes. */
int sf_read_status(int index, uint8_t* value);
/* Write SR1 (01h) or SR1+SR2 (01h, two bytes) after WREN, then wait for WIP clear. count = 1 or 2. */
int sf_write_status(const uint8_t* values, int count);

void sf_cancel(void);

/* ---- helpers (no hardware) ---- */
uint32_t sf_crc32(const uint8_t* data, size_t len); /* same CRC as dpcmd prints */
/* Load .bin/.hex/.s19 (format from extension, like dpcmd). *data is malloc'd, free with sf_free(). */
int sf_load_file(const char* path, uint8_t** data, size_t* len);
int sf_save_file(const char* path, const uint8_t* data, size_t len); /* raw binary */
void sf_free(void* p);

#ifdef __cplusplus
}
#endif
#endif
