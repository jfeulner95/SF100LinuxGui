// ConfigDialog: programmer settings, address range and batch sequence. Pure UI: never calls sf_*.
#pragma once

#include <QDialog>
#include <QList>

#include "coreworker.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

class ConfigDialog : public QDialog {
    Q_OBJECT
public:
    // chip may be null (no chip detected yet): then the range is not validated against a chip size.
    ConfigDialog(const sf_config& cfg, const CoreWorker::Range& range, const QList<CoreWorker::Op>& steps,
                 const sf_chip_info* chip, QWidget* parent = nullptr);

    sf_config config() const;
    CoreWorker::Range range() const; // {0,0} unless "Partial range" is checked
    QList<CoreWorker::Op> steps() const;

    void accept() override; // validates the range first

private:
    bool parseHex(const QLineEdit* e, quint64& v) const;

    sf_chip_info m_chip{};
    bool m_haveChip;
    QComboBox *m_vcc, *m_clock, *m_target;
    QCheckBox *m_partial, *m_erase, *m_blank, *m_prog, *m_verify;
    QLineEdit *m_start, *m_len;
};
