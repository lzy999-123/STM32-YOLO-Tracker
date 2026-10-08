QT += core network testlib
QT -= gui
CONFIG += console testcase c++17
TARGET = networkcontroller_test
INCLUDEPATH += ..
SOURCES += networkcontroller_test.cpp ../networkcontroller.cpp
HEADERS += ../networkcontroller.h
