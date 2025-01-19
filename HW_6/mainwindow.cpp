#include "mainwindow.h"
#include "ui_mainwindow.h"  // Подключаем автоматически сгенерированный файл UI

#include <QtMath>  // Для математических функций (qSin)
#include <QwtSeriesData>  // Для работы с данными для Qwt

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)  // Используем правильный класс Ui::MainWindow
{
    ui->setupUi(this);  // Настроим UI

    // Инициализация графика
    plot = new QwtPlot(this);
    curve = new QwtPlotCurve();
    
    // Создаём данные для графика
    QVector<QPointF> points;
    for (int i = 0; i < 100; ++i)
        points.append(QPointF(i, qSin(i * M_PI / 50.0)));  // Пример графика синуса
    
    // Создание объекта для хранения данных кривой
    QwtPointSeriesData *seriesData = new QwtPointSeriesData(points);
    
    curve->setData(seriesData);  // Установка данных кривой
    curve->attach(plot);  // Прикрепление кривой к графику
    plot->replot();  // Отображаем график
}

MainWindow::~MainWindow()
{
    delete ui;  // Освобождение памяти
}
