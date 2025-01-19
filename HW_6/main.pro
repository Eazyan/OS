QT       += core gui widgets network

CONFIG   += c++11

FORMS += mainwindow.ui

# QWT library settings
QWT_INCLUDE_PATH = /opt/homebrew/Cellar/qwt/6.3.0/include
QWT_LIB_PATH = /opt/homebrew/Cellar/qwt/6.3.0/lib

INCLUDEPATH += $$QWT_INCLUDE_PATH

# Добавляем исходные файлы
SOURCES += main.cpp \
           mainwindow.cpp  # Если у вас есть этот файл

# Добавляем заголовочные файлы
HEADERS += mainwindow.h  # Заголовочный файл для MainWindow

# Если у вас есть UI файл, добавьте его
FORMS += mainwindow.ui

# Указание на путь фреймворка Qwt
FRAMEWORKS += /opt/homebrew/Cellar/qwt/6.3.0/lib/qwt.framework

# Путь для фреймворка Qwt
QMAKE_LFLAGS += -F/opt/homebrew/Cellar/qwt/6.3.0/lib

# Линковка с фреймворками
LIBS += -framework qwt -framework QtWidgets -framework QtGui -framework QtCore

CONFIG += sdk_no_version_check

