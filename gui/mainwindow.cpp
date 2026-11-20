#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRegularExpression>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTime>
#include <QToolBar>
#include <QVBoxLayout>
#include <cstdio>

using Op = CoreWorker::Op;
using Range = CoreWorker::Range;

namespace {
const int LvlInfo = SF_LOG_INFO;
const int LvlWarn = SF_LOG_WARN;
const int LvlError = SF_LOG_ERROR;
const int LvlOk = 4; // GUI-only: success (green)

QString sizeStr(quint64 bytes)
{
    return QLocale().formattedDataSize(qint64(bytes), 1, QLocale::DataSizeIecFormat);
}

QString hexAddr(quint64 a)
{
    return QString::asprintf("0x%08llX", static_cast<unsigned long long>(a));
}

// WP3.1 placeholder: plain hex dump of the first 64 KiB. WP3.2 replaces this with a real hex view.
void showBuffer(QPlainTextEdit* view, const QByteArray& d)
{
    if (d.isEmpty()) {
        view->setPlainText(QStringLiteral("(empty)"));
        return;
    }
    const int n = int(qMin<qsizetype>(d.size(), 64 * 1024));
    const auto* b = reinterpret_cast<const uchar*>(d.constData());
    QByteArray out;
    out.reserve(n / 16 * 80 + 64);
    char line[128];
    for (int i = 0; i < n; i += 16) {
        int p = std::snprintf(line, sizeof line, "%08X ", i);
        for (int j = 0; j < 16; ++j) {
            if (i + j < n)
                p += std::snprintf(line + p, sizeof line - size_t(p), " %02X", b[i + j]);
            else
                p += std::snprintf(line + p, sizeof line - size_t(p), "   ");
            if (j == 7)
                line[p++] = ' ';
        }
        line[p++] = ' ';
        line[p++] = '|';
        for (int j = 0; j < 16 && i + j < n; ++j)
            line[p++] = (b[i + j] >= 32 && b[i + j] < 127) ? char(b[i + j]) : '.';
        line[p++] = '|';
        line[p++] = '\n';
        out.append(line, p);
    }
    QString text = QString::fromLatin1(out);
    if (d.size() > n)
        text += QStringLiteral("… %1 more bytes").arg(d.size() - n);
    view->setPlainText(text);
}
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    sf_config_default(&m_cfg); // pure helper, no hardware

    buildUi();

    // Move to the worker thread BEFORE connecting so the connections below are queued.
    m_worker = new CoreWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &CoreWorker::log, this, &MainWindow::onLog);
    connect(m_worker, &CoreWorker::progress, this, &MainWindow::onProgress);
    connect(m_worker, &CoreWorker::finished, this, &MainWindow::onFinished);
    m_thread.start();

    m_tick.setInterval(200);
    connect(&m_tick, &QTimer::timeout, this, &MainWindow::updateRate);

    updateActions();
}

MainWindow::~MainWindow()
{
    sf_cancel(); // thread-safe; lets a running op end quickly
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->shutdown(); }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

