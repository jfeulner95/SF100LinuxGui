#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QThread>
#include <QTimer>
#include <functional>

#include "coreworker.h"

class QAction;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QTabWidget;

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
    void handleError(CoreWorker::Op op, int status);

    QThread m_thread;
    CoreWorker* m_worker = nullptr;

    // WP3.2's Config dialog edits these.
    sf_config m_cfg;
    CoreWorker::Range m_range; // whole chip by default

    QByteArray m_file, m_chip;
    bool m_haveChip = false;
    bool m_blank = false; // chip known blank (last Erase/Blank succeeded, nothing programmed since)
    sf_chip_info m_chipInfo{};
    bool m_busy = false;

    QElapsedTimer m_opTimer, m_stepTimer;
    QTimer m_tick;
    quint64 m_done = 0;

    QAction *m_aDetect, *m_aFile, *m_aSave, *m_aBlank, *m_aErase, *m_aProg, *m_aVerify, *m_aBatch, *m_aRead,
        *m_aConfig, *m_aStatus, *m_aCancel;
    QTabWidget* m_tabs;
    QPlainTextEdit *m_fileView, *m_chipView, *m_log;
    QProgressBar* m_bar;
    QLabel *m_rateLabel, *m_progLabel, *m_chipLabel, *m_fileLabel;
};
