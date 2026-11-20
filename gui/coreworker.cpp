#include "coreworker.h"

#include <QFile>
#include <new>

CoreWorker::CoreWorker(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<CoreWorker::Op>("CoreWorker::Op");
    qRegisterMetaType<CoreWorker::Range>("CoreWorker::Range");
    qRegisterMetaType<CoreWorker::Result>("CoreWorker::Result");
}

const char* CoreWorker::opName(Op op)
{
    switch (op) {
    case Op::Detect: return "Detect";
    case Op::LoadFile: return "Load file";
    case Op::SaveFile: return "Save file";
    case Op::Blank: return "Blank check";
    case Op::Erase: return "Erase";
    case Op::Program: return "Program";
    case Op::Verify: return "Verify";
    case Op::Read: return "Read";
    case Op::ReadStatus: return "Read status";
    case Op::WriteStatus: return "Write status";
    case Op::Batch: return "Batch";
    }
    return "?";
}

// ---- engine hooks (called on the worker thread) ----

void CoreWorker::logTrampoline(void* user, sf_log_level level, const char* text)
{
    static_cast<CoreWorker*>(user)->onEngineLog(int(level), text);
}

void CoreWorker::progressTrampoline(void* user, uint64_t done, uint64_t total)
{
    auto* w = static_cast<CoreWorker*>(user);
    // Throttle: the engine may report per page; the GUI needs ~20 updates/s.
    const bool last = total && done >= total;
    if (!last && w->m_progTimer.isValid() && w->m_progTimer.elapsed() < 50)
        return;
    w->m_progTimer.start();
    emit w->progress(done, total);
}

void CoreWorker::onEngineLog(int level, const char* text)
{
    if (level != m_lineLevel) { // keep one level per emitted line
        emitLines(true);
        m_lineLevel = level;
    }
    m_lineBuf.append(text);
    emitLines(false);
}

// Emits every complete line in the buffer; with flushRemainder also the unterminated tail.
void CoreWorker::emitLines(bool flushRemainder)
{
    for (;;) {
        int nl = m_lineBuf.indexOf('\n');
        QByteArray line;
        if (nl >= 0) {
            line = m_lineBuf.left(nl);
            m_lineBuf.remove(0, nl + 1);
        } else if (flushRemainder) {
            line = m_lineBuf;
            m_lineBuf.clear();
        } else {
            return;
        }
        const QString s = QString::fromUtf8(line).trimmed();
        if (!s.isEmpty())
            emit log(m_lineLevel, s);
        if (nl < 0)
            return;
    }
}

void CoreWorker::begin()
{
    if (!m_hooked) { // sf_set_* are sf_* calls: must happen on this thread, so not in the constructor
        sf_set_log_handler(&CoreWorker::logTrampoline, this);
        sf_set_progress_handler(&CoreWorker::progressTrampoline, this);
        m_hooked = true;
    }
    m_progTimer.invalidate();
}

void CoreWorker::end(Op op, int status, Result& r)
{
    emitLines(true);
    emit finished(op, status, r);
}

void CoreWorker::shutdown()
{
    if (m_open)
        sf_close();
    m_open = false;
    if (m_hooked) { // don't leave the engine holding a pointer to a soon-deleted worker
        sf_set_log_handler(nullptr, nullptr);
        sf_set_progress_handler(nullptr, nullptr);
        m_hooked = false;
    }
}

// ---- operations ----

static quint64 dataLen(const QByteArray& d, CoreWorker::Range r)
{
    const quint64 n = quint64(d.size());
    return (r.len && r.len < n) ? r.len : n;
}

int CoreWorker::doBlank(Range r) { return sf_blank_check(r.addr, r.len); }

int CoreWorker::doErase(Range r)
{
    if (r.addr == 0 && r.len == 0)
        return sf_erase_chip();
    return sf_erase_range(r.addr, r.len);
}

int CoreWorker::doProgram(const QByteArray& d, Range r)
{
    const quint64 len = dataLen(d, r);
    if (len == 0)
        return SF_ERR_ARG;
    return sf_program(reinterpret_cast<const uint8_t*>(d.constData()), len, r.addr);
}

