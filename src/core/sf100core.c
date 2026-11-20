/*
 * sf100core.c - implementation of the public API in sf100core.h on top of the legacy dpcmd engine.
 *
 * The engine keeps its state in globals and is not re-entrant: one programmer (engine index s_idx),
 * all calls except sf_cancel() from one thread. Operations run synchronously on the caller thread by
 * calling threadRun() directly (the CLI runs the same function on a pthread).
 */
#include "sf100core.h"
#include "ChipInfoDb.h"
#include "FlashCommand.h"
#include "SerialFlash.h"
#include "board.h"
#include "dpcmd.h"
#include "project.h"
#include "sflog.h"
#include "usbdriver.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Engine globals (defined in the engine's dpcmd.c/project.c/board.c/SerialFlash.c); types as defined there. */
extern unsigned int g_Vcc;
extern unsigned int g_ucSPIClock;
extern unsigned int g_uiDevNum;
extern unsigned int g_uiAddr;
extern size_t g_uiLen;
extern unsigned int g_ucFill;
extern int g_StartupMode;
extern char* g_parameter_vcc;
extern CHIP_INFO g_ChipInfo;
extern char g_strTypeName[TYPENAME_MAX_LEN];
extern char g_board_type[9];
extern int g_firmversion;
extern bool isSendFFsequence;
extern bool g_bIsNANDFlash;
extern volatile int m_isCanceled;
extern struct CAddressRange DownloadAddrRange;
extern struct CAddressRange UploadAddrRange;
extern struct CAddressRange LockAddrrange;
extern volatile bool g_is_operation_on_going;
extern bool g_is_operation_successful[16];
extern unsigned char* pBufferForLastReadData[16];
extern unsigned char* pBufferforLoadedFile;
extern unsigned long g_ulFileSize;
extern unsigned int g_uiFileChecksum;
extern volatile bool g_bIsSF600[16];
extern volatile bool g_bIsSF700[16];
extern volatile bool g_bIsSF600PG2[16];
extern bool bAuto[16];
extern unsigned int CompleteCnt;
extern unsigned char mcode_SegmentErase;

/* ---- session state ---- */
static int s_open = 0;
static int s_idx = 0; /* engine index = device number - 1 */
static int s_have_chip = 0;
static int s_nand = 0; /* active chip is SPI NAND: detected/listed, but operations are not supported */
static int s_cfg_dirty = 1;
static sf_config s_cfg = { SF_VCC_AUTO, SF_CLK_12M, SF_TARGET_SOCKET1 };

/* ---- progress ---- */
static sf_progress_fn s_pfn = NULL;
static void* s_puser = NULL;
static uint64_t s_total = 0, s_done = 0, s_last = 0;
static int s_active = 0;

void sf_set_log_handler(sf_log_fn fn, void* user)
{
    sflog_set_handler(fn, user);
}

void sf_set_progress_handler(sf_progress_fn fn, void* user)
{
    s_pfn = fn;
    s_puser = user;
}

static void sf_progress_begin(uint64_t total)
{
    s_total = total;
    s_done = 0;
    s_last = 0;
    s_active = 1;
    if (s_pfn)
        s_pfn(s_puser, 0, total);
}

/* Reports at most once per 64 KiB (and at completion). */
static void progress_add(uint64_t bytes)
{
    if (!s_active)
        return;
    s_done += bytes;
    if (s_total > 0 && s_done > s_total)
        s_done = s_total;
    if (s_pfn && s_done != s_last && (s_done - s_last >= 65536 || (s_total > 0 && s_done == s_total))) {
        s_last = s_done;
        s_pfn(s_puser, s_done, s_total);
    }
}

/* Installed as g_usb_progress_hook (engine usbdriver.c) while a programmer is open. */
static void sf_progress_add_ul(unsigned long bytes)
{
    progress_add((uint64_t)bytes);
}

static void sf_progress_end(int ok)
{
    if (ok && s_pfn && s_total > 0 && s_last != s_total)
        s_pfn(s_puser, s_total, s_total);
    s_active = 0;
}

