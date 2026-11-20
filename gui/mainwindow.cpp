#include "mainwindow.h"

#include "chipselectdialog.h"
#include "configdialog.h"
#include "hexview.h"
#include "statusregdialog.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRegularExpression>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QTabWidget>
#include <QTime>
#include <QToolBar>
#include <QVBoxLayout>

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

quint32 crcOf(const QByteArray& d)
{
    return sf_crc32(reinterpret_cast<const uint8_t*>(d.constData()), size_t(d.size())); // pure helper, GUI thread ok
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

    m_crcTimer.setSingleShot(true);
    m_crcTimer.setInterval(300);
    connect(&m_crcTimer, &QTimer::timeout, this, [this] { updateFileLabel(crcOf(fileBuf()), true); });

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
    m_aSelect = makeAction(tr("Select"), "view-list-details", QStyle::SP_FileDialogContentsView, &MainWindow::actSelect);
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
    m_aConfig = makeAction(tr("Config"), "preferences-system", QStyle::SP_FileDialogDetailedView, &MainWindow::actConfig);
    m_aStatus = makeAction(tr("Status Reg"), "dialog-information", QStyle::SP_MessageBoxInformation, &MainWindow::actStatus);
    m_aCancel = makeAction(tr("Cancel"), "process-stop", QStyle::SP_DialogCancelButton, &MainWindow::actCancel);
    m_aGoto = makeAction(tr("&Go to address…"), "go-jump", QStyle::SP_ArrowRight, &MainWindow::actGoto);
    m_aGoto->setShortcut(QKeySequence::fromString(QStringLiteral("Ctrl+G")));
    m_aNextDiff = makeAction(tr("&Next difference"), "go-next", QStyle::SP_ArrowForward, &MainWindow::actNextDiff);
    m_aNextDiff->setShortcut(QKeySequence::fromString(QStringLiteral("F3")));
    m_aCopyToFile = makeAction(tr("&Copy chip buffer to file buffer"), "edit-copy", QStyle::SP_FileIcon,
                               &MainWindow::actCopyToFile);

    auto* tb = addToolBar(tr("Operations"));
    tb->setObjectName(QStringLiteral("toolbar"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tb->setIconSize(QSize(32, 32));
    for (QAction* a : {m_aDetect, m_aSelect, m_aFile, m_aSave, m_aBlank, m_aErase, m_aProg, m_aVerify, m_aBatch, m_aRead,
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
    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(m_aGoto);
    editMenu->addAction(m_aNextDiff);
    editMenu->addAction(m_aCopyToFile);
    auto* opMenu = menuBar()->addMenu(tr("&Operations"));
    for (QAction* a : {m_aDetect, m_aSelect, m_aBlank, m_aErase, m_aProg, m_aVerify, m_aBatch, m_aRead, m_aConfig, m_aStatus,
                       m_aCancel})
        opMenu->addAction(a);
    auto* about = new QAction(tr("&About"), this);
    connect(about, &QAction::triggered, this, &MainWindow::actAbout);
    menuBar()->addMenu(tr("&Help"))->addAction(about);

    // Buffers
    m_fileView = new HexView(true);
    m_chipView = new HexView(false);
    connect(m_fileView, &HexView::bufferEdited, this, [this] {
        m_chipView->clearCompare(); // its compare data is the pre-edit file
        m_crcTimer.start();
    });
    m_tabs = new QTabWidget;
    m_tabs->addTab(m_fileView, tr("File buffer"));
    m_tabs->addTab(m_chipView, tr("Chip buffer"));
    setCentralWidget(m_tabs);

    // Log + progress dock
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
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
    m_rangeLabel = new QLabel;
    for (QLabel* l : {m_progLabel, m_chipLabel, m_fileLabel, m_rangeLabel})
        statusBar()->addPermanentWidget(l);
    updateRangeLabel();
}

void MainWindow::updateActions()
{
    const bool idle = !m_busy;
    const bool chip = idle && m_haveChip;
    const bool file = !fileBuf().isEmpty();
    m_aDetect->setEnabled(idle);
    m_aFile->setEnabled(idle);
    m_aSave->setEnabled(idle && !chipBuf().isEmpty());
    m_aBlank->setEnabled(chip);
    m_aErase->setEnabled(chip);
    m_aRead->setEnabled(chip);
    m_aProg->setEnabled(chip && file);
    m_aVerify->setEnabled(chip && file);
    m_aBatch->setEnabled(chip && file);
    m_aSelect->setEnabled(idle);
    m_aConfig->setEnabled(idle); // not while busy: the range/config must not change under a running op
    m_aStatus->setEnabled(chip);
    m_aCopyToFile->setEnabled(idle && !chipBuf().isEmpty());
    m_fileView->setEditTriggers(idle ? QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                           QAbstractItemView::AnyKeyPressed
                                     : QAbstractItemView::NoEditTriggers);
    if (m_srDlg)
        m_srDlg->setBusy(m_busy);
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
    else if (op == Op::Program || op == Op::Batch || op == Op::Detect || op == Op::SelectChip)
        m_blank = false;

    // Chip data (Read, or read-back after a verify mismatch).
    if ((op == Op::Read || op == Op::Verify || op == Op::Batch) && !r.data.isEmpty()) {
        m_chipView->setBuffer(r.data);
        m_chipView->setBaseAddress(m_range.addr);
        m_fileView->clearCompare();
        m_tabs->setCurrentWidget(m_chipView);
    }

    if (op == Op::ListChips && ok)
        m_chips = r.chips;

    const QString name = QString::fromLatin1(CoreWorker::opName(op));
    if (op == Op::Detect || op == Op::SelectChip) {
        if (op == Op::Detect || ok) // a failed manual selection keeps the previous chip
            m_haveChip = ok;
        if (ok) {
            m_chipInfo = r.chip;
            QString vcc = r.chip.vcc_mv ? tr(" · %1 V").arg(r.chip.vcc_mv / 1000.0, 0, 'f', 1) : QString();
            if (r.prog.model[0]) // SelectChip only fills prog when it had to open the programmer
                m_progLabel->setText(tr("%1 · FW %2 · SN %3")
                                         .arg(QString::fromUtf8(r.prog.model), QString::fromUtf8(r.prog.firmware))
                                         .arg(r.prog.serial));
            m_chipLabel->setText(QStringLiteral("%1 · %2 · %3%4")
                                     .arg(chipName(), QString::fromUtf8(r.chip.manufacturer),
                                          sizeStr(r.chip.size_bytes), vcc));
            if (op == Op::Detect)
                onLog(LvlOk, tr("Detected %1 (%2, %3) on %4 FW %5")
                                 .arg(chipName(), QString::fromUtf8(r.chip.manufacturer), sizeStr(r.chip.size_bytes),
                                      QString::fromUtf8(r.prog.model), QString::fromUtf8(r.prog.firmware)));
            else
                onLog(LvlOk, tr("Chip %1 selected manually").arg(chipName()));
        } else if (op == Op::Detect) {
            m_chipLabel->setText(tr("No chip"));
            if (status == SF_ERR_NO_DEVICE || status == SF_ERR_ACCESS)
                m_progLabel->setText(tr("No programmer"));
        }
    } else if (op == Op::LoadFile && ok) {
        m_fileView->setBuffer(r.data);
        m_fileView->setBaseAddress(0);
        m_chipView->clearCompare();
        m_fileName = QFileInfo(r.path).fileName();
        updateFileLabel(r.crc, false);
        m_tabs->setCurrentWidget(m_fileView);
        onLog(LvlOk, tr("Loaded %1 (%2 bytes)").arg(r.path).arg(fileBuf().size()));
    } else if (op == Op::SaveFile && ok) {
        onLog(LvlOk, tr("Saved %1").arg(r.path));
    } else if (op == Op::ReadStatus && ok) {
        m_srLast = QByteArray(reinterpret_cast<const char*>(r.sr), 3);
        onLog(LvlOk, tr("Status: SR1=%1 SR2=%2 SR3=%3")
                         .arg(r.sr[0], 2, 16, QLatin1Char('0')).arg(r.sr[1], 2, 16, QLatin1Char('0'))
                         .arg(r.sr[2], 2, 16, QLatin1Char('0')));
        if (m_srDlg)
            m_srDlg->setValues(r.sr);
    } else if (ok) {
        onLog(LvlOk, tr("%1 OK").arg(name));
    }

    if (!ok) {
        if (status == SF_ERR_VERIFY && (op == Op::Verify || op == Op::Batch)) {
            const quint64 at = m_range.addr + r.firstDiff;
            onLog(LvlError, QString::asprintf("Verify failed at 0x%08llX", static_cast<unsigned long long>(at)));
            if (!r.data.isEmpty()) { // read-back succeeded: both buffers start at offset 0 <-> chip address range.addr
                m_fileView->setCompare(chipBuf(), 0);
                m_chipView->setCompare(fileBuf(), 0);
                m_fileView->gotoAddress(r.firstDiff);
                m_chipView->gotoAddress(at);
                m_tabs->setCurrentWidget(m_chipView);
            }
        } else
            handleError(op, status);
    }
    updateActions();

    if (op == Op::WriteStatus && m_srDlg) // always read back, also after a failed write
        startReadStatus();
    else if (op == Op::ListChips && ok)
        openChipSelect();
}

// ---- confirmations / texts ----

QString MainWindow::chipName() const { return QString::fromUtf8(m_chipInfo.type_name); }

const QByteArray& MainWindow::fileBuf() const { return m_fileView->buffer(); }
const QByteArray& MainWindow::chipBuf() const { return m_chipView->buffer(); }

HexView* MainWindow::currentView() const { return m_tabs->currentWidget() == m_chipView ? m_chipView : m_fileView; }

void MainWindow::updateRangeLabel()
{
    QString s;
    if (m_range.addr == 0 && m_range.len == 0)
        s = tr("whole chip");
    else if (m_range.len == 0)
        s = tr("%1 to end").arg(hexAddr(m_range.addr));
    else
        s = QStringLiteral("%1 +0x%2").arg(hexAddr(m_range.addr), QString::number(m_range.len, 16).toUpper());
    m_rangeLabel->setText(tr("Range: %1").arg(s));
}

void MainWindow::updateFileLabel(quint32 crc, bool modified)
{
    m_fileLabel->setText(QStringLiteral("%1 · %2 · CRC32 %3%4")
                             .arg(m_fileName, sizeStr(quint64(fileBuf().size())),
                                  QString::number(crc, 16).toUpper().rightJustified(8, QLatin1Char('0')),
                                  modified ? tr(" (edited)") : QString()));
}

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
    start(tr("Saving %1…").arg(path), [path, d = chipBuf()](CoreWorker* w) { w->saveFile(path, d); });
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
    const QString range = tr("file (%1 bytes) to chip address %2").arg(fileBuf().size()).arg(hexAddr(m_range.addr));
    const QString extra = m_blank ? QString() : tr("Prog does not erase; use Batch or Erase first.");
    if (!confirm(tr("Program"), tr("Program the file into the chip?"), range, extra))
        return;
    start(tr("Programming…"), [d = fileBuf(), r = m_range](CoreWorker* w) { w->program(d, r); });
}

void MainWindow::actVerify()
{
    start(tr("Verifying…"), [d = fileBuf(), r = m_range](CoreWorker* w) { w->verify(d, r); });
}

void MainWindow::actBatch()
{
    if (m_steps.isEmpty()) {
        QMessageBox::warning(this, tr("Batch"), tr("The batch sequence is empty. Choose its steps in Config."));
        return;
    }
    QStringList names;
    for (Op s : m_steps)
        names << QString::fromLatin1(CoreWorker::opName(s));
    const QString range = tr("%1 (file: %2 bytes)").arg(rangeText()).arg(fileBuf().size());
    if (!confirm(tr("Batch"), tr("Run %1?").arg(names.join(QStringLiteral(" → "))), range))
        return;
    start(tr("Batch…"), [d = fileBuf(), r = m_range, s = m_steps](CoreWorker* w) { w->batch(s, d, r); });
}

void MainWindow::actRead()
{
    start(tr("Reading…"), [r = m_range](CoreWorker* w) { w->read(r); });
}

void MainWindow::actConfig()
{
    ConfigDialog dlg(m_cfg, m_range, m_steps, m_haveChip ? &m_chipInfo : nullptr, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_cfg = dlg.config();
    m_range = dlg.range();
    m_steps = dlg.steps();
    // The worker applies it with sf_configure before its next chip operation.
    QMetaObject::invokeMethod(m_worker, [w = m_worker, cfg = m_cfg] { w->setConfig(cfg); }, Qt::QueuedConnection);
    updateRangeLabel();
    onLog(LvlInfo, tr("Configuration updated (%1)").arg(m_rangeLabel->text()));
}

void MainWindow::actSelect()
{
    if (m_chips.isEmpty())
        start(tr("Loading chip database…"), [](CoreWorker* w) { w->listChips(); }); // dialog opens on completion
    else
        openChipSelect();
}

void MainWindow::openChipSelect()
{
    ChipSelectDialog dlg(m_chips, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString type = dlg.selectedType();
    start(tr("Selecting %1…").arg(type), [type, cfg = m_cfg](CoreWorker* w) { w->selectChip(type, cfg); });
}

void MainWindow::startReadStatus()
{
    start(tr("Reading status registers…"), [](CoreWorker* w) { w->readStatus(); });
}

void MainWindow::actStatus()
{
    StatusRegDialog dlg(this);
    connect(&dlg, &StatusRegDialog::readRequested, this, &MainWindow::startReadStatus);
    connect(&dlg, &StatusRegDialog::writeRequested, this, [this](const QByteArray& v) {
        QString s = tr("Status write:");
        for (int i = 0; i < v.size(); ++i)
            s += QStringLiteral(" SR%1 %2 → %3")
                     .arg(i + 1)
                     .arg(i < m_srLast.size() ? QString::asprintf("%02X", uchar(m_srLast.at(i))) : QStringLiteral("??"),
                          QString::asprintf("%02X", uchar(v.at(i))));
        onLog(LvlInfo, s);
        start(tr("Writing status…"), [v](CoreWorker* w) { w->writeStatus(v); });
    });
    m_srDlg = &dlg; // modal: onFinished keeps running inside exec() and routes the results here
    startReadStatus();
    dlg.exec();
    m_srDlg = nullptr;
}

void MainWindow::actGoto()
{
    bool ok = false;
    QString t = QInputDialog::getText(this, tr("Go to address"), tr("Address (hex):"), QLineEdit::Normal, QString(), &ok)
                    .trimmed();
    if (!ok)
        return;
    if (t.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        t.remove(0, 2);
    const quint64 a = t.toULongLong(&ok, 16);
    if (!ok || !currentView()->gotoAddress(a))
        statusBar()->showMessage(tr("Address outside the buffer"), 3000);
}

void MainWindow::actNextDiff()
{
    if (!currentView()->nextDiff())
        statusBar()->showMessage(tr("No differences"), 3000);
}

void MainWindow::actCopyToFile()
{
    m_fileView->setBuffer(chipBuf());
    m_fileView->setBaseAddress(0);
    m_chipView->clearCompare();
    m_fileName = tr("(chip buffer)");
    updateFileLabel(crcOf(fileBuf()), false);
    m_tabs->setCurrentWidget(m_fileView);
    updateActions();
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
