// CoreWorker: the only place that calls sf_* (besides sf_cancel / pure helpers). Lives on its own QThread.
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>

#include "sf100core.h"

class CoreWorker : public QObject {
    Q_OBJECT
public:
    enum class Op { Detect, LoadFile, SaveFile, Blank, Erase, Program, Verify, Read, ReadStatus, WriteStatus, Batch };

    struct Range {
        quint64 addr = 0;
        quint64 len = 0; // 0 = to end (chip end for chip ops, data end for program/verify)
    };

    struct Result {
        QByteArray data;       // Read: chip data; Verify/Batch mismatch: read-back of the verified range; LoadFile: file
        sf_chip_info chip{};   // Detect
        sf_programmer_info prog{}; // Detect
        quint64 firstDiff = 0; // Verify mismatch: offset relative to Range.addr (as sf_verify reports it)
        quint8 sr[3]{};        // ReadStatus
        quint32 crc = 0;       // LoadFile: CRC32 of data
        QString path;          // LoadFile / SaveFile
    };

    explicit CoreWorker(QObject* parent = nullptr);

    static const char* opName(Op op);

public slots:
    void detect(sf_config cfg);
    void loadFile(QString path);
    void saveFile(QString path, QByteArray data);
    void blank(Range r);
    void erase(Range r);
    // Program/verify: the data buffer is used from its offset 0 and written to chip address r.addr
    // (like dpcmd --addr). r.len == 0 means the whole buffer; larger values are clamped to data.size().
    void program(QByteArray data, Range r);
    void verify(QByteArray data, Range r);
    void read(Range r);
    void readStatus();
    void writeStatus(QByteArray values);
    // Runs Erase/Blank/Program/Verify steps in order, stops at the first failure.
    void batch(QList<Op> steps, QByteArray data, Range r);
    // Call (BlockingQueued) before the thread quits: closes the device on the worker thread.
    void shutdown();

signals:
    void log(int level, QString text); // level = sf_log_level
    void progress(quint64 done, quint64 total);
    void finished(CoreWorker::Op op, int status, CoreWorker::Result r);

private:
    static void logTrampoline(void* user, sf_log_level level, const char* text);
    static void progressTrampoline(void* user, uint64_t done, uint64_t total);
    void onEngineLog(int level, const char* text);
    void emitLines(bool flushRemainder);
    void begin();
    void end(Op op, int status, Result& r);

    int doBlank(Range r);
    int doErase(Range r);
    int doProgram(const QByteArray& d, Range r);
    int doVerify(const QByteArray& d, Range r, Result& out);
    int doRead(Range r, QByteArray& out);

    bool m_open = false;
    bool m_hooked = false;
    QByteArray m_lineBuf;
    int m_lineLevel = SF_LOG_INFO;
    QElapsedTimer m_progTimer;
};

Q_DECLARE_METATYPE(CoreWorker::Op)
Q_DECLARE_METATYPE(CoreWorker::Range)
Q_DECLARE_METATYPE(CoreWorker::Result)
