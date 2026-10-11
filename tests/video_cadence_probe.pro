include(tracking_concurrency_test.pro)
CONFIG -= testcase
TARGET = video_cadence_probe
SOURCES -= $$PWD/tracking_concurrency_test.cpp
SOURCES += $$PWD/video_cadence_probe.cpp
