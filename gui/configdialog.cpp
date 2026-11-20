#include "configdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

using Op = CoreWorker::Op;

namespace {
void addItem(QComboBox* c, const QString& text, int value, int current)
{
    c->addItem(text, value);
    if (value == current)
        c->setCurrentIndex(c->count() - 1);
}
} // namespace

ConfigDialog::ConfigDialog(const sf_config& cfg, const CoreWorker::Range& range, const QList<Op>& steps,
                           const sf_chip_info* chip, QWidget* parent)
    : QDialog(parent), m_haveChip(chip != nullptr)
{
    setWindowTitle(tr("Configuration"));
    if (chip)
        m_chip = *chip;

    m_vcc = new QComboBox;
    addItem(m_vcc, tr("Auto"), SF_VCC_AUTO, cfg.vcc);
    addItem(m_vcc, tr("3.3 V"), SF_VCC_3V3, cfg.vcc);
    addItem(m_vcc, tr("2.5 V"), SF_VCC_2V5, cfg.vcc);
    addItem(m_vcc, tr("1.8 V"), SF_VCC_1V8, cfg.vcc);

    m_clock = new QComboBox;
    addItem(m_clock, tr("24 MHz"), SF_CLK_24M, cfg.clock);
    addItem(m_clock, tr("12 MHz"), SF_CLK_12M, cfg.clock);
    addItem(m_clock, tr("8 MHz"), SF_CLK_8M, cfg.clock);
    addItem(m_clock, tr("3 MHz"), SF_CLK_3M, cfg.clock);
    addItem(m_clock, tr("2.18 MHz"), SF_CLK_2180K, cfg.clock);
    addItem(m_clock, tr("1.5 MHz"), SF_CLK_1500K, cfg.clock);
    addItem(m_clock, tr("750 kHz"), SF_CLK_750K, cfg.clock);
    addItem(m_clock, tr("375 kHz"), SF_CLK_375K, cfg.clock);

    m_target = new QComboBox;
    addItem(m_target, tr("Socket / ISP 1"), SF_TARGET_SOCKET1, cfg.target);
    addItem(m_target, tr("Socket / ISP 2"), SF_TARGET_SOCKET2, cfg.target);
    addItem(m_target, tr("Card"), SF_TARGET_CARD, cfg.target);
    addItem(m_target, tr("SKT"), SF_TARGET_SKT, cfg.target);

    m_partial = new QCheckBox(tr("Partial range"));
    m_partial->setChecked(range.addr != 0 || range.len != 0);
    m_start = new QLineEdit(QString::number(range.addr, 16).toUpper());
    m_len = new QLineEdit(QString::number(range.len, 16).toUpper());
    for (QLineEdit* e : {m_start, m_len}) {
        e->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9A-Fa-f]{1,16}")), e));
        e->setEnabled(m_partial->isChecked());
    }
    m_start->setPlaceholderText(tr("hex"));
    m_len->setPlaceholderText(tr("hex, 0 = to end"));
    connect(m_partial, &QCheckBox::toggled, m_start, &QWidget::setEnabled);
    connect(m_partial, &QCheckBox::toggled, m_len, &QWidget::setEnabled);

    auto* note = new QLabel;
    note->setWordWrap(true);
    if (m_haveChip && m_chip.sector_size)
        note->setText(tr("Partial erase requires start and length to be multiples of 0x%1 (the erase unit of %2); "
                         "the length may run to the chip end.")
                          .arg(m_chip.sector_size, 0, 16)
                          .arg(QString::fromUtf8(m_chip.type_name)));

    m_erase = new QCheckBox(tr("Erase"));
    m_blank = new QCheckBox(tr("Blank check"));
    m_prog = new QCheckBox(tr("Program"));
    m_verify = new QCheckBox(tr("Verify"));
    m_erase->setChecked(steps.contains(Op::Erase));
    m_blank->setChecked(steps.contains(Op::Blank));
    m_prog->setChecked(steps.contains(Op::Program));
    m_verify->setChecked(steps.contains(Op::Verify));

    auto* form = new QFormLayout;
    form->addRow(tr("Vcc"), m_vcc);
    form->addRow(tr("SPI clock"), m_clock);
    form->addRow(tr("Target"), m_target);
    form->addRow(m_partial);
    form->addRow(tr("Start address (hex)"), m_start);
    form->addRow(tr("Length (hex)"), m_len);
    form->addRow(note);
    form->addRow(tr("Batch sequence (fixed order)"), m_erase);
    form->addRow(QString(), m_blank);
    form->addRow(QString(), m_prog);
    form->addRow(QString(), m_verify);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, this, &ConfigDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* top = new QVBoxLayout(this);
    top->addLayout(form);
    top->addWidget(new QLabel(tr("Changes apply to the next operation. Re-run Detect after changing Vcc or target.")));
    top->addWidget(box);
}

sf_config ConfigDialog::config() const
{
    sf_config c;
    sf_config_default(&c); // pure helper
    c.vcc = sf_vcc(m_vcc->currentData().toInt());
    c.clock = sf_spi_clock(m_clock->currentData().toInt());
    c.target = sf_target(m_target->currentData().toInt());
    return c;
}

bool ConfigDialog::parseHex(const QLineEdit* e, quint64& v) const
{
    if (e->text().isEmpty()) {
        v = 0;
        return true;
    }
    bool ok = false;
    v = e->text().toULongLong(&ok, 16);
    return ok;
}

CoreWorker::Range ConfigDialog::range() const
{
    CoreWorker::Range r;
    if (m_partial->isChecked()) {
        parseHex(m_start, r.addr);
        parseHex(m_len, r.len);
    }
    return r;
}

QList<Op> ConfigDialog::steps() const
{
    QList<Op> s;
    if (m_erase->isChecked()) s << Op::Erase;
    if (m_blank->isChecked()) s << Op::Blank;
    if (m_prog->isChecked()) s << Op::Program;
    if (m_verify->isChecked()) s << Op::Verify;
    return s;
}

void ConfigDialog::accept()
{
    if (m_partial->isChecked()) {
        quint64 a = 0, l = 0;
        if (!parseHex(m_start, a) || !parseHex(m_len, l)) {
            QMessageBox::warning(this, windowTitle(), tr("Invalid hex number."));
            return;
        }
        const quint64 size = m_haveChip ? m_chip.size_bytes : 0;
        if (size && (a >= size || l > size - a)) { // written to avoid overflow of a + l
            QMessageBox::warning(this, windowTitle(),
                                 tr("Range outside the chip: start must be below 0x%1 and start + length at most 0x%1.")
                                     .arg(size, 0, 16));
            return;
        }
    }
    QDialog::accept();
}