const char* sf_strerror(int status)
{
    switch (status) {
    case SF_OK: return "success";
    case SF_ERR_NO_DEVICE: return "no programmer found on USB";
    case SF_ERR_ACCESS: return "USB permission denied (check udev rules)";
    case SF_ERR_USB: return "USB communication failure";
    case SF_ERR_NOT_OPEN: return "programmer not open";
    case SF_ERR_NO_CHIP: return "chip not detected or not selected";
    case SF_ERR_CHIPDB: return "chip database (ChipInfoDb.dedicfg) not found or unreadable";
    case SF_ERR_ARG: return "invalid argument or range outside the chip";
    case SF_ERR_CANCELLED: return "operation cancelled";
    case SF_ERR_NOT_BLANK: return "chip is not blank";
    case SF_ERR_VERIFY: return "verify mismatch";
    case SF_ERR_FAILED: return "operation failed";
    case SF_ERR_NOMEM: return "out of memory";
    case SF_ERR_FILE: return "file I/O or file format error";
    default: return "unknown error";
    }
}

/* ---- helpers ---- */
static void copy_str(char* dst, size_t n, const char* src)
{
    size_t l = strlen(src);
    if (l >= n)
        l = n - 1;
    memcpy(dst, src, l);
    dst[l] = '\0';
}

static size_t max_sz(size_t a, size_t b)
{
    return a > b ? a : b;
}

/* smallest erase unit the engine can really apply: its segment-erase opcode (D8h & co.) erases a
 * whole block, so this is max(sector, block), the same step BlazeUpdate() (dpcmd --auto) uses. */
static uint32_t erase_unit(const CHIP_INFO* c)
{
    return (uint32_t)max_sz(c->SectorSizeInByte, c->BlockSizeInByte);
}

static void chip_to_info(const CHIP_INFO* c, sf_chip_info* o)
{
    memset(o, 0, sizeof(*o));
    copy_str(o->type_name, sizeof(o->type_name), c->TypeName);
    copy_str(o->manufacturer, sizeof(o->manufacturer), c->Manufacturer);
    copy_str(o->ic_type, sizeof(o->ic_type), c->ICType);
    o->size_bytes = c->ChipSizeInByte;
    o->page_size = (uint32_t)c->PageSizeInByte;
    o->sector_size = erase_unit(c);
    o->jedec_id = (uint32_t)c->UniqueID;
    o->vcc_mv = (int)c->VoltageInMv;
}

static void enter_standalone(int i)
{
    if (g_bIsSF600[i] == true || is_SF700_Or_SF600PG2(i))
        LeaveSF600Standalone(false, i);
}

static void leave_standalone(int i)
{
    if (g_bIsSF600[i] == true || is_SF700_Or_SF600PG2(i))
        LeaveSF600Standalone(true, i);
}

static void free_buffers(void)
{
    free(pBufferForLastReadData[s_idx]);
    pBufferForLastReadData[s_idx] = NULL;
    free(pBufferforLoadedFile);
    pBufferforLoadedFile = NULL;
    g_ulFileSize = 0;
}

static void reset_chip_state(void)
{
    memset(&g_ChipInfo, 0, sizeof(g_ChipInfo));
    g_strTypeName[0] = '\0';
    s_have_chip = 0;
    s_nand = 0;
    isSendFFsequence = false;
    g_bIsNANDFlash = false;
}

/* ---- programmer ---- */
int sf_device_count(void)
{
    return usb_count_devices();
}

int sf_open(int device)
{
    char path[512];
    int err;

    if (device < 0 || device > MAX_Dev_Index)
        return SF_ERR_ARG;
    sf_close();
    s_idx = device <= 0 ? 0 : device - 1;
    g_uiDevNum = (unsigned int)s_idx + 1; /* engine single-device code paths */
    memset(g_board_type, 0, sizeof(g_board_type));
    g_firmversion = 0;
    reset_chip_state();

    if (!usb_driver_init()) {
        err = g_usb_last_error;
        usb_driver_release(); /* usb_driver_init() leaves the context/device refs behind on failure */
        if (err == LIBUSB_ERROR_ACCESS)
            return SF_ERR_ACCESS;
        if (err == 0 || err == LIBUSB_ERROR_NO_DEVICE)
            return SF_ERR_NO_DEVICE;
        return SF_ERR_USB;
    }
    s_open = 1;
    g_usb_progress_hook = sf_progress_add_ul;
    /* same as dpcmd main() after OpenUSB() */
    leave_standalone(s_idx);
    QueryBoard(s_idx);

    if (!FindChipDbFile(path, sizeof(path))) { /* 512 = buffer size the engine's GetChipDbPath() uses */
        sf_eprintf("Error: ChipInfoDb.dedicfg not found (set DPCMD_CHIPDB)\n");
        sf_close();
        return SF_ERR_CHIPDB;
    }
    s_cfg_dirty = 1;
    return SF_OK;
}

