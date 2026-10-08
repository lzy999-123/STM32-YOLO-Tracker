QT       += core gui network concurrent multimedia multimediawidgets widgets

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

SOURCES += \
    main.cpp \
    luckfoxdiscovery.cpp \
    mainwindow.cpp \
    networkcontroller.cpp \
    hostwifimonitor.cpp \
    cameramanager.cpp \
    trackingengine.cpp \
    dnnthread.cpp

HEADERS += \
    mainwindow.h \
    luckfoxdiscovery.h \
    networkcontroller.h \
    hostwifimonitor.h \
    cameramanager.h \
    trackingengine.h \
    dnnthread.h

FORMS += \
    mainwindow.ui

win32:LIBS += -lwlanapi

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# ========== 追加：OpenCV 配置（核心，需修改以下 3 处关键内容）==========
# 1. 编译后的OpenCV安装路径（关键：是build/install，不是原始opencv文件夹）
OPENCV_DIR = $$(OPENCV_DIR)
isEmpty(OPENCV_DIR): OPENCV_DIR = D:/opencv_build/build/install

# 2. 头文件路径：指向包含opencv2文件夹的上级目录（关键修改！）
# 你的正确路径是：install/include/opencv2/opencv.hpp
# 所以INCLUDEPATH要指向 install/include（opencv2的上级目录）
INCLUDEPATH += $$OPENCV_DIR/include

# 3. 链接OpenCV库文件（自动区分 Debug 和 Release）
CONFIG(debug, debug|release) {
    LIBS += -L$$OPENCV_DIR/x64/vc17/lib \
            -lopencv_core4120d \
            -lopencv_highgui4120d \
            -lopencv_imgproc4120d \
            -lopencv_imgcodecs4120d \
            -lopencv_videoio4120d \
            -lopencv_tracking4120d \
            -lopencv_features2d4120d \
            -lopencv_calib3d4120d \
            -lopencv_video4120d \
            -lopencv_dnn4120d
} else {
    LIBS += -L$$OPENCV_DIR/x64/vc17/lib \
            -lopencv_core4120 \
            -lopencv_highgui4120 \
            -lopencv_imgproc4120 \
            -lopencv_imgcodecs4120 \
            -lopencv_videoio4120 \
            -lopencv_tracking4120 \
            -lopencv_features2d4120 \
            -lopencv_calib3d4120 \
            -lopencv_video4120 \
            -lopencv_dnn4120
}


# ========== 追加：ONNX Runtime 配置 ==========
ONNXRUNTIME_DIR = $$(ONNXRUNTIME_DIR)
isEmpty(ONNXRUNTIME_DIR): ONNXRUNTIME_DIR = D:/onnxruntime_gpu/onnxruntime-win-x64-gpu-1.20.1

INCLUDEPATH += $$ONNXRUNTIME_DIR/include
LIBS += -L$$ONNXRUNTIME_DIR/lib -lonnxruntime -lonnxruntime_providers_cuda -lonnxruntime_providers_shared