QAction* MainWindow::makeAction(const QString& text, const char* themeIcon, int fallbackPixmap,
                                void (MainWindow::*slot)())
{
    const QIcon icon = QIcon::fromTheme(QString::fromLatin1(themeIcon),
                                        style()->standardIcon(QStyle::StandardPixmap(fallbackPixmap)));
    auto* a = new QAction(icon, text, this);
    if (slot)
        connect(a, &QAction::triggered, this, slot);
    return a;
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("SF100 GUI"));
    resize(1100, 750);

    m_aDetect = makeAction(tr("Detect"), "edit-find", QStyle::SP_BrowserReload, &MainWindow::actDetect);
    m_aFile = makeAction(tr("&Open…"), "document-open", QStyle::SP_DialogOpenButton, &MainWindow::actFile);
    m_aFile->setIconText(tr("File"));
    m_aFile->setShortcut(QKeySequence::Open);
    m_aSave = makeAction(tr("&Save Chip Buffer…"), "document-save", QStyle::SP_DialogSaveButton,
                         &MainWindow::actSave);
    m_aSave->setIconText(tr("Save"));
    m_aSave->setShortcut(QKeySequence::Save);
    m_aBlank = makeAction(tr("Blank"), "edit-clear", QStyle::SP_DialogResetButton, &MainWindow::actBlank);
    m_aErase = makeAction(tr("Erase"), "edit-delete", QStyle::SP_TrashIcon, &MainWindow::actErase);
    m_aProg = makeAction(tr("Prog"), "document-save-as", QStyle::SP_ArrowDown, &MainWindow::actProg);
    m_aVerify = makeAction(tr("Verify"), "dialog-ok-apply", QStyle::SP_DialogApplyButton, &MainWindow::actVerify);
    m_aBatch = makeAction(tr("Batch"), "system-run", QStyle::SP_MediaPlay, &MainWindow::actBatch);
    m_aRead = makeAction(tr("Read"), "go-up", QStyle::SP_ArrowUp, &MainWindow::actRead);
    m_aConfig = makeAction(tr("Config"), "preferences-system", QStyle::SP_FileDialogDetailedView, nullptr);
    m_aStatus = makeAction(tr("Status Reg"), "dialog-information", QStyle::SP_MessageBoxInformation, nullptr);
    m_aCancel = makeAction(tr("Cancel"), "process-stop", QStyle::SP_DialogCancelButton, &MainWindow::actCancel);
    m_aConfig->setToolTip(tr("coming in WP3.2"));
    m_aStatus->setToolTip(tr("coming in WP3.2"));

    auto* tb = addToolBar(tr("Operations"));
    tb->setObjectName(QStringLiteral("toolbar"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tb->setIconSize(QSize(32, 32));
    for (QAction* a : {m_aDetect, m_aFile, m_aSave, m_aBlank, m_aErase, m_aProg, m_aVerify, m_aBatch, m_aRead,
                       m_aConfig, m_aStatus, m_aCancel}) {
        if (a == m_aBlank || a == m_aConfig || a == m_aCancel)
            tb->addSeparator();
        tb->addAction(a);
    }

    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(m_aFile);
    fileMenu->addAction(m_aSave);
    fileMenu->addSeparator();
    auto* quit = new QAction(tr("&Quit"), this);
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);
    fileMenu->addAction(quit);
    auto* opMenu = menuBar()->addMenu(tr("&Operations"));
    for (QAction* a : {m_aDetect, m_aBlank, m_aErase, m_aProg, m_aVerify, m_aBatch, m_aRead, m_aConfig, m_aStatus,
                       m_aCancel})
        opMenu->addAction(a);
    auto* about = new QAction(tr("&About"), this);
    connect(about, &QAction::triggered, this, &MainWindow::actAbout);
    menuBar()->addMenu(tr("&Help"))->addAction(about);

    // Buffers
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_fileView = new QPlainTextEdit;
    m_chipView = new QPlainTextEdit;
    for (QPlainTextEdit* v : {m_fileView, m_chipView}) {
        v->setReadOnly(true);
        v->setFont(mono);
        v->setLineWrapMode(QPlainTextEdit::NoWrap);
    }
    m_tabs = new QTabWidget;
    m_tabs->addTab(m_fileView, tr("File buffer"));
    m_tabs->addTab(m_chipView, tr("Chip buffer"));
    setCentralWidget(m_tabs);

    // Log + progress dock
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);
    m_log->setFont(mono);
    m_bar = new QProgressBar;
    m_bar->setRange(0, 1000);
    m_bar->setValue(0);
    m_rateLabel = new QLabel;
    auto* bottom = new QWidget;
    auto* vl = new QVBoxLayout(bottom);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->addWidget(m_log, 1);
    auto* hl = new QHBoxLayout;
    hl->addWidget(m_bar, 1);
    hl->addWidget(m_rateLabel);
    vl->addLayout(hl);
    auto* dock = new QDockWidget(tr("Log"), this);
    dock->setObjectName(QStringLiteral("logdock"));
    dock->setFeatures(QDockWidget::DockWidgetMovable); // not closable
    dock->setWidget(bottom);
    addDockWidget(Qt::BottomDockWidgetArea, dock);

    // Status bar
    m_progLabel = new QLabel(tr("No programmer"));
    m_chipLabel = new QLabel(tr("No chip"));
    m_fileLabel = new QLabel(tr("No file"));
    for (QLabel* l : {m_progLabel, m_chipLabel, m_fileLabel})
        statusBar()->addPermanentWidget(l);
}

