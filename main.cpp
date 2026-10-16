#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    if (ResearchTrace::enabled()) ResearchTrace::instance();
    MainWindow w;
    w.show();
    return a.exec();
}
