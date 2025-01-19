#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    
    MainWindow w;  // Создаём объект MainWindow
    w.show();  // Показываем окно
    
    return a.exec();
}