void sf_close(void)
{
    g_usb_progress_hook = NULL;
    if (s_open) {
        enter_standalone(s_idx); /* CloseProject() */
        usb_driver_release();
        s_open = 0;
    }
    free_buffers();
    reset_chip_state();
}

int sf_get_programmer_info(sf_programmer_info* out)
{
    size_t n;

    if (!s_open)
        return SF_ERR_NOT_OPEN;
    if (out == NULL)
        return SF_ERR_ARG;
    memset(out, 0, sizeof(*out));
    n = strnlen(g_board_type, sizeof(g_board_type));
    while (n > 0 && (g_board_type[n - 1] == ' ' || g_board_type[n - 1] == '\0'))
        n--;
    if (n == 0)
        return SF_ERR_USB; /* board did not answer the info request */
    memcpy(out->model, g_board_type, n);
    out->model[n] = '\0';
    snprintf(out->firmware, sizeof(out->firmware), "%u.%u.%u",
        (unsigned)((g_firmversion >> 16) & 0xFF), (unsigned)((g_firmversion >> 8) & 0xFF), (unsigned)(g_firmversion & 0xFF));
    out->serial = ReadUID(s_idx);
    return SF_OK;
}

/* ---- settings ---- */
void sf_config_default(sf_config* cfg)
{
    if (cfg == NULL)
        return;
    cfg->vcc = SF_VCC_AUTO;
    cfg->clock = SF_CLK_12M;
    cfg->target = SF_TARGET_SOCKET1;
}

int sf_configure(const sf_config* cfg)
{
    if (cfg == NULL)
        return SF_ERR_ARG;
    if (cfg->vcc != SF_VCC_AUTO && cfg->vcc != SF_VCC_3V3 && cfg->vcc != SF_VCC_2V5 && cfg->vcc != SF_VCC_1V8)
        return SF_ERR_ARG;
    if ((int)cfg->clock < SF_CLK_24M || (int)cfg->clock > SF_CLK_375K)
        return SF_ERR_ARG;
    if ((int)cfg->target < SF_TARGET_CARD || (int)cfg->target > SF_TARGET_SKT)
        return SF_ERR_ARG;
    s_cfg = *cfg; /* allowed before sf_open(); applied by the next detect/select/operation */
    s_cfg_dirty = 1;
    return SF_OK;
}

/* What dpcmd InitProject() does per device before detection, driven by s_cfg instead of argv.
 *
 * Vcc mapping (dpcmd --vcc N -> g_Vcc, see cli_classic_usage() and cli/dpcmd.c SetVcc()):
 *   SF_VCC_AUTO : g_parameter_vcc = "NO"; the engine probes 1.8/2.5/3.3(3.5) V in GetFirstDetectionMatch()
 *                 and takes the voltage of the detected chip (dpcmd default when --vcc is not given).
 *   SF100       : g_Vcc is a selector 0x10|index with index 0/1/2 = 3.5V/2.5V/1.8V (== --vcc 0/1/2):
 *                 SF_VCC_3V3 -> 0x10 (vcc3_5V, the SF100's "3.3V" rail is 3.5V), 2V5 -> 0x11, 1V8 -> 0x12.
 *   SF600/SF700/SF600PG2: g_Vcc is the voltage in millivolts (== --vcc 3300 etc., range 1800..3800).
 * An explicit voltage sets g_parameter_vcc to a non-"NO" string, which makes the engine skip probing
 * and keep g_Vcc as configured. */
