// HexModel/HexView: virtualised hex dump (16 bytes per row + ASCII) that stays responsive for 256 MiB.
#pragma once

#include <QAbstractTableModel>
#include <QByteArray>
#include <QTableView>

class HexModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit HexModel(bool editable, QObject* parent = nullptr);

    const QByteArray& buffer() const { return m_buf; }
    void setBuffer(const QByteArray& b); // also drops the compare data (it would be stale)
    void setBaseAddress(quint64 a);
    quint64 baseAddress() const { return m_base; }
    // Byte i differs if other[i - otherOffset] exists and != this[i]. Bytes outside `other` never differ.
    void setCompare(const QByteArray& other, qint64 otherOffset);
    void clearCompare();
    bool hasCompare() const { return m_hasCmp; }
    // First differing index >= from, wrapping once; -1 if none. O(n).
    qint64 nextDiff(qint64 from) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QVariant headerData(int section, Qt::Orientation o, int role = Qt::DisplayRole) const override;

signals:
    void bufferEdited();

private:
    bool differs(qint64 i) const;
    void changedAll();

    bool m_editable;
    QByteArray m_buf;
    quint64 m_base = 0;
    QByteArray m_cmp;
    qint64 m_cmpOff = 0;
    bool m_hasCmp = false;
};

class HexView : public QTableView {
    Q_OBJECT
public:
    explicit HexView(bool editable, QWidget* parent = nullptr);

    const QByteArray& buffer() const { return m_model->buffer(); }
    void setBuffer(const QByteArray& b) { m_model->setBuffer(b); }
    void setBaseAddress(quint64 a) { m_model->setBaseAddress(a); }
    void setCompare(const QByteArray& other, qint64 otherOffset) { m_model->setCompare(other, otherOffset); }
    void clearCompare() { m_model->clearCompare(); }
    bool hasCompare() const { return m_model->hasCompare(); }

    bool gotoAddress(quint64 addr); // false if outside the buffer
    bool nextDiff();                // from the row after the current one, wraps once; false if no difference

signals:
    void bufferEdited();

private:
    void select(qint64 index);

    HexModel* m_model;
};
