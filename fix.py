import sys

with open('mainwindow.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

idx = content.find('    m_needInit = true;')

prefix = '''#include mainwindow.h
#include ui_mainwindow.h

#include <QSerialPortInfo>
#include <QMessageBox>
#include <QPainter>
#include <QVBoxLayout>
#include <QElapsedTimer>
#include <utility>
#include <QTimer>
#include <QDateTime>
#include <QPen>
#include <QRect>
#include <algorithm>
#include <cmath>

using namespace cv;
using namespace cv::dnn;

// ==========================================
// 大脑线程实现 (YOLOv8)
// ==========================================
DnnThread::DnnThread(QObject *parent) : QThread(parent) {
    m_isTracking = false;
    m_needInit = false;
    m_hasNewFrame = false;
    m_isComputing = false;
    m_useYolo = false;
    m_lockedClassId = -1;

    m_classNames = {
        person, bicycle, car, motorcycle, airplane, bus, train, truck, boat, traffic light,
        fire hydrant, stop sign, parking meter, bench, bird, cat, dog, horse, sheep, cow,
        elephant, bear, zebra, giraffe, backpack, umbrella, handbag, tie, suitcase, frisbee,
        skis, snowboard, sports ball, kite, baseball bat, baseball glove, skateboard, surfboard, tennis racket, bottle,
        wine glass, cup, fork, knife, spoon, bowl, banana, apple, sandwich, orange,
        broccoli, carrot, hot dog, pizza, donut, cake, chair, couch, potted plant, bed,
        dining table, toilet, tv, laptop, mouse, remote, keyboard, cell phone, microwave, oven,
        toaster, sink, refrigerator, book, clock, vase, scissors, teddy bear, hair drier, toothbrush
    };

    try {
        m_yoloNet = cv::dnn::readNetFromONNX(yolov8n.onnx);
        if (m_yoloNet.empty()) {
            qDebug() << YOLO 模型加载失败: yolov8n.onnx 为空;
        } else {
            m_yoloNet.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            m_yoloNet.setPreferableTarget(cv::dnn::DNN_TARGET_OPENCL); // 尝试使用集成显卡(OpenCL)加速
        }
    } catch (const std::exception& e) {
        qDebug() << YOLO 加载异常:  << e.what();
    }
}

DnnThread::~DnnThread() {
    requestInterruption();
    wait();
}

bool DnnThread::isBusy() {
    QMutexLocker locker(&m_mutex);
    return m_isComputing || m_hasNewFrame || m_needInit;
}

void DnnThread::initDnn(const cv::Mat &frame, const cv::Rect2d &target) {
    QMutexLocker locker(&m_mutex);
    if(frame.empty()) return;
    m_frame = frame;
    m_initRect = target;
'''

with open('mainwindow.cpp', 'w', encoding='utf-8') as f:
    f.write(prefix + content[idx:])