static void apply_config(void)
{
    static const int targets[4] = { STARTUP_APPLI_CARD, STARTUP_APPLI_SF_1, STARTUP_APPLI_SF_2, STARTUP_APPLI_SF_SKT };
    static char vcc_explicit[] = "0";
    int mv_board = (g_bIsSF600[s_idx] == true) || is_SF700_Or_SF600PG2(s_idx);

    g_StartupMode = targets[s_cfg.target];
    SetTargetFlash((unsigned char)targets[s_cfg.target], s_idx);
    SetIOMOdeValue(s_idx); /* SetSPIIOMode() */
    g_ucSPIClock = (unsigned int)s_cfg.clock;
    SetSPIClock(s_idx);

    if (s_cfg.vcc == SF_VCC_AUTO) {
        g_parameter_vcc = "NO";
    } else {
        g_parameter_vcc = vcc_explicit;
        if (mv_board)
            g_Vcc = (unsigned int)s_cfg.vcc;
        else
            g_Vcc = vcc3_5V + (s_cfg.vcc == SF_VCC_3V3 ? 0u : s_cfg.vcc == SF_VCC_2V5 ? 1u : 2u);
    }

    dediprog_set_vpp_voltage(0, s_idx); /* SetVppVoltage(0, i): request + 200 ms settle */
    Sleep(200);
    s_cfg_dirty = 0;
}

/* ---- chip ---- */
int sf_detect_chip(sf_chip_info* out)
{
    if (!s_open)
        return SF_ERR_NOT_OPEN;
    SerialFlash_ClearCancelOperationFlag();
    g_uiDevNum = (unsigned int)s_idx + 1;
    apply_config();
    isSendFFsequence = false;
    g_bIsNANDFlash = false;

    /* InitProject() -> ProjectInit(): detect, then initialise the engine for the chip */
    g_ChipInfo = GetFirstDetectionMatch(g_strTypeName, s_idx);
    if (g_ChipInfo.UniqueID == 0) {
        s_have_chip = 0;
        s_nand = 0;
        return m_isCanceled ? SF_ERR_CANCELLED : SF_ERR_NO_CHIP;
    }
    ProjectInitWithID(g_ChipInfo, s_idx);
    s_have_chip = 1;
    s_nand = strstr(g_ChipInfo.ICType, "SPI_NAND") != NULL;
    if (out)
        chip_to_info(&g_ChipInfo, out);
    return SF_OK;
}

int sf_select_chip(const char* type_name, sf_chip_info* out)
{
    char name[TYPENAME_MAX_LEN];

    if (!s_open)
        return SF_ERR_NOT_OPEN;
    if (type_name == NULL || type_name[0] == '\0' || strlen(type_name) >= sizeof(name))
        return SF_ERR_ARG;
    strcpy(name, type_name);
    SerialFlash_ClearCancelOperationFlag();
    g_uiDevNum = (unsigned int)s_idx + 1;
    apply_config();
    isSendFFsequence = false;
    g_bIsNANDFlash = false;
    s_have_chip = 0;
    s_nand = 0;

    /* InitProject() with --type. The DB lookup returns 1 also when the DB file is missing, hence the name check. */
    if (!Dedi_Search_Chip_Db_ByTypeName(name, &g_ChipInfo) || strcmp(g_ChipInfo.TypeName, name) != 0) {
        memset(&g_ChipInfo, 0, sizeof(g_ChipInfo));
        return SF_ERR_ARG;
    }
    ProjectInitWithID(g_ChipInfo, s_idx);
    if (!strcmp(g_ChipInfo.Class, "N25Qxxx_Large"))
        isSendFFsequence = true;
    s_have_chip = 1;
    s_nand = strstr(g_ChipInfo.ICType, "SPI_NAND") != NULL;
    if (out)
        chip_to_info(&g_ChipInfo, out);
    return SF_OK;
}

int sf_get_chip(sf_chip_info* out)
{
    if (!s_open)
        return SF_ERR_NOT_OPEN;
    if (!s_have_chip)
        return SF_ERR_NO_CHIP;
    if (out)
        chip_to_info(&g_ChipInfo, out);
    return SF_OK;
}

