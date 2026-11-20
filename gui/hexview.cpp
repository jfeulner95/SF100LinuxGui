#include "hexview.h"

#include <QBrush>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHeaderView>
#include <QPalette>
#include <QRegularExpression>

namespace {
const int BytesPerRow = 16;
const int AsciiCol = BytesPerRow;

// Red blended with the palette's base colour, so it reads on light and dark themes.
QColor diffColor()
{
    const QColor b = QGuiApplication::palette().color(QPalette::Base);
    return QColor((b.red() * 6 + 255 * 4) / 10, b.green() * 6 / 10, b.blue() * 6 / 10);
}
} // namespace

HexModel::HexModel(bool editable, QObject* parent) : QAbstractTableModel(parent), m_editable(editable) {}

void HexModel::setBuffer(const QByteArray& b)
{
    beginResetModel();
    m_buf = b;
    m_hasCmp = false;
    m_cmp.clear();
    endResetModel();
}

void HexModel::setBaseAddress(quint64 a)
{
    m_base = a;
    if (rowCount() > 0)
        emit headerDataChanged(Qt::Vertical, 0, rowCount() - 1);
}

void HexModel::changedAll()
{
    if (rowCount() > 0)
        emit dataChanged(index(0, 0), index(rowCount() - 1, AsciiCol)); // only visible cells get repainted
}

void HexModel::setCompare(const QByteArray& other, qint64 otherOffset)
{
    m_cmp = other;
    m_cmpOff = otherOffset;
    m_hasCmp = true;
    changedAll();
}

void HexModel::clearCompare()
{
    if (!m_hasCmp)
        return;
    m_hasCmp = false;
    m_cmp.clear();
    changedAll();
}

bool HexModel::differs(qint64 i) const
{
    if (!m_hasCmp)
        return false;
    const qint64 j = i - m_cmpOff;
    return j >= 0 && j < m_cmp.size() && m_buf.at(i) != m_cmp.at(j);
}

qint64 HexModel::nextDiff(qint64 from) const
{
    if (!m_hasCmp)
        return -1;
    const qint64 n = m_buf.size();
    for (qint64 i = qMax<qint64>(from, 0); i < n; ++i)
        if (differs(i))
            return i;
    for (qint64 i = 0; i < qMin(from, n); ++i)
        if (differs(i))
            return i;
    return -1;
}

int HexModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int((m_buf.size() + BytesPerRow - 1) / BytesPerRow);
}

int HexModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : BytesPerRow + 1; }

QVariant HexModel::data(const QModelIndex& ix, int role) const
{
    if (!ix.isValid())
        return QVariant();
    const qint64 rowStart = qint64(ix.row()) * BytesPerRow;
    const qint64 size = m_buf.size();
    if (rowStart >= size)
        return QVariant();

    if (ix.column() == AsciiCol) {
        if (role == Qt::DisplayRole) {
            QString s;
            for (qint64 i = rowStart; i < qMin(rowStart + BytesPerRow, size); ++i) {
                const uchar c = uchar(m_buf.at(i));
                s += (c >= 32 && c < 127) ? QChar(c) : QChar::fromLatin1('.');
            }
            return s;
        }
        if (role == Qt::BackgroundRole && m_hasCmp)
            for (qint64 i = rowStart; i < qMin(rowStart + BytesPerRow, size); ++i)
                if (differs(i))
                    return QBrush(diffColor());
        return QVariant();
    }

    const qint64 i = rowStart + ix.column();
    if (i >= size)
        return QVariant();
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole: return QString::asprintf("%02X", unsigned(uchar(m_buf.at(i))));
    case Qt::TextAlignmentRole: return int(Qt::AlignCenter);
    case Qt::BackgroundRole:
        if (differs(i))
            return QBrush(diffColor());
        return QVariant();
    default: return QVariant();
    }
}

bool HexModel::setData(const QModelIndex& ix, const QVariant& value, int role)
{
    static const QRegularExpression hex2(QStringLiteral("^[0-9A-Fa-f]{1,2}$"));
    if (role != Qt::EditRole || !m_editable || !ix.isValid() || ix.column() >= AsciiCol)
        return false;
    const qint64 i = qint64(ix.row()) * BytesPerRow + ix.column();
    const QString t = value.toString().trimmed();
    if (i >= m_buf.size() || !hex2.match(t).hasMatch())
        return false;
    m_buf[i] = char(t.toUInt(nullptr, 16));
    emit dataChanged(ix, ix);
    emit dataChanged(index(ix.row(), AsciiCol), index(ix.row(), AsciiCol));
    emit bufferEdited();
    return true;
}

Qt::ItemFlags HexModel::flags(const QModelIndex& ix) const
{
    if (!ix.isValid())
        return Qt::NoItemFlags;
    if (ix.column() == AsciiCol)
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (qint64(ix.row()) * BytesPerRow + ix.column() >= m_buf.size()) // padding cells of the last row
        return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (m_editable)
        f |= Qt::ItemIsEditable;
    return f;
}

QVariant HexModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant();
    if (o == Qt::Horizontal)
        return section < AsciiCol ? QString::asprintf("%02X", section) : QStringLiteral("ASCII");
    return QString::asprintf("%08llX", static_cast<unsigned long long>(m_base + quint64(section) * BytesPerRow));
}

// ---- HexView ----

HexView::HexView(bool editable, QWidget* parent) : QTableView(parent), m_model(new HexModel(editable, this))
{
    setModel(m_model);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    setFont(mono);
    const QFontMetrics fm(mono);
    setSelectionMode(QAbstractItemView::SingleSelection);
    // Fixed section sizes: the headers never measure the 16M rows.
    horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    horizontalHeader()->setDefaultSectionSize(fm.horizontalAdvance(QStringLiteral("FF")) + 16);
    horizontalHeader()->resizeSection(AsciiCol, fm.horizontalAdvance(QStringLiteral("M")) * (BytesPerRow + 1));
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    verticalHeader()->setDefaultSectionSize(fm.height() + 4);
    connect(m_model, &HexModel::bufferEdited, this, &HexView::bufferEdited);
}

void HexView::select(qint64 i)
{
    const QModelIndex ix = m_model->index(int(i / BytesPerRow), int(i % BytesPerRow));
    setCurrentIndex(ix);
    scrollTo(ix, QAbstractItemView::PositionAtCenter);
}

bool HexView::gotoAddress(quint64 addr)
{
    if (addr < m_model->baseAddress() || addr - m_model->baseAddress() >= quint64(buffer().size()))
        return false;
    select(qint64(addr - m_model->baseAddress()));
    return true;
}

bool HexView::nextDiff()
{
    const QModelIndex c = currentIndex();
    const qint64 i = m_model->nextDiff(c.isValid() ? qint64(c.row() + 1) * BytesPerRow : 0);
    if (i < 0)
        return false;
    select(i);
    return true;
}
