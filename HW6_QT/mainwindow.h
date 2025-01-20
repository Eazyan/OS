#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <QNetworkAccessManager>
#include <QVector>
#include <QPointF>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Ui::MainWindow *ui;
    QwtPlot *plot;                 // График
    QwtPlotCurve *curve;           // Кривая на графике
    QVector<QPointF> *plotData;    // Данные для графика
    QNetworkAccessManager *networkManager; // Для запросов API

    void fetchTemperatureData();   // Запрос данных о температуре

private slots:
    void onDataFetched(QNetworkReply *reply); // Обработка полученных данных
};

#endif // MAINWINDOW_H