/* Chip database: parsed once on first use and cached for the life of the process (never freed). */
static sf_chip_info* s_db = NULL;
static int s_db_n = 0;

static int db_load(void)
{
    CHIP_INFO* raw = NULL;
    int n, i;

    if (s_db != NULL)
        return s_db_n;
    n = Dedi_Load_AllChips(&raw);
    if (n <= 0) {
        free(raw);
        return SF_ERR_CHIPDB; /* not cached: a later call may succeed once DPCMD_CHIPDB is fixed */
    }
    s_db = (sf_chip_info*)malloc((size_t)n * sizeof(sf_chip_info));
    if (s_db == NULL) {
        free(raw);
        return SF_ERR_NOMEM;
    }
    for (i = 0; i < n; i++)
        chip_to_info(&raw[i], &s_db[i]);
    free(raw);
    s_db_n = n;
    return n;
}

int sf_chipdb_count(void)
{
    return db_load();
}

int sf_chipdb_get(int index, sf_chip_info* out)
{
    int n = db_load();

    if (n < 0)
        return n;
    if (out == NULL || index < 0 || index >= n)
        return SF_ERR_ARG;
    *out = s_db[index];
    return SF_OK;
}

/* ---- operations ---- */
static int op_enter(void)
{
    if (!s_open)
        return SF_ERR_NOT_OPEN;
    if (!s_have_chip)
        return SF_ERR_NO_CHIP;
    if (s_nand) {
        sf_eprintf("Error: SPI NAND chips are not supported by this API.\n");
        return SF_ERR_ARG;
    }
    SerialFlash_ClearCancelOperationFlag();
    g_op_identify_failed = false;
    g_uiDevNum = (unsigned int)s_idx + 1;
    if (s_cfg_dirty)
        apply_config();
    return SF_OK;
}

/* cancel wins over every failure code */
static int op_result(int ok)
{
    if (m_isCanceled)
        return SF_ERR_CANCELLED;
    if (ok)
        return SF_OK;
    return g_op_identify_failed ? SF_ERR_NO_CHIP : SF_ERR_FAILED;
}

/* Run one engine operation synchronously: threadRun() is what dpcmd runs on a pthread (Run()). */
static int engine_op(OPERATION_TYPE type)
{
    THREAD_STRUCT* d = (THREAD_STRUCT*)malloc(sizeof(THREAD_STRUCT));

    if (d == NULL)
        return SF_ERR_NOMEM;
    d->type = type;
    d->USBIndex = s_idx;
    CompleteCnt = 0;
    bAuto[s_idx] = false;
    g_is_operation_on_going = true;
    threadRun(d); /* frees d, sets g_is_operation_successful[] and clears g_is_operation_on_going */
    return op_result(g_is_operation_successful[s_idx]);
}

/* addr/len against the chip; len == 0 -> to end of chip */
static int norm_range(uint64_t addr, uint64_t* len)
{
    uint64_t size = g_ChipInfo.ChipSizeInByte;

    if (addr >= size)
        return SF_ERR_ARG;
    if (*len == 0)
        *len = size - addr;
    else if (*len > size - addr)
        return SF_ERR_ARG;
    if (*len > (uint64_t)SIZE_MAX)
        return SF_ERR_ARG;
    return SF_OK;
}

int sf_read(uint8_t* out, uint64_t addr, uint64_t len)
{
    int rc;
    unsigned char* buf;

    rc = op_enter();
    if (rc != SF_OK)
        return rc;
    if (out == NULL)
        return SF_ERR_ARG;
    rc = norm_range(addr, &len);
    if (rc != SF_OK)
        return rc;

    /* like dpcmd do_Read() */
    DownloadAddrRange.start = (size_t)addr;
    DownloadAddrRange.length = (size_t)len;
    DownloadAddrRange.end = (size_t)(addr + len);
    g_uiAddr = (unsigned int)addr;
    g_uiLen = (size_t)len;

    sf_progress_begin(len);
    rc = engine_op(READ_ANY_BY_PREFERENCE_CONFIGURATION);
    buf = pBufferForLastReadData[s_idx];
    if (rc == SF_OK) {
        if (buf == NULL || addr < UploadAddrRange.start || addr + len > UploadAddrRange.end)
            rc = SF_ERR_FAILED;
        else
            memcpy(out, buf + (addr - UploadAddrRange.start), (size_t)len);
    }
    free(buf); /* the engine keeps a chip-sized copy otherwise */
    pBufferForLastReadData[s_idx] = NULL;
    sf_progress_end(rc == SF_OK);
    return rc;
}

