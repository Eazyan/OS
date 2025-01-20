#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QPen>
#include <QVector>
#include <QPointF>
#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_point_data.h>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    plot(new QwtPlot(this)),
    curve(new QwtPlotCurve()),
    networkManager(new QNetworkAccessManager(this)),
    plotData(new QVector<QPointF>()) // Инициализация массива данных
{
    ui->setupUi(this);

    // Настройка графика
    setCentralWidget(plot);
    plot->setTitle("Temperature Graph");
    plot->setAxisTitle(QwtPlot::xBottom, "Timestamp");
    plot->setAxisTitle(QwtPlot::yLeft, "Temperature (°C)");

    // Настройка кривой
    curve->setPen(QPen(Qt::blue));  // Синий цвет линии
    curve->attach(plot);  // Привязываем кривую к графику

    // Настройка таймера для запроса данных каждые 1 секунду
    QTimer::singleShot(1000, this, &MainWindow::fetchTemperatureData);

    // Подключение к слоту получения данных
    connect(networkManager, &QNetworkAccessManager::finished, this, &MainWindow::onDataFetched);

    this->setWindowState(Qt::WindowFullScreen);  // Режим полного экрана
    this->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);  // Без рамки

    // Блокируем ввод с клавиатуры и мыши
    installEventFilter(this);
}

MainWindow::~MainWindow()
{
    delete plotData;  // Освобождение памяти
    delete ui;
}

void MainWindow::fetchTemperatureData()
{
    // Адрес API для получения текущей температуры
    QNetworkRequest request(QUrl("http://localhost:8080/temperature/current"));
    networkManager->get(request);
}

void MainWindow::onDataFetched(QNetworkReply* reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "Ошибка при получении данных: " << reply->errorString();
        return;
    }

    // Чтение данных из ответа
    QByteArray response = reply->readAll();
    QJsonDocument jsonDoc = QJsonDocument::fromJson(response);
    QJsonObject dataObject = jsonDoc.object();

    // Получаем температуру и время
    double temperature = dataObject["temperature"].toDouble();
    qint64 timestamp = dataObject["timestamp"].toVariant().toLongLong();

    // Логируем текущую температуру
    qDebug() << "Текущая температура: " << temperature << "°C на " << timestamp;

    // Добавляем данные в массив точек
    plotData->append(QPointF(timestamp, temperature));

    // Ограничиваем количество точек (например, до 100)
    if (plotData->size() > 100) {
        plotData->remove(0);  // Удаляем самую старую точку
    }

    // Обновляем данные графика
    QwtPointSeriesData *data = new QwtPointSeriesData(*plotData);
    curve->setData(data);

    // Настройка осей графика
    plot->setAxisScale(QwtPlot::xBottom, plotData->first().x(), plotData->last().x());  // Ось X по времени
    plot->setAxisScale(QwtPlot::yLeft, -10, 40);  // Ось Y для температуры (примерный диапазон)

    plot->replot();  // Перерисовываем график

    // Повторяем запрос через 1 секунду
    QTimer::singleShot(1000, this, &MainWindow::fetchTemperatureData);
}

// Функция для блокировки всех событий ввода (клавиатуры и мыши)
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::MouseButtonPress) {
        return true;  // Игнорируем все нажатия клавиш и мыши
    }
    return QMainWindow::eventFilter(watched, event);
}
