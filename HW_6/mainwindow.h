#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <Qwt_Plot.h>  // Для QwtPlot
#include <Qwt_Plot_Curve.h>  // Для рисования кривой

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    virtual ~MainWindow();  // Виртуальный деструктор

private:
    QwtPlot *plot;  // Виджет для графика
    QwtPlotCurve *curve;  // Кривая для графика
};

#endif // MAINWINDOW_H