int sf_blank_check(uint64_t addr, uint64_t len)
{
    int rc;
    uint64_t i;
    uint8_t* tmp;

    rc = op_enter();
    if (rc != SF_OK)
        return rc;
    if (addr == 0 && (len == 0 || len == g_ChipInfo.ChipSizeInByte)) {
        sf_progress_begin(g_ChipInfo.ChipSizeInByte);
        rc = engine_op(BLANKCHECK_WHOLE_CHIP);
        /* the engine reports only blank / not blank; a read error inside it looks the same */
        if (rc == SF_ERR_FAILED)
            rc = SF_ERR_NOT_BLANK;
        sf_progress_end(rc == SF_OK);
        return rc;
    }
    rc = norm_range(addr, &len);
    if (rc != SF_OK)
        return rc;
    tmp = (uint8_t*)malloc((size_t)len);
    if (tmp == NULL)
        return SF_ERR_NOMEM;
    rc = sf_read(tmp, addr, len);
    if (rc == SF_OK) {
        for (i = 0; i < len; i++) {
            if (tmp[i] != 0xFF) {
                rc = SF_ERR_NOT_BLANK;
                break;
            }
        }
    }
    free(tmp);
    return rc;
}

int sf_erase_chip(void)
{
    int rc = op_enter();

    if (rc != SF_OK)
        return rc;
    sf_progress_begin(0); /* indeterminate */
    rc = engine_op(ERASE_WHOLE_CHIP);
    sf_progress_end(rc == SF_OK);
    return rc;
}

/* Range erase through the engine's batch erase (the function dpcmd --auto uses): it sends the class-specific
 * segment-erase opcode (mcode_SegmentErase) for each address, handles 4-byte addressing and wraps the
 * sequence in StartofOperation(). SerialFlash_rangeErase() is NOT used: it has no callers in the engine, takes
 * cmd/section size that cannot be derived from CHIP_INFO (EraseCmd is not parsed from the DB), skips
 * StartofOperation() and sends the erase as a RESULT_IN transceive. */
int sf_erase_range(uint64_t addr, uint64_t len)
{
    enum { CHUNK = 16 };
    int rc, ok;
    uint64_t size, unit, off;
    uintptr_t addrs[CHUNK];

    rc = op_enter();
    if (rc != SF_OK)
        return rc;
    size = g_ChipInfo.ChipSizeInByte;
    unit = erase_unit(&g_ChipInfo);
    if (unit == 0)
        return SF_ERR_ARG;
    rc = norm_range(addr, &len);
    if (rc != SF_OK)
        return rc;
    if (addr % unit != 0 || (len % unit != 0 && addr + len != size))
        return SF_ERR_ARG;
    if (mcode_SegmentErase == 0) { /* batchErase would silently "succeed" */
        sf_eprintf("Error: this chip has no segment erase; use sf_erase_chip().\n");
        return SF_ERR_FAILED;
    }

    sf_progress_begin(len);
    g_is_operation_on_going = true;
    g_is_operation_successful[s_idx] = true;
    ok = OpBegin(s_idx);
    if (ok) {
        for (off = 0; off < len && ok && !m_isCanceled;) {
            size_t n = 0;
            while (n < CHUNK && off + n * unit < len) {
                addrs[n] = (uintptr_t)(addr + off + n * unit);
                n++;
            }
            if (strstr(g_ChipInfo.Class, SUPPORT_WINBOND_W25Mxx_Large) != NULL)
                ok = SerialFlash_batchErase_W25Mxx_Large(addrs, n, s_idx) != 0;
            else
                ok = SerialFlash_batchErase(addrs, n, s_idx) != 0;
            off += n * unit;
            if (ok)
                progress_add(n * unit);
        }
        if (ok && !m_isCanceled && strstr(g_ChipInfo.Class, SUPPORT_MACRONIX_MX25Lxxx) != NULL) {
            TurnOFFVcc(s_idx); /* as BlazeUpdate() */
            Sleep(100);
            TurnONVcc(s_idx);
        }
    }
    g_is_operation_successful[s_idx] = ok ? true : false;
    OpEnd(s_idx, ok && !m_isCanceled);
    g_is_operation_on_going = false;
    rc = op_result(ok);
    sf_progress_end(rc == SF_OK);
    return rc;
}