void MainWindow::updateActions()
{
    const bool idle = !m_busy;
    const bool chip = idle && m_haveChip;
    const bool file = !m_file.isEmpty();
    m_aDetect->setEnabled(idle);
    m_aFile->setEnabled(idle);
    m_aSave->setEnabled(idle && !m_chip.isEmpty());
    m_aBlank->setEnabled(chip);
    m_aErase->setEnabled(chip);
    m_aRead->setEnabled(chip);
    m_aProg->setEnabled(chip && file);
    m_aVerify->setEnabled(chip && file);
    m_aBatch->setEnabled(chip && file);
    m_aConfig->setEnabled(false); // WP3.2
    m_aStatus->setEnabled(false); // WP3.2
    m_aCancel->setEnabled(m_busy);
}

// ---- running ops ----

void MainWindow::start(const QString& what, std::function<void(CoreWorker*)> fn)
{
    m_busy = true;
    m_done = 0;
    m_opTimer.start();
    m_stepTimer.start();
    m_tick.start();
    m_bar->setRange(0, 0); // busy until the engine reports a total
    updateRate();
    updateActions();
    onLog(LvlInfo, what);
    QMetaObject::invokeMethod(m_worker, [w = m_worker, fn] { fn(w); }, Qt::QueuedConnection);
}

void MainWindow::updateRate()
{
    QString s = tr("elapsed %1 s").arg(m_opTimer.isValid() ? m_opTimer.elapsed() / 1000.0 : 0.0, 0, 'f', 1);
    const double step = m_stepTimer.isValid() ? m_stepTimer.elapsed() / 1000.0 : 0.0;
    if (m_done > 0 && step > 0.05)
        s.prepend(QStringLiteral("%1 KB/s · ").arg(double(m_done) / 1024.0 / step, 0, 'f', 0));
    m_rateLabel->setText(s);
}

void MainWindow::onProgress(quint64 done, quint64 total)
{
    if (done < m_done) // a new batch step started counting from zero
        m_stepTimer.restart();
    m_done = done;
    if (total == 0) {
        m_bar->setRange(0, 0);
    } else {
        m_bar->setRange(0, 1000);
        m_bar->setValue(int(qMin<quint64>(done, total) * 1000 / total));
    }
}

void MainWindow::onLog(int level, const QString& text)
{
    const char* color = nullptr;
    switch (level) {
    case SF_LOG_ERROR: color = "#d32f2f"; break;
    case SF_LOG_WARN: color = "#e69100"; break;
    case LvlOk: color = "#2e9d3e"; break;
    default: break;
    }
    const QString style = color ? QStringLiteral("color:%1;").arg(QLatin1String(color)) : QString();
    m_log->appendHtml(QStringLiteral("<span style=\"white-space:pre-wrap;%1\">%2  %3</span>")
                          .arg(style, QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                               text.toHtmlEscaped()));
}

