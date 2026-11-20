// StatusRegDialog: view/edit SR1..SR3 bits. Pure UI: it only emits requests; MainWindow runs them on the
// CoreWorker and feeds results back through setValues()/setBusy(). Never calls sf_*.
#pragma once

#include <QByteArray>
#include <QDialog>

class QCheckBox;
class QLineEdit;
class QPushButton;

class StatusRegDialog : public QDialog {
    Q_OBJECT
public:
    explicit StatusRegDialog(QWidget* parent = nullptr);

    void setValues(const quint8 sr[3]); // result of a read
    void setBusy(bool busy);            // disables the buttons while an operation runs

signals:
    void readRequested();
    void writeRequested(const QByteArray& values); // 1 byte (SR1) or 2 bytes (SR1, SR2)

private:
    quint8 value(int reg) const;
    QByteArray sr12() const;
    void setValue(int reg, quint8 v);
    void updateButtons();
    void unprotect();

    QCheckBox* m_bit[3][8]; // [register][bit number]
    QLineEdit* m_hex[3];
    QPushButton *m_read, *m_write1, *m_write2, *m_unprot;
    bool m_busy = false;
    bool m_valid = false; // a read succeeded: writes are only allowed from known values
};