/* Equivalent of `dpcmd --prog file --addr A`: the engine programs the buffer WITHOUT erasing first
 * (ProgramChip/PrepareProgramParameters have no erase step), so the target area must already be blank (0xFF)
 * or only have bits cleared. Fill byte is 0xFF; no lock area. */
int sf_program(const uint8_t* data, uint64_t len, uint64_t addr)
{
    int rc;
    uint8_t* buf;

    rc = op_enter();
    if (rc != SF_OK)
        return rc;
    if (data == NULL || len == 0 || addr >= g_ChipInfo.ChipSizeInByte || len > g_ChipInfo.ChipSizeInByte - addr
        || addr > UINT_MAX || len > ULONG_MAX || len > (uint64_t)SIZE_MAX)
        return SF_ERR_ARG;

    buf = (uint8_t*)malloc((size_t)len);
    if (buf == NULL)
        return SF_ERR_NOMEM;
    memcpy(buf, data, (size_t)len);
    free(pBufferforLoadedFile); /* engine-owned global; freed on the next load/close */
    pBufferforLoadedFile = buf;
    g_ulFileSize = (unsigned long)len;
    g_uiFileChecksum = CRC32(buf, (unsigned long)len); /* do_loadFile()/LoadFile() */

    /* do_Program() -> SaveProgContextChanges() */
    g_uiAddr = (unsigned int)addr;
    g_uiLen = 0; /* 0 = whole buffer */
    g_ucFill = 0xFF;
    memset(&LockAddrrange, 0, sizeof(LockAddrrange));

    sf_progress_begin(len);
    rc = engine_op(PROGRAM_CHIP);
    sf_progress_end(rc == SF_OK);
    free(pBufferforLoadedFile);
    pBufferforLoadedFile = NULL;
    g_ulFileSize = 0;
    return rc;
}

/* The engine's VERIFY_CONTENT only compares CRC32s, so read back and compare here to get the offset. */
int sf_verify(const uint8_t* data, uint64_t len, uint64_t addr, uint64_t* first_diff)
{
    int rc;
    uint64_t i;
    uint8_t* tmp;

    if (!s_open)
        return SF_ERR_NOT_OPEN;
    if (!s_have_chip)
        return SF_ERR_NO_CHIP;
    if (data == NULL || len == 0 || len > (uint64_t)SIZE_MAX)
        return SF_ERR_ARG;
    tmp = (uint8_t*)malloc((size_t)len);
    if (tmp == NULL)
        return SF_ERR_NOMEM;
    rc = sf_read(tmp, addr, len);
    if (rc == SF_OK && memcmp(tmp, data, (size_t)len) != 0) {
        for (i = 0; i < len && tmp[i] == data[i]; i++)
            ;
        if (first_diff)
            *first_diff = i;
        rc = SF_ERR_VERIFY;
    }
    free(tmp);
    return rc;
}