void MainWindow::handleError(Op op, int status)
{
    if (status == SF_ERR_CANCELLED) {
        onLog(LvlWarn, tr("Cancelled"));
        return;
    }
    onLog(LvlError, tr("%1 failed: %2").arg(QString::fromLatin1(CoreWorker::opName(op)),
                                           QString::fromUtf8(sf_strerror(status))));
    if (status == SF_ERR_NO_DEVICE) {
        onLog(LvlWarn, tr("Connect an SF100/SF600 programmer and press Detect"));
    } else if (status == SF_ERR_ACCESS) {
        QMessageBox::warning(
            this, tr("USB access denied"),
            tr("No permission to access the programmer.\n\n"
               "On Linux, install the udev rules:\n"
               "  1. Copy 60-dediprog.rules to /etc/udev/rules.d/\n"
               "  2. sudo udevadm control --reload-rules && sudo udevadm trigger\n"
               "  3. Unplug and replug the programmer, then press Detect."));
    }
}

void MainWindow::onFinished(Op op, int status, const CoreWorker::Result& r)
{
    const bool ok = status == SF_OK;
    m_busy = false;
    m_tick.stop();
    m_bar->setRange(0, 1000);
    m_bar->setValue(ok ? 1000 : 0);
    updateRate();

    if (op == Op::Erase || op == Op::Blank)
        m_blank = ok;
    else if (op == Op::Program || op == Op::Batch || op == Op::Detect)
        m_blank = false;

    // Chip data (Read, or read-back after a verify mismatch).
    if ((op == Op::Read || op == Op::Verify || op == Op::Batch) && !r.data.isEmpty()) {
        m_chip = r.data;
        showBuffer(m_chipView, m_chip);
        m_tabs->setCurrentWidget(m_chipView);
    }

    const QString name = QString::fromLatin1(CoreWorker::opName(op));
    if (op == Op::Detect) {
        m_haveChip = ok;
        if (ok) {
            m_chipInfo = r.chip;
            QString vcc = r.chip.vcc_mv ? tr(" · %1 V").arg(r.chip.vcc_mv / 1000.0, 0, 'f', 1) : QString();
            m_progLabel->setText(tr("%1 · FW %2 · SN %3")
                                     .arg(QString::fromUtf8(r.prog.model), QString::fromUtf8(r.prog.firmware))
                                     .arg(r.prog.serial));
            m_chipLabel->setText(QStringLiteral("%1 · %2 · %3%4")
                                     .arg(chipName(), QString::fromUtf8(r.chip.manufacturer),
                                          sizeStr(r.chip.size_bytes), vcc));
            onLog(LvlOk, tr("Detected %1 (%2, %3) on %4 FW %5")
                             .arg(chipName(), QString::fromUtf8(r.chip.manufacturer), sizeStr(r.chip.size_bytes),
                                  QString::fromUtf8(r.prog.model), QString::fromUtf8(r.prog.firmware)));
        } else {
            m_chipLabel->setText(tr("No chip"));
            if (status == SF_ERR_NO_DEVICE || status == SF_ERR_ACCESS)
                m_progLabel->setText(tr("No programmer"));
        }
    } else if (op == Op::LoadFile && ok) {
        m_file = r.data;
        m_fileLabel->setText(QStringLiteral("%1 · %2 · CRC32 %3")
                                 .arg(QFileInfo(r.path).fileName(), sizeStr(quint64(m_file.size())),
                                      QString::number(r.crc, 16).toUpper().rightJustified(8, QLatin1Char('0'))));
        showBuffer(m_fileView, m_file);
        m_tabs->setCurrentWidget(m_fileView);
        onLog(LvlOk, tr("Loaded %1 (%2 bytes)").arg(r.path).arg(m_file.size()));
    } else if (op == Op::SaveFile && ok) {
        onLog(LvlOk, tr("Saved %1").arg(r.path));
    } else if (ok) {
        onLog(LvlOk, tr("%1 OK").arg(name));
    }

    if (!ok) {
        if (status == SF_ERR_VERIFY && (op == Op::Verify || op == Op::Batch))
            onLog(LvlError, QString::asprintf("Verify failed at 0x%08llX",
                                              static_cast<unsigned long long>(m_range.addr + r.firstDiff)));
        else
            handleError(op, status);
    }
    updateActions();
}

// ---- confirmations / texts ----

