QT       += core gui serialport  multimedia multimediawidgets widgets

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    mainwindow.h

FORMS += \
    mainwindow.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# ========== 追加：OpenCV 配置（核心，需修改以下 3 处关键内容）==========
# 1. 编译后的OpenCV安装路径（关键：是build/install，不是原始opencv文件夹）
OPENCV_DIR = D:/opencv_build/build/install

# 2. 头文件路径：指向包含opencv2文件夹的上级目录（关键修改！）
# 你的正确路径是：install/include/opencv2/opencv.hpp
# 所以INCLUDEPATH要指向 install/include（opencv2的上级目录）
INCLUDEPATH += $$OPENCV_DIR/include

# 3. 链接OpenCV库文件（Debug版本，64位）
LIBS += -L$$OPENCV_DIR/x64/vc17/lib \
        -lopencv_core4120d \
        -lopencv_highgui4120d \
        -lopencv_imgproc4120d \
        -lopencv_imgcodecs4120d \
        -lopencv_tracking4120d \ # CSRT追踪核心模块，必须加
        -lopencv_features2d4120d\
        -lopencv_calib3d4120d\
        -lopencv_video4120d\
        -lopencv_dnn4120d      # 接入DNN






