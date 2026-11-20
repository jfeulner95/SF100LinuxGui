# sf100gui architecture

C++17, Qt 6 Widgets (minimum Qt 6.4 — Ubuntu 24.04's qt6-base-dev). Links `sf100core` (C API in
`src/core/sf100core.h`). Built only when Qt6 is found (`-DSF100_BUILD_GUI=ON` default, skipped with a
message if Qt6 is missing).

## Threading

The engine is a single-session, non-reentrant C library. Rule: **every `sf_*` call except `sf_cancel()`
runs on one worker thread.**

```
GUI thread                                   worker QThread
MainWindow ──invokeMethod(Queued)──────────▶ CoreWorker slots (detect, read, program, …)
           ◀──signals (Qt::QueuedConnection)─ log(level,text), progress(done,total),
                                              finished(Op, status, Result)
Cancel button ──▶ sf_cancel()  (thread-safe, called directly)
```

- `CoreWorker` installs `sf_set_log_handler` / `sf_set_progress_handler` with static trampolines that
  emit signals. Signals cross threads queued, so widgets are only touched on the GUI thread.
- Exactly one operation at a time: MainWindow disables all operation actions while busy (Cancel enabled).
- Data buffers are `QByteArray` (implicitly shared, cheap to pass by value across threads).
- `finished` carries `Result { QByteArray data; sf_chip_info chip; sf_programmer_info prog;
  quint64 firstDiff; uint8_t sr[3]; … }` — only the fields the op fills.

## Buffers

- **File buffer**: what was loaded from disk (or a chip read copied into it via "Read → use as file").
- **Chip buffer**: last data read from the chip (Read, or read-back during Verify).
- CRC32 (`sf_crc32`) of the file buffer is shown in the status bar.

## Operations (toolbar, in this order)

Detect · File (load) · Save · Blank · Erase · Prog · Verify · Batch · Read · Config · Status Reg · Cancel

| Op | Core calls | Notes |
|----|-----------|-------|
| Detect | `sf_open` (if not open) → `sf_get_programmer_info` → `sf_configure` → `sf_detect_chip` | auto-opens device 1 |
| File | `sf_load_file` (on worker: S19 reader allocates 256 MB) | |
| Blank | `sf_blank_check(range)` | |
| Erase | `sf_erase_chip`, or `sf_erase_range(range)` when a partial range is configured | |
| Prog | `sf_program(file, range)` | engine does NOT erase first |
| Verify | `sf_verify(file, range, &firstDiff)` | on mismatch: also read range → chip buffer, jump hex view to diff |
| Batch | user-ordered subset of Erase → Blank → Prog → Verify, stops at first failure | |
| Read | `sf_read(range)` → chip buffer | |
| Status Reg | `sf_read_status` ×3, `sf_write_status` | generic JEDEC bit names, raw hex always editable |

Range = Config dialog start address + length (0 = to end of chip / whole file).

## Error handling

`status != SF_OK` → red log line with `sf_strerror`. `SF_ERR_ACCESS` additionally shows a dialog with
the udev instructions (Linux). `SF_ERR_NO_DEVICE` → "connect the programmer". `SF_ERR_CANCELLED` → yellow.
