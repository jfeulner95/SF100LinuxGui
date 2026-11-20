// ChipSelectDialog: pick a chip from the (already loaded) chip database. Pure UI: never calls sf_*.
#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

#include "sf100core.h"

class QSortFilterProxyModel;
class QTableView;

class ChipSelectDialog : public QDialog {
    Q_OBJECT
public:
    explicit ChipSelectDialog(const QVector<sf_chip_info>& chips, QWidget* parent = nullptr);

    QString selectedType() const; // type_name of the chosen row, empty if none

    void accept() override; // ignored while no row is selected

private:
    QSortFilterProxyModel* m_proxy;
    QTableView* m_view;
};
