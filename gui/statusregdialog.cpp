#include "statusregdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {
// Generic Winbond/JEDEC names, listed from bit 7 down to bit 0.
const char* const BitNames[3][8] = {
    {"SRP0", "SEC/BP4", "TB/BP3", "BP2", "BP1", "BP0", "WEL", "WIP"},
    {"SUS", "CMP", "LB3", "LB2", "LB1", "(R)", "QE", "SRP1"},
    {"HOLD/RST", "DRV1", "DRV0", "-", "-", "WPS", "-", "-"},
};
} // namespace

StatusRegDialog::StatusRegDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Status registers"));
    auto* top = new QVBoxLayout(this);
    auto* hint = new QLabel(tr("Bit names are generic (Winbond-style). Check your chip's datasheet before writing."));
    hint->setWordWrap(true);
    top->addWidget(hint);

    for (int r = 0; r < 3; ++r) {
        auto* group = new QGroupBox(tr("SR%1").arg(r + 1));
        auto* grid = new QGridLayout(group);
        for (int col = 0; col < 8; ++col) {
            const int bit = 7 - col;
            const QString name = QString::fromLatin1(BitNames[r][col]);
            auto* cb = new QCheckBox(QStringLiteral("%1 %2").arg(bit).arg(name));
            // WIP/WEL/SUS are status flags set by the chip; "-"/(R) are unused; SR3 is read-only in this version.
            if (r == 2 || name == QLatin1String("-") || name == QLatin1String("(R)") || name == QLatin1String("WIP") ||
                name == QLatin1String("WEL") || name == QLatin1String("SUS"))
                cb->setEnabled(false);
            m_bit[r][bit] = cb;
            grid->addWidget(cb, 0, col);
            connect(cb, &QCheckBox::toggled, this, [this, r] { m_hex[r]->setText(QString::asprintf("%02X", value(r))); });
        }
        m_hex[r] = new QLineEdit(QStringLiteral("00"));
        m_hex[r]->setMaximumWidth(60);
        m_hex[r]->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9A-Fa-f]{1,2}")), m_hex[r]));
        grid->addWidget(new QLabel(tr("Hex:")), 0, 8);
        grid->addWidget(m_hex[r], 0, 9);
        connect(m_hex[r], &QLineEdit::textEdited, this, [this, r](const QString& t) {
            const quint8 v = quint8(t.toUInt(nullptr, 16));
            for (int b = 0; b < 8; ++b) {
                const QSignalBlocker block(m_bit[r][b]); // don't rewrite the text being typed
                m_bit[r][b]->setChecked(v & (1 << b));
            }
        });
        if (r == 2) {
            m_hex[r]->setReadOnly(true);
            group->setToolTip(tr("SR3 is read-only in this version."));
        }
        top->addWidget(group);
    }

    m_read = new QPushButton(tr("Read"));
    m_write1 = new QPushButton(tr("Write SR1"));
    m_write2 = new QPushButton(tr("Write SR1+SR2"));
    m_unprot = new QPushButton(tr("Unprotect"));
    m_unprot->setToolTip(tr("Clear SRP0, SEC, TB, BP2..BP0 in SR1 and CMP in SR2 (QE and the rest unchanged), then write SR1+SR2"));
    auto* close = new QPushButton(tr("Close"));
    auto* row = new QHBoxLayout;
    for (QPushButton* b : {m_read, m_write1, m_write2, m_unprot})
        row->addWidget(b);
    row->addStretch();
    row->addWidget(close);
    top->addLayout(row);

    connect(m_read, &QPushButton::clicked, this, &StatusRegDialog::readRequested);
    connect(m_write1, &QPushButton::clicked, this, [this] { emit writeRequested(QByteArray(1, char(value(0)))); });
    connect(m_write2, &QPushButton::clicked, this, [this] { emit writeRequested(sr12()); });
    connect(m_unprot, &QPushButton::clicked, this, &StatusRegDialog::unprotect);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    updateButtons();
}

quint8 StatusRegDialog::value(int reg) const
{
    int v = 0;
    for (int b = 0; b < 8; ++b)
        if (m_bit[reg][b]->isChecked())
            v |= 1 << b;
    return quint8(v);
}

QByteArray StatusRegDialog::sr12() const
{
    QByteArray b(1, char(value(0)));
    b.append(char(value(1)));
    return b;
}

void StatusRegDialog::setValue(int reg, quint8 v)
{
    for (int b = 0; b < 8; ++b)
        m_bit[reg][b]->setChecked(v & (1 << b)); // toggled() keeps the hex edit in sync
    m_hex[reg]->setText(QString::asprintf("%02X", v)); // also covers the no-change case
}

void StatusRegDialog::setValues(const quint8 sr[3])
{
    for (int r = 0; r < 3; ++r)
        setValue(r, sr[r]);
    m_valid = true;
    updateButtons();
}

void StatusRegDialog::setBusy(bool busy)
{
    m_busy = busy;
    updateButtons();
}

void StatusRegDialog::updateButtons()
{
    m_read->setEnabled(!m_busy);
    for (QPushButton* b : {m_write1, m_write2, m_unprot})
        b->setEnabled(!m_busy && m_valid);
}

void StatusRegDialog::unprotect()
{
    if (QMessageBox::question(this, tr("Unprotect"),
                              tr("Clear the protection bits (SRP0, SEC, TB, BP2..BP0 in SR1, CMP in SR2) and write SR1+SR2?\n"
                                 "QE and the other SR2 bits are written back as shown.")) != QMessageBox::Yes)
        return;
    setValue(0, quint8(value(0) & 0x03)); // keep WEL/WIP only; bits 7..2 = SRP0, SEC, TB, BP2, BP1, BP0
    setValue(1, quint8(value(1) & ~0x40)); // CMP=1 with BP=0 protects the WHOLE array (Winbond), so clear it too
    emit writeRequested(sr12());
}
