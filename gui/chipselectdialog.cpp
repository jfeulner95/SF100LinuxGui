#include "chipselectdialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QLocale>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>

ChipSelectDialog::ChipSelectDialog(const QVector<sf_chip_info>& chips, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Select chip"));
    resize(760, 520);

    auto* model = new QStandardItemModel(0, 5, this);
    model->setHorizontalHeaderLabels({tr("Type"), tr("Manufacturer"), tr("Size"), tr("Vcc"), tr("IC type")});
    // UserRole carries the sort key (proxy sortRole): the size column must sort numerically.
    auto item = [](const QString& text, const QVariant& key) {
        auto* it = new QStandardItem(text);
        it->setEditable(false);
        it->setData(key, Qt::UserRole);
        return it;
    };
    for (const sf_chip_info& c : chips) {
        const QString type = QString::fromUtf8(c.type_name), man = QString::fromUtf8(c.manufacturer),
                      ic = QString::fromUtf8(c.ic_type);
        model->appendRow({item(type, type), item(man, man),
                          item(QLocale().formattedDataSize(qint64(c.size_bytes), 1, QLocale::DataSizeIecFormat),
                               qulonglong(c.size_bytes)),
                          item(c.vcc_mv ? tr("%1 V").arg(c.vcc_mv / 1000.0, 0, 'f', 1) : QStringLiteral("-"),
                               c.vcc_mv),
                          item(ic, ic)});
    }

    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(model);
    m_proxy->setSortRole(Qt::UserRole);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(-1); // all columns

    m_view = new QTableView;
    m_view->setModel(m_proxy);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSortingEnabled(true);
    m_view->verticalHeader()->hide();
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->setColumnWidth(0, 220);
    m_view->setColumnWidth(1, 180);
    connect(m_view, &QTableView::doubleClicked, this, &ChipSelectDialog::accept);

    auto* filter = new QLineEdit;
    filter->setPlaceholderText(tr("Filter (type, manufacturer, size…)"));
    filter->setClearButtonEnabled(true);
    connect(filter, &QLineEdit::textChanged, m_proxy, &QSortFilterProxyModel::setFilterFixedString);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, this, &ChipSelectDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* top = new QVBoxLayout(this);
    top->addWidget(filter);
    top->addWidget(m_view, 1);
    top->addWidget(box);
    filter->setFocus();
}

QString ChipSelectDialog::selectedType() const
{
    const QModelIndex ix = m_view->currentIndex();
    return ix.isValid() ? ix.siblingAtColumn(0).data().toString() : QString();
}

void ChipSelectDialog::accept()
{
    if (!selectedType().isEmpty())
        QDialog::accept();
}
