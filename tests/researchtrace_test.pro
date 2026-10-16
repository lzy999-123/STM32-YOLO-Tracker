QT += core testlib
QT -= gui
CONFIG += console testcase c++17
CONFIG -= debug debug_and_release
TARGET = researchtrace_test
INCLUDEPATH += ..
SOURCES += researchtrace_test.cpp
HEADERS += ../researchtrace.h ../researchframe.h