QString MainWindow::chipName() const { return QString::fromUtf8(m_chipInfo.type_name); }

QString MainWindow::rangeText() const
{
    if (m_range.len == 0)
        return m_range.addr == 0 ? tr("whole chip (%1)").arg(sizeStr(m_chipInfo.size_bytes))
                                 : tr("%1 to end of chip").arg(hexAddr(m_range.addr));
    return tr("%1, %2 bytes").arg(hexAddr(m_range.addr)).arg(m_range.len);
}

bool MainWindow::confirm(const QString& title, const QString& what, const QString& range, const QString& extra)
{
    QString text = tr("%1\n\nChip: %2\nRange: %3\n\nExisting data in this range will be overwritten.")
                       .arg(what, chipName(), range);
    if (!extra.isEmpty())
        text += QStringLiteral("\n\n") + extra;
    return QMessageBox::question(this, title, text) == QMessageBox::Yes;
}

// ---- actions ----

void MainWindow::actDetect()
{
    start(tr("Detecting…"), [cfg = m_cfg](CoreWorker* w) { w->detect(cfg); });
}

void MainWindow::actFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open file"), QString(),
                                                      tr("Flash images (*.bin *.hex *.s19);;All files (*)"));
    if (path.isEmpty())
        return;
    start(tr("Loading %1…").arg(path), [path](CoreWorker* w) { w->loadFile(path); });
}

void MainWindow::actSave()
{
    QString base = m_haveChip ? chipName() : QStringLiteral("chip");
    base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._+-]")), QStringLiteral("_"));
    const QString path = QFileDialog::getSaveFileName(this, tr("Save chip buffer"), base + QStringLiteral("_read.bin"),
                                                      tr("Binary (*.bin);;All files (*)"));
    if (path.isEmpty())
        return;
    start(tr("Saving %1…").arg(path), [path, d = m_chip](CoreWorker* w) { w->saveFile(path, d); });
}

void MainWindow::actBlank()
{
    start(tr("Blank check…"), [r = m_range](CoreWorker* w) { w->blank(r); });
}

void MainWindow::actErase()
{
    if (!confirm(tr("Erase"), tr("Erase the selected range?"), rangeText()))
        return;
    start(tr("Erasing…"), [r = m_range](CoreWorker* w) { w->erase(r); });
}

void MainWindow::actProg()
{
    const QString range = tr("file (%1 bytes) to chip address %2").arg(m_file.size()).arg(hexAddr(m_range.addr));
    const QString extra = m_blank ? QString() : tr("Prog does not erase; use Batch or Erase first.");
    if (!confirm(tr("Program"), tr("Program the file into the chip?"), range, extra))
        return;
    start(tr("Programming…"), [d = m_file, r = m_range](CoreWorker* w) { w->program(d, r); });
}

void MainWindow::actVerify()
{
    start(tr("Verifying…"), [d = m_file, r = m_range](CoreWorker* w) { w->verify(d, r); });
}

void MainWindow::actBatch()
{
    const QString range = tr("%1 (file: %2 bytes)").arg(rangeText()).arg(m_file.size());
    if (!confirm(tr("Batch"), tr("Run Erase → Blank → Prog → Verify?"), range))
        return;
    start(tr("Batch…"), [d = m_file, r = m_range](CoreWorker* w) {
        w->batch({Op::Erase, Op::Blank, Op::Program, Op::Verify}, d, r);
    });
}

void MainWindow::actRead()
{
    start(tr("Reading…"), [r = m_range](CoreWorker* w) { w->read(r); });
}

void MainWindow::actCancel()
{
    sf_cancel(); // the one sf_* call that is safe from the GUI thread
    onLog(LvlWarn, tr("Cancelling…"));
}

void MainWindow::actAbout()
{
    QMessageBox::about(this, tr("About SF100 GUI"),
                       tr("<b>SF100 GUI</b> %1<br>Uses libsf100core.<br>Qt %2")
                           .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion())));
}