int CoreWorker::doRead(Range r, QByteArray& out)
{
    sf_chip_info c;
    int st = sf_get_chip(&c);
    if (st != SF_OK)
        return st;
    if (r.addr >= c.size_bytes)
        return SF_ERR_ARG;
    const quint64 len = r.len ? r.len : c.size_bytes - r.addr;
    if (len > c.size_bytes - r.addr)
        return SF_ERR_ARG;
    try {
        out.resize(qsizetype(len));
    } catch (const std::bad_alloc&) {
        out.clear();
        return SF_ERR_NOMEM;
    }
    return sf_read(reinterpret_cast<uint8_t*>(out.data()), r.addr, len);
}

int CoreWorker::doVerify(const QByteArray& d, Range r, Result& out)
{
    const quint64 len = dataLen(d, r);
    if (len == 0)
        return SF_ERR_ARG;
    int st = sf_verify(reinterpret_cast<const uint8_t*>(d.constData()), len, r.addr, &out.firstDiff);
    if (st == SF_ERR_VERIFY) { // hand the chip's contents back so the GUI can show them
        QByteArray rb;
        if (doRead(Range{r.addr, len}, rb) == SF_OK)
            out.data = rb;
    }
    return st;
}

void CoreWorker::detect(sf_config cfg)
{
    begin();
    Result r;
    // Always reopen: picks up a replugged or swapped programmer (sf_open closes any previous session).
    int st = sf_open(0);
    m_open = (st == SF_OK);
    if (st == SF_OK) st = sf_get_programmer_info(&r.prog);
    if (st == SF_OK) st = sf_configure(&cfg);
    if (st == SF_OK) st = sf_detect_chip(&r.chip);
    end(Op::Detect, st, r);
}

void CoreWorker::loadFile(QString path)
{
    begin();
    Result r;
    r.path = path;
    uint8_t* buf = nullptr;
    size_t len = 0;
    int st = sf_load_file(QFile::encodeName(path).constData(), &buf, &len);
    if (st == SF_OK) {
        r.data = QByteArray(reinterpret_cast<const char*>(buf), qsizetype(len)); // copy, then release the C buffer
        r.crc = sf_crc32(buf, len);
    }
    if (buf)
        sf_free(buf);
    end(Op::LoadFile, st, r);
}

void CoreWorker::saveFile(QString path, QByteArray data)
{
    begin();
    Result r;
    r.path = path;
    int st = sf_save_file(QFile::encodeName(path).constData(), reinterpret_cast<const uint8_t*>(data.constData()),
                          size_t(data.size()));
    end(Op::SaveFile, st, r);
}

void CoreWorker::blank(Range rg)
{
    begin();
    Result r;
    end(Op::Blank, doBlank(rg), r);
}

void CoreWorker::erase(Range rg)
{
    begin();
    Result r;
    end(Op::Erase, doErase(rg), r);
}

void CoreWorker::program(QByteArray data, Range rg)
{
    begin();
    Result r;
    end(Op::Program, doProgram(data, rg), r);
}

void CoreWorker::verify(QByteArray data, Range rg)
{
    begin();
    Result r;
    int st = doVerify(data, rg, r);
    end(Op::Verify, st, r);
}

void CoreWorker::read(Range rg)
{
    begin();
    Result r;
    int st = doRead(rg, r.data);
    if (st != SF_OK)
        r.data.clear();
    end(Op::Read, st, r);
}

void CoreWorker::readStatus()
{
    begin();
    Result r;
    int st = SF_OK;
    for (int i = 0; i < 3 && st == SF_OK; ++i)
        st = sf_read_status(i, &r.sr[i]);
    end(Op::ReadStatus, st, r);
}

void CoreWorker::writeStatus(QByteArray values)
{
    begin();
    Result r;
    int st = SF_ERR_ARG;
    if (values.size() == 1 || values.size() == 2)
        st = sf_write_status(reinterpret_cast<const uint8_t*>(values.constData()), int(values.size()));
    end(Op::WriteStatus, st, r);
}

void CoreWorker::batch(QList<Op> steps, QByteArray data, Range rg)
{
    begin();
    Result r;
    int st = SF_OK;
    for (Op step : steps) {
        emitLines(true);
        emit log(SF_LOG_INFO, QStringLiteral("Batch: %1").arg(QString::fromLatin1(opName(step))));
        switch (step) {
        case Op::Erase: st = doErase(rg); break;
        case Op::Blank: st = doBlank(rg); break;
        case Op::Program: st = doProgram(data, rg); break;
        case Op::Verify: st = doVerify(data, rg, r); break;
        default: st = SF_ERR_ARG; break;
        }
        if (st != SF_OK)
            break;
    }
    end(Op::Batch, st, r);
}
