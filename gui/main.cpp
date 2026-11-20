#include <QApplication>

#include "mainwindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("SF100 GUI"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    MainWindow w;
    if (app.arguments().contains(QStringLiteral("--smoke"))) { // CI: build the UI, never touch USB
        QCoreApplication::processEvents();
        return 0;
    }
    w.show();
    return app.exec();
}