int sf_read_status(int index, uint8_t* value)
{
    static const unsigned char opcodes[3] = { 0x05, 0x35, 0x15 };
    unsigned char op, sr = 0;
    int rc, ok = 0;

    rc = op_enter();
    if (rc != SF_OK)
        return rc;
    if (index < 0 || index > 2 || value == NULL)
        return SF_ERR_ARG;
    op = opcodes[index];

    g_is_operation_on_going = true;
    g_is_operation_successful[s_idx] = true;
    if (OpBegin(s_idx)) {
        /* returns FlashCommand_TRUE (1) on success, SerialFlash_FALSE (-1) on USB failure */
        ok = FlashCommand_SendCommand_OneOutOneIn(&op, 1, &sr, 1, s_idx) == FlashCommand_TRUE;
        g_is_operation_successful[s_idx] = ok ? true : false;
    }
    OpEnd(s_idx, ok);
    g_is_operation_on_going = false;
    rc = m_isCanceled ? SF_ERR_CANCELLED : ok ? SF_OK : g_op_identify_failed ? SF_ERR_NO_CHIP : SF_ERR_USB;
    if (rc == SF_OK)
        *value = sr;
    return rc;
}

int sf_write_status(const uint8_t* values, int count)
{
    unsigned char cmd[3];
    int rc, ok = 0;

    rc = op_enter();
    if (rc != SF_OK)
        return rc;
    if (values == NULL || count < 1 || count > 2)
        return SF_ERR_ARG;
    cmd[0] = 0x01;
    cmd[1] = values[0];
    cmd[2] = count > 1 ? values[1] : 0;

    g_is_operation_on_going = true;
    g_is_operation_successful[s_idx] = true;
    if (OpBegin(s_idx)) {
        /* both senders return the byte count (>0) on success and SerialFlash_FALSE (-1) on USB failure */
        ok = SerialFlash_doWREN(s_idx) != SerialFlash_FALSE
            && FlashCommand_SendCommand_OutOnlyInstruction(cmd, 1 + count, s_idx) != SerialFlash_FALSE
            && SerialFlash_waitForWIP(s_idx);
        g_is_operation_successful[s_idx] = ok ? true : false;
    }
    OpEnd(s_idx, ok);
    g_is_operation_on_going = false;
    return m_isCanceled ? SF_ERR_CANCELLED : ok ? SF_OK : g_op_identify_failed ? SF_ERR_NO_CHIP : SF_ERR_USB;
}

void sf_cancel(void)
{
    SerialFlash_SetCancelOperationFlag();
}

/* ---- helpers (no hardware) ---- */
uint32_t sf_crc32(const uint8_t* data, size_t len)
{
    return CRC32((unsigned char*)data, (unsigned long)len);
}

/* The engine readers (ReadFile -> ReadBINFile/HexFileToBin/S19FileToBin) allocate pBufferforLoadedFile
 * themselves and set g_ulFileSize, so no chip or pre-sized buffer is needed. The result is copied out
 * and the engine's buffer released. */
int sf_load_file(const char* path, uint8_t** data, size_t* len)
{
    unsigned long sz = 0;
    uint8_t* copy;

    if (path == NULL || data == NULL || len == NULL)
        return SF_ERR_ARG;
    *data = NULL;
    *len = 0;
    g_bIsNANDFlash = false; /* ReadFile() would take the NAND reader otherwise */
    g_ulFileSize = 0;
    if (!ReadFile(path, NULL, &sz, 0xFF) || pBufferforLoadedFile == NULL || g_ulFileSize == 0) {
        free(pBufferforLoadedFile);
        pBufferforLoadedFile = NULL;
        g_ulFileSize = 0;
        return SF_ERR_FILE;
    }
    copy = (uint8_t*)malloc(g_ulFileSize);
    if (copy == NULL) {
        free(pBufferforLoadedFile);
        pBufferforLoadedFile = NULL;
        g_ulFileSize = 0;
        return SF_ERR_NOMEM;
    }
    memcpy(copy, pBufferforLoadedFile, g_ulFileSize);
    *data = copy;
    *len = g_ulFileSize;
    free(pBufferforLoadedFile);
    pBufferforLoadedFile = NULL;
    g_ulFileSize = 0;
    return SF_OK;
}

int sf_save_file(const char* path, const uint8_t* data, size_t len)
{
    if (path == NULL || (data == NULL && len != 0))
        return SF_ERR_ARG;
    /* raw binary regardless of extension (WriteFile() would pick hex/s19 from it) */
    return WriteBINFile(path, (unsigned char*)data, (unsigned long)len) ? SF_OK : SF_ERR_FILE;
}

void sf_free(void* p)
{
    free(p);
}
