include(../untitled7.pro)
QT += testlib
CONFIG += console testcase
CONFIG -= debug debug_and_release
DEFINES += TRACKING_CONCURRENCY_TEST
TARGET = tracking_concurrency_test
SOURCES = $$PWD/tracking_concurrency_test.cpp \
    $$PWD/../mainwindow.cpp $$PWD/../cameramanager.cpp \
    $$PWD/../luckfoxdiscovery.cpp $$PWD/../networkcontroller.cpp \
    $$PWD/../hostwifimonitor.cpp $$PWD/../trackingengine.cpp $$PWD/../dnnthread.cpp
HEADERS = $$PWD/../mainwindow.h $$PWD/../cameramanager.h \
    $$PWD/../luckfoxdiscovery.h $$PWD/../networkcontroller.h \
    $$PWD/../hostwifimonitor.h $$PWD/../trackingengine.h $$PWD/../dnnthread.h
FORMS = $$PWD/../mainwindow.ui
INCLUDEPATH += $$PWD/..
