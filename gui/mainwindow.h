#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <functional>

#include "coreworker.h"

class QAction;
class QLabel;
class HexView;
class QPlainTextEdit;
class QProgressBar;
class QTabWidget;
class StatusRegDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onLog(int level, const QString& text);
    void onProgress(quint64 done, quint64 total);
    void onFinished(CoreWorker::Op op, int status, const CoreWorker::Result& r);
    void updateRate();

    void actDetect();
    void actFile();
    void actSave();
    void actBlank();
    void actErase();
    void actProg();
    void actVerify();
    void actBatch();
    void actRead();
    void actConfig();
    void actSelect();
    void actStatus();
    void actGoto();
    void actNextDiff();
    void actCopyToFile();
    void actCancel();
    void actAbout();

private:
    QAction* makeAction(const QString& text, const char* themeIcon, int fallbackPixmap, void (MainWindow::*slot)());
    void buildUi();
    void updateActions();
    void start(const QString& what, std::function<void(CoreWorker*)> fn);
    bool confirm(const QString& title, const QString& what, const QString& range, const QString& extra = QString());
    QString rangeText() const;
    QString chipName() const;
    void updateRangeLabel();
    void updateFileLabel(quint32 crc, bool modified);
    void openChipSelect();
    void startReadStatus();
    HexView* currentView() const;
    const QByteArray& fileBuf() const;
    const QByteArray& chipBuf() const;
    void handleError(CoreWorker::Op op, int status);

    QThread m_thread;
    CoreWorker* m_worker = nullptr;

    // Edited by the Config dialog.
    sf_config m_cfg;
    CoreWorker::Range m_range; // whole chip by default
    QList<CoreWorker::Op> m_steps{CoreWorker::Op::Erase, CoreWorker::Op::Blank, CoreWorker::Op::Program,
                                  CoreWorker::Op::Verify}; // Batch sequence

    // The file/chip buffers live in m_fileView / m_chipView (single source of truth): fileBuf() / chipBuf().
    QString m_fileName;
    QTimer m_crcTimer; // debounces the CRC refresh after hex edits
    QVector<sf_chip_info> m_chips; // chip database, loaded on first Select
    StatusRegDialog* m_srDlg = nullptr; // set while the (modal) dialog runs: ReadStatus/WriteStatus results go there
    QByteArray m_srLast; // last SR1..SR3 read, for the old -> new log
    bool m_haveChip = false;
    bool m_blank = false; // chip known blank (last Erase/Blank succeeded, nothing programmed since)
    sf_chip_info m_chipInfo{};
    bool m_busy = false;

    QElapsedTimer m_opTimer, m_stepTimer;
    QTimer m_tick;
    quint64 m_done = 0;

    QAction *m_aDetect, *m_aFile, *m_aSave, *m_aBlank, *m_aErase, *m_aProg, *m_aVerify, *m_aBatch, *m_aRead,
        *m_aSelect, *m_aConfig, *m_aStatus, *m_aCancel, *m_aGoto, *m_aNextDiff, *m_aCopyToFile;
    QTabWidget* m_tabs;
    HexView *m_fileView, *m_chipView;
    QPlainTextEdit* m_log;
    QProgressBar* m_bar;
    QLabel *m_rateLabel, *m_progLabel, *m_chipLabel, *m_fileLabel, *m_rangeLabel;
};
