#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QCameraDevice>
#include <QDateTime>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QScopeGuard>
#include <QSerialPortInfo>
#include <QTimer>

#include <algorithm>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_isCapturing(false)
    , m_isSelecting(false)
    , m_hasSelectedTarget(false)
    , m_currentMode(0)
    , m_isTargetTracked(false)
    , m_offsetX(0)
    , m_offsetY(0)
    , m_lastSelectedRect(cv::Rect2d())
    , m_wasTrackingBeforeDisconn(false)
    , m_forceResetTracking(false)
    , m_waitingForRecover(false)
    , m_lastSerialSendTime(0)
    , m_dnnFrameSkipCounter(0)
    , m_lostFrameCount(0)
{
    ui->setupUi(this);
    cv::setNumThreads(4);
    setWindowTitle(QStringLiteral("STM32云台双核自动跟踪系统"));

    ui->imageLabel->setAlignment(Qt::AlignCenter);
    QPixmap initPlaceholder(640, 480);
    initPlaceholder.fill(QColor(50, 50, 50));
    QPainter initPainter(&initPlaceholder);
    initPainter.setPen(Qt::white);
    QFont initFont = initPainter.font();
    initFont.setPointSize(12);
    initPainter.setFont(initFont);
    initPainter.drawText(initPlaceholder.rect(), Qt::AlignCenter, QStringLiteral("请点击“打开摄像头”"));
    initPainter.end();
    ui->imageLabel->setPixmap(initPlaceholder);

    ui->LED1->setStyleSheet(QStringLiteral("background-color:red"));
    ui->pushButton_2->setEnabled(false);
    ui->pushButton_3->setEnabled(false);
    ui->label_11->setText(QStringLiteral("手动"));
    ui->label_11->setStyleSheet(QStringLiteral("color: black; font-size: 14px; font-weight: bold;"));

    on_pushButton_8_clicked();
    testDNN();

    connect(&m_trackingEngine, &TrackingEngine::targetTracked, this,
            [this](const cv::Rect2d &rect, int offsetX, int offsetY) {
        m_trackedRect = rect;
        m_isTargetTracked = true;
        m_offsetX = static_cast<int16_t>(offsetX);
        m_offsetY = static_cast<int16_t>(offsetY);
    });
    connect(&m_trackingEngine, &TrackingEngine::targetLost, this, [this]() {
        m_isTargetTracked = false;
        m_trackedRect = cv::Rect2d();
        m_offsetX = 0;
        m_offsetY = 0;
    });
    connect(&m_trackingEngine, &TrackingEngine::logMessage, this, [this](const QString &msg) {
        if (ui->plainTextEdit_2) {
            ui->plainTextEdit_2->appendPlainText(msg);
        }
    });

    ui->comboBox_4->clear();
    ui->comboBox_4->addItem(QStringLiteral("YOLOv8s（精度优先）"), QStringLiteral("yolov8s.onnx"));
    ui->comboBox_4->addItem(QStringLiteral("YOLOv8n（速度优先）"), QStringLiteral("yolov8n.onnx"));
    ui->comboBox_4->addItem(QStringLiteral("特征跟踪 CSRT+ORB（任意物体）"), QStringLiteral("feature"));
    connect(ui->comboBox_4, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTrackingModelChanged);
    m_trackingEngine.preloadYoloModels();
    m_trackingEngine.setCurrentModel(currentTrackingModelFileName());

    ui->comboBox_3->clear();
    QList<QCameraDevice> cameraDevices;
    try {
        cameraDevices = QMediaDevices::videoInputs();
    } catch (...) {
    }

    if (cameraDevices.isEmpty()) {
        ui->comboBox_3->addItem(QStringLiteral("未检测到摄像头"));
        ui->comboBox_3->setEnabled(false);
        ui->pushButton_9->setEnabled(false);
    } else {
        for (const QCameraDevice &device : cameraDevices) {
            ui->comboBox_3->addItem(device.description(), device.id());
        }
        ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
    }

    connect(ui->comboBox_3, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onCameraChanged);
    connect(&m_cameraManager, &CameraManager::frameReady,
            this, &MainWindow::handleNewVideoFrame);
    connect(&m_cameraManager, &CameraManager::stateChanged, this,
            [this](CameraManager::CameraState state) {
        if (state == CameraManager::CameraState::Idle) {
            ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
            ui->pushButton_9->setEnabled(true);
        } else if (state == CameraManager::CameraState::Open) {
            ui->pushButton_9->setText(QStringLiteral("关闭摄像头"));
            ui->pushButton_9->setEnabled(true);
        } else if (state == CameraManager::CameraState::Opening) {
            ui->pushButton_9->setText(QStringLiteral("正在打开..."));
            ui->pushButton_9->setEnabled(false);
        } else if (state == CameraManager::CameraState::Closing) {
            ui->pushButton_9->setText(QStringLiteral("正在关闭..."));
            ui->pushButton_9->setEnabled(false);
        } else {
            ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
            ui->pushButton_9->setEnabled(true);
        }
    });
    connect(&m_cameraManager, &CameraManager::cameraError, this, [this](const QString &errorMsg) {
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】%1").arg(errorMsg));
        ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
        ui->pushButton_9->setEnabled(true);
    });
    m_cameraManager.startChecking();
}

MainWindow::~MainWindow()
{
    m_trackingEngine.stopTracking();
    m_cameraManager.closeCamera();
    if (m_serialController.isOpen()) {
        m_serialController.closeSerial();
    }
    delete ui;
}

QString MainWindow::currentTrackingModelFileName() const
{
    if (!ui || !ui->comboBox_4) {
        return QStringLiteral("yolov8s.onnx");
    }

    const QString modelFileName = ui->comboBox_4->currentData().toString();
    if (modelFileName.isEmpty() || modelFileName == QStringLiteral("feature")) {
        return QStringLiteral("yolov8s.onnx");
    }
    return modelFileName;
}

MainWindow::TrackingBackend MainWindow::currentTrackingBackend() const
{
    if (!ui || !ui->comboBox_4) {
        return TrackingBackend::Yolo;
    }
    return ui->comboBox_4->currentData().toString() == QStringLiteral("feature")
               ? TrackingBackend::Feature
               : TrackingBackend::Yolo;
}

bool MainWindow::isFeatureTrackingSelected() const
{
    return currentTrackingBackend() == TrackingBackend::Feature;
}

void MainWindow::onTrackingModelChanged(int index)
{
    if (index < 0) return;

    const bool wasAutoTracking = m_isCapturing && ui->label_11->text() == QStringLiteral("自动");
    m_isCapturing = false;
    m_isTargetTracked = false;
    m_trackedRect = cv::Rect2d();
    m_offsetX = 0;
    m_offsetY = 0;
    m_trackingEngine.stopTracking();
    if (wasAutoTracking) {
        sendCommand(0x12);
    }

    if (isFeatureTrackingSelected()) {
        m_trackingEngine.setTrackingBackend(true, QStringLiteral("feature"));
        m_trackingEngine.setCurrentModel(QStringLiteral("yolov8s.onnx"));
        ui->btnStartTracking->setEnabled(true);
        ui->btnStartTracking->setToolTip(QString());
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【系统】已切换到特征跟踪：CSRT 主跟踪 + YOLO 候选重捕 + 外观校验。"));
    } else {
        const QString modelFileName = currentTrackingModelFileName();
        m_trackingEngine.setTrackingBackend(false, modelFileName);
        m_trackingEngine.setCurrentModel(modelFileName);
    }
}

void MainWindow::testDNN()
{
    try {
        cv::Mat dummyInput = cv::Mat::zeros(224, 224, CV_8UC3);
        cv::Mat blob = cv::dnn::blobFromImage(
            dummyInput, 1.0, cv::Size(224, 224), cv::Scalar(0, 0, 0), true, false);
        Q_UNUSED(blob);
    } catch (...) {
    }
}

void MainWindow::on_pushButton_clicked()
{
    if (ui->pushButton->text() == QStringLiteral("打开串口")) {
        if (ui->comboBox->currentText().isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("未检测到可用串口！"));
            return;
        }

        if (m_serialController.openSerial(ui->comboBox->currentText(), ui->comboBox_2->currentText().toInt())) {
            ui->lineEdit->setText(QStringLiteral("已连接"));
            disconnect(&m_serialController, &SerialController::telemetryReceived, this, nullptr);
            connect(&m_serialController, &SerialController::telemetryReceived, this,
                    [this](int remoteMode, const QString& param1, const QString& param2) {
                ui->plainTextEdit_3->setPlainText(param1);
                ui->plainTextEdit_4->setPlainText(param2);

                static int lastRemoteMode = -1;
                if (remoteMode == lastRemoteMode) {
                    return;
                }
                lastRemoteMode = remoteMode;

                {
                    QMutexLocker locker(&m_modeMutex);
                    m_currentMode = remoteMode;
                }
                if (remoteMode == 0) {
                    ui->label_11->setText(QStringLiteral("手动"));
                    ui->label_11->setStyleSheet(QStringLiteral("color: black; font-size: 14px; font-weight: bold;"));
                } else {
                    ui->label_11->setText(QStringLiteral("自动"));
                    ui->label_11->setStyleSheet(QStringLiteral("color: red; font-size: 14px; font-weight: bold;"));
                }

                m_isCapturing = false;
                m_trackingEngine.stopTracking();
                m_trackedRect = cv::Rect2d();
                m_isTargetTracked = false;
                m_offsetX = 0;
                m_offsetY = 0;
            });

            ui->pushButton->setText(QStringLiteral("关闭串口"));
            ui->LED1->setStyleSheet(QStringLiteral("background-color:green"));
            ui->pushButton_2->setEnabled(true);
            ui->pushButton_3->setEnabled(true);
            ui->plainTextEdit_2->appendPlainText(QStringLiteral("【串口】已连接，状态同步完成"));
        } else {
            ui->lineEdit->setText(QStringLiteral("串口打开失败！"));
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("串口打开失败！"));
        }
    } else {
        ui->lineEdit->setText(QStringLiteral("未连接"));
        ui->pushButton->setText(QStringLiteral("打开串口"));
        ui->LED1->setStyleSheet(QStringLiteral("background-color:red"));
        disconnect(&m_serialController, &SerialController::telemetryReceived, this, nullptr);
        m_serialController.closeSerial();
        ui->pushButton_2->setEnabled(false);
        ui->pushButton_3->setEnabled(false);
    }
}

void MainWindow::on_pushButton_8_clicked()
{
    ui->comboBox->clear();
    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        ui->comboBox->addItem(info.portName());
    }
}

void MainWindow::onCameraChanged(int index)
{
    if (index < 0) return;
    if (m_cameraManager.state() == CameraManager::CameraState::Open) {
        m_cameraManager.closeCamera();
        m_cameraManager.openCamera(ui->comboBox_3->itemData(index).toString());
    }
}

void MainWindow::on_pushButton_9_clicked()
{
    if (m_cameraManager.state() == CameraManager::CameraState::Idle ||
        m_cameraManager.state() == CameraManager::CameraState::Error) {
        ui->pushButton_9->setText(QStringLiteral("正在打开..."));
        ui->pushButton_9->setEnabled(false);
        m_cameraManager.openCamera(ui->comboBox_3->currentData().toString());
        return;
    }

    ui->pushButton_9->setText(QStringLiteral("正在关闭..."));
    ui->pushButton_9->setEnabled(false);
    m_cameraManager.closeCamera();

    {
        QMutexLocker locker(&m_pendingFrameMutex);
        m_pendingVideoFrame = QVideoFrame();
        m_frameDispatchPending = false;
    }

    m_isCapturing = false;
    m_isSelecting = false;
    m_hasSelectedTarget = false;
    m_wasTrackingBeforeDisconn = false;
    m_trackingEngine.stopTracking();
    m_selectedRect = cv::Rect2d();
    m_trackedRect = cv::Rect2d();
    m_isTargetTracked = false;
    m_lastFrame.release();

    QPixmap placeholder(ui->imageLabel->size());
    placeholder.fill(QColor(50, 50, 50));
    QPainter painter(&placeholder);
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setPointSize(12);
    painter.setFont(font);
    painter.drawText(ui->imageLabel->rect(), Qt::AlignCenter, QStringLiteral("摄像头已关闭\n请点击打开"));
    painter.end();
    ui->imageLabel->setPixmap(placeholder);
    ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】已关闭"));
}

cv::Mat MainWindow::QImageToCvMat(const QImage& qImage)
{
    if (qImage.isNull()) return cv::Mat();
    const QImage converted = qImage.convertToFormat(QImage::Format_RGB888);
    cv::Mat rgb(converted.height(), converted.width(), CV_8UC3,
                const_cast<uchar*>(converted.bits()), converted.bytesPerLine());
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    return bgr.clone();
}

cv::Mat MainWindow::QVideoFrameToCvMat(const QVideoFrame &frame)
{
    if (!frame.isValid()) return cv::Mat();
    const QImage qImage = frame.toImage();
    if (qImage.isNull()) return cv::Mat();
    return QImageToCvMat(qImage);
}

QImage MainWindow::CvMatToQImage(const cv::Mat& mat)
{
    if (mat.empty()) return QImage();
    if (mat.type() == CV_8UC3) {
        cv::Mat rgbMat;
        cv::cvtColor(mat, rgbMat, cv::COLOR_BGR2RGB);
        return QImage(
            reinterpret_cast<const uchar*>(rgbMat.data),
            rgbMat.cols,
            rgbMat.rows,
            static_cast<qsizetype>(rgbMat.step),
            QImage::Format_RGB888).copy();
    }
    if (mat.type() == CV_8UC1) {
        return QImage(
            reinterpret_cast<const uchar*>(mat.data),
            mat.cols,
            mat.rows,
            static_cast<qsizetype>(mat.step),
            QImage::Format_Grayscale8).copy();
    }
    return QImage();
}

void MainWindow::handleNewVideoFrame(const QVideoFrame &frame)
{
    if (!frame.isValid()) return;

    bool shouldDispatch = false;
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        m_pendingVideoFrame = frame;
        if (!m_frameDispatchPending) {
            m_frameDispatchPending = true;
            shouldDispatch = true;
        }
    }

    if (shouldDispatch) {
        QMetaObject::invokeMethod(
            this, &MainWindow::processLatestVideoFrame, Qt::QueuedConnection);
    }
}

void MainWindow::processLatestVideoFrame()
{
    const auto resetFrameDispatch = qScopeGuard([this]() {
        QMutexLocker locker(&m_pendingFrameMutex);
        m_frameDispatchPending = false;
    });

    QVideoFrame frame;
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        frame = m_pendingVideoFrame;
        m_pendingVideoFrame = QVideoFrame();
    }

    if (m_cameraManager.state() != CameraManager::CameraState::Open || !frame.isValid()) return;

    cv::Mat cvMat;
    try {
        cvMat = QVideoFrameToCvMat(frame);
    } catch (...) {
        return;
    }
    if (cvMat.empty()) return;

    cv::flip(cvMat, cvMat, -1);
    {
        QMutexLocker locker(&m_frameSizeMutex);
        m_frameSize = QSize(cvMat.cols, cvMat.rows);
    }

    if (m_forceResetTracking) {
        if (m_wasTrackingBeforeDisconn && m_hasSelectedTarget) {
            const cv::Rect2d frameBounds(0, 0, cvMat.cols, cvMat.rows);
            m_selectedRect = m_lastSelectedRect & frameBounds;
            if (m_selectedRect.width >= 8 && m_selectedRect.height >= 8) {
                const bool useFeatureTracking = isFeatureTrackingSelected();
                m_trackingEngine.startTracking(
                    cvMat,
                    m_selectedRect,
                    useFeatureTracking,
                    useFeatureTracking ? QStringLiteral("feature") : currentTrackingModelFileName());
                ui->plainTextEdit_2->appendPlainText(QStringLiteral("【系统】摄像头已恢复，正在重新锁定目标"));
            }
        }
        m_forceResetTracking = false;
        m_wasTrackingBeforeDisconn = false;
    }

    m_trackingEngine.setFrameSize(QSize(cvMat.cols, cvMat.rows));
    m_trackingEngine.processFrame(cvMat);
    if (m_cameraManager.state() != CameraManager::CameraState::Open) return;
    m_lastFrame = cvMat.clone();

    const QImage img = CvMatToQImage(cvMat);
    if (!img.isNull()) {
        const QSize labelSize = ui->imageLabel->size();
        if (labelSize.width() >= 50 && labelSize.height() >= 50) {
            const QSize imageSize = img.size();
            if (m_renderCanvas.size() != labelSize) {
                m_renderCanvas = QPixmap(labelSize);
            }

            QPainter painter(&m_renderCanvas);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            m_renderCanvas.fill(Qt::black);

            const QSize scaledImageSize = imageSize.scaled(labelSize, Qt::KeepAspectRatio);
            const int xOffset = (labelSize.width() - scaledImageSize.width()) / 2;
            const int yOffset = (labelSize.height() - scaledImageSize.height()) / 2;
            painter.drawImage(QRect(QPoint(xOffset, yOffset), scaledImageSize), img);

            const double scaleX = static_cast<double>(scaledImageSize.width()) / imageSize.width();
            const double scaleY = static_cast<double>(scaledImageSize.height()) / imageSize.height();
            const int centerX = labelSize.width() / 2;
            const int centerY = labelSize.height() / 2;
            constexpr int crossLength = 30;

            painter.setPen(QPen(Qt::red, 3));
            painter.drawLine(centerX, centerY - crossLength, centerX, centerY + crossLength);
            painter.drawLine(centerX - crossLength, centerY, centerX + crossLength, centerY);

            if (!m_waitingForRecover && m_isTargetTracked &&
                m_trackedRect.width > 5 && m_trackedRect.height > 5) {
                const QRectF qtTrackedRect(
                    xOffset + m_trackedRect.x * scaleX,
                    yOffset + m_trackedRect.y * scaleY,
                    m_trackedRect.width * scaleX,
                    m_trackedRect.height * scaleY);
                painter.setPen(QPen(Qt::green, 4));
                painter.drawRect(qtTrackedRect);
                QFont trackFont = painter.font();
                trackFont.setPointSize(10);
                trackFont.setBold(true);
                painter.setFont(trackFont);
                painter.drawText(
                    qtTrackedRect.topLeft() + QPointF(5, -5),
                    isFeatureTrackingSelected() ? QStringLiteral("CSRT+ORB TRACK")
                                                : QStringLiteral("YOLO TRACK"));
            }

            if (m_isSelecting && m_selectStartImg != m_selectEndImg) {
                const QRect originalSelectRect = QRect(m_selectStartImg, m_selectEndImg).normalized();
                const QRectF scaledSelectRect(
                    xOffset + originalSelectRect.x() * scaleX,
                    yOffset + originalSelectRect.y() * scaleY,
                    originalSelectRect.width() * scaleX,
                    originalSelectRect.height() * scaleY);
                painter.setPen(QPen(Qt::blue, 3));
                painter.drawRect(scaledSelectRect);
                QFont selFont = painter.font();
                selFont.setPointSize(10);
                painter.setFont(selFont);
                painter.drawText(scaledSelectRect.topLeft() + QPointF(5, -5), QStringLiteral("Select"));
            }

            static qint64 lastFpsCalcTime = QDateTime::currentMSecsSinceEpoch();
            static int frameCount = 0;
            static int displayedFps = 0;

            frameCount++;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - lastFpsCalcTime >= 1000) {
                displayedFps = frameCount;
                frameCount = 0;
                lastFpsCalcTime = now;
            }

            QFont fpsFont = painter.font();
            fpsFont.setPointSize(10);
            fpsFont.setBold(true);
            painter.setFont(fpsFont);
            painter.setPen(Qt::black);
            painter.drawText(32, 72, QStringLiteral("FPS: %1").arg(displayedFps));
            painter.setPen(Qt::yellow);
            painter.drawText(30, 70, QStringLiteral("FPS: %1").arg(displayedFps));

            painter.end();
            ui->imageLabel->setPixmap(m_renderCanvas);
        }
    }

    {
        QMutexLocker modeLocker(&m_modeMutex);
        if (m_serialController.isOpen() && m_currentMode == 1 && m_isTargetTracked) {
            const qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
            if (currentTime - m_lastSerialSendTime >= 20) {
                m_serialController.sendTrackData(m_offsetX, m_offsetY);
                m_lastSerialSendTime = currentTime;
            }
        }
    }

    static qint64 lastInfoUpdateTime = 0;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - lastInfoUpdateTime >= 100) {
        lastInfoUpdateTime = nowMs;
        if (m_isCapturing && !m_trackedRect.empty()) {
            QString info;
            info += QStringLiteral("X 偏移：%1\n").arg(m_offsetX);
            info += QStringLiteral("Y 偏移：%1\n").arg(m_offsetY);
            info += (m_offsetX > 0) ? QStringLiteral("舵机X → 向右转\n")
                                    : (m_offsetX < 0) ? QStringLiteral("舵机X → 向左转\n")
                                                      : QStringLiteral("舵机X → 居中\n");
            info += (m_offsetY > 0) ? QStringLiteral("舵机Y → 向下转\n")
                                    : (m_offsetY < 0) ? QStringLiteral("舵机Y → 向上转\n")
                                                      : QStringLiteral("舵机Y → 居中\n");
            ui->plainTextEdit->setPlainText(info);
        } else if (!m_waitingForRecover) {
            ui->plainTextEdit->setPlainText(QStringLiteral("未追踪到目标\n舵机保持居中"));
        }
    }
}

void MainWindow::on_btnSelectTarget_clicked()
{
    if (m_cameraManager.state() != CameraManager::CameraState::Open) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先打开摄像头！"));
        return;
    }

    m_trackingEngine.stopTracking();
    m_isCapturing = false;
    m_hasSelectedTarget = false;
    m_trackedRect = cv::Rect2d();
    m_isTargetTracked = false;
    m_isSelecting = true;
    m_lostFrameCount = 0;
    m_selectStart = QPoint();
    m_selectEnd = QPoint();
    m_selectStartImg = QPoint();
    m_selectEndImg = QPoint();
    ui->plainTextEdit_2->appendPlainText(QStringLiteral("【提示】正在框选，请在画面内拖动鼠标..."));
}

void MainWindow::on_btnStartTracking_clicked()
{
    if (m_cameraManager.state() != CameraManager::CameraState::Open) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先打开摄像头！"));
        return;
    }
    if (!m_hasSelectedTarget) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先选择目标！"));
        return;
    }

    const bool useFeatureTracking = isFeatureTrackingSelected();
    if (!useFeatureTracking && !m_trackingEngine.yoloReady()) {
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【错误】YOLO 模型加载失败，请检查程序目录或当前工作目录中的 yolov8s.onnx / yolov8n.onnx！"));
        return;
    }
    if (!useFeatureTracking && !m_trackingEngine.yoloWarmedUp()) {
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【提示】YOLO 引擎正在后台预热，请稍候再开始跟踪。"));
        return;
    }

    cv::Mat currentFrameClone;
    if (!m_lastFrame.empty()) {
        currentFrameClone = m_lastFrame.clone();
    }
    if (currentFrameClone.empty()) {
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【提示】尚未收到摄像头画面，请稍后重试。"));
        return;
    }

    m_isCapturing = true;
    m_wasTrackingBeforeDisconn = true;
    m_lostFrameCount = 0;
    m_trackingEngine.startTracking(
        currentFrameClone,
        m_selectedRect,
        useFeatureTracking,
        useFeatureTracking ? QStringLiteral("feature") : currentTrackingModelFileName());

    if (ui->label_11->text() == QStringLiteral("自动")) {
        sendCommand(0x11);
    }
    ui->plainTextEdit_2->appendPlainText(
        useFeatureTracking ? QStringLiteral("开始特征跟踪（CSRT + ORB）！")
                           : QStringLiteral("开始纯 YOLOv8 跟踪！"));
}

void MainWindow::on_btnStopTracking_clicked()
{
    cv::Mat currentFrameClone;
    if (!m_lastFrame.empty()) {
        currentFrameClone = m_lastFrame.clone();
    }

    m_isCapturing = false;
    m_wasTrackingBeforeDisconn = false;
    m_trackingEngine.stopTracking();
    m_trackedRect = cv::Rect2d();
    m_isTargetTracked = false;
    m_offsetX = 0;
    m_offsetY = 0;

    if (!currentFrameClone.empty()) {
        const QImage img = CvMatToQImage(currentFrameClone);
        if (!img.isNull()) {
            ui->imageLabel->setPixmap(QPixmap::fromImage(img).scaled(
                ui->imageLabel->size(),
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation));
        }
    }
    if (ui->label_11->text() == QStringLiteral("自动")) {
        sendCommand(0x12);
    }
    ui->plainTextEdit_2->appendPlainText(QStringLiteral("停止跟踪！"));
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    if (!m_isSelecting) {
        QMainWindow::mousePressEvent(event);
        return;
    }
    QPoint localPos = ui->imageLabel->mapFromGlobal(event->globalPosition().toPoint());
    if (!ui->imageLabel->rect().contains(localPos) || event->button() != Qt::LeftButton) {
        QMainWindow::mousePressEvent(event);
        return;
    }

    m_selectStart = localPos;
    m_selectEnd = localPos;
    QSize frameSize;
    {
        QMutexLocker locker(&m_frameSizeMutex);
        frameSize = m_frameSize;
    }
    if (frameSize.isEmpty()) return;

    const QSize imageSize = frameSize;
    const QSize labelSize = ui->imageLabel->size();
    const QSize scaledImageSize = imageSize.scaled(labelSize, Qt::KeepAspectRatio);
    const int xOffset = (labelSize.width() - scaledImageSize.width()) / 2;
    const int yOffset = (labelSize.height() - scaledImageSize.height()) / 2;
    const QRect displayedImageRect(QPoint(xOffset, yOffset), scaledImageSize);
    if (!displayedImageRect.contains(localPos)) return;

    const double scaleX = static_cast<double>(imageSize.width()) / scaledImageSize.width();
    const double scaleY = static_cast<double>(imageSize.height()) / scaledImageSize.height();
    m_selectStartImg = QPoint(
        static_cast<int>((localPos.x() - xOffset) * scaleX),
        static_cast<int>((localPos.y() - yOffset) * scaleY));
    m_selectEndImg = m_selectStartImg;
}

void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_isSelecting) {
        QMainWindow::mouseMoveEvent(event);
        return;
    }

    QPoint localPos = ui->imageLabel->mapFromGlobal(event->globalPosition().toPoint());
    if (!ui->imageLabel->rect().contains(localPos)) return;
    m_selectEnd = localPos;

    QSize frameSize;
    {
        QMutexLocker locker(&m_frameSizeMutex);
        frameSize = m_frameSize;
    }
    if (frameSize.isEmpty()) return;

    const QSize imageSize = frameSize;
    const QSize labelSize = ui->imageLabel->size();
    const QSize scaledImageSize = imageSize.scaled(labelSize, Qt::KeepAspectRatio);
    const int xOffset = (labelSize.width() - scaledImageSize.width()) / 2;
    const int yOffset = (labelSize.height() - scaledImageSize.height()) / 2;
    const QRect displayedImageRect(QPoint(xOffset, yOffset), scaledImageSize);
    localPos.setX(std::clamp(localPos.x(), displayedImageRect.left(), displayedImageRect.right()));
    localPos.setY(std::clamp(localPos.y(), displayedImageRect.top(), displayedImageRect.bottom()));

    const double scaleX = static_cast<double>(imageSize.width()) / scaledImageSize.width();
    const double scaleY = static_cast<double>(imageSize.height()) / scaledImageSize.height();
    m_selectEndImg = QPoint(
        static_cast<int>((localPos.x() - xOffset) * scaleX),
        static_cast<int>((localPos.y() - yOffset) * scaleY));
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_isSelecting) {
        QMainWindow::mouseReleaseEvent(event);
        return;
    }

    QPoint localPos = ui->imageLabel->mapFromGlobal(event->globalPosition().toPoint());
    if (!ui->imageLabel->rect().contains(localPos) || event->button() != Qt::LeftButton) {
        QMainWindow::mouseReleaseEvent(event);
        return;
    }
    m_selectEnd = localPos;

    QSize frameSize;
    {
        QMutexLocker locker(&m_frameSizeMutex);
        frameSize = m_frameSize;
    }
    if (frameSize.isEmpty()) return;

    const QSize imageSize = frameSize;
    const QSize labelSize = ui->imageLabel->size();
    const QSize scaledImageSize = imageSize.scaled(labelSize, Qt::KeepAspectRatio);
    const int xOffset = (labelSize.width() - scaledImageSize.width()) / 2;
    const int yOffset = (labelSize.height() - scaledImageSize.height()) / 2;
    const QRect displayedImageRect(QPoint(xOffset, yOffset), scaledImageSize);
    localPos.setX(std::clamp(localPos.x(), displayedImageRect.left(), displayedImageRect.right()));
    localPos.setY(std::clamp(localPos.y(), displayedImageRect.top(), displayedImageRect.bottom()));

    const double scaleX = static_cast<double>(imageSize.width()) / scaledImageSize.width();
    const double scaleY = static_cast<double>(imageSize.height()) / scaledImageSize.height();
    m_selectEndImg = QPoint(
        static_cast<int>((localPos.x() - xOffset) * scaleX),
        static_cast<int>((localPos.y() - yOffset) * scaleY));
    m_isSelecting = false;

    const QRect selectRect = QRect(m_selectStartImg, m_selectEndImg)
                                 .normalized()
                                 .intersected(QRect(0, 0, frameSize.width(), frameSize.height()));
    if (selectRect.width() < 8 || selectRect.height() < 8) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("框选区域太小！"));
        return;
    }

    m_selectedRect = cv::Rect2d(selectRect.x(), selectRect.y(), selectRect.width(), selectRect.height());
    m_hasSelectedTarget = true;
    m_lastSelectedRect = m_selectedRect;

    const int maxEdge = std::max(selectRect.width(), selectRect.height());
    if (maxEdge > 180) {
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【提示】框选区域较大，可能降低跟踪帧率。建议仅框选核心局部！"));
    } else {
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【提示】目标模板已锁定。"));
    }
}

void MainWindow::paintEvent(QPaintEvent *event)
{
    QMainWindow::paintEvent(event);
}

void MainWindow::on_pushButton_2_clicked()
{
    QMutexLocker locker(&m_modeMutex);
    ui->pushButton_2->setEnabled(false);
    QTimer::singleShot(100, this, [this]() { ui->pushButton_2->setEnabled(true); });

    m_isCapturing = false;
    m_trackingEngine.stopTracking();
    m_trackedRect = cv::Rect2d();
    m_isTargetTracked = false;
    m_hasSelectedTarget = false;
    m_isSelecting = false;
    m_offsetX = 0;
    m_offsetY = 0;
    sendCommand(0x02);
}

void MainWindow::on_pushButton_3_clicked()
{
    if (!m_serialController.isOpen()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先连接串口！"));
        return;
    }

    QMutexLocker locker(&m_modeMutex);
    m_currentMode = !m_currentMode;
    if (m_currentMode == 0) {
        ui->label_11->setText(QStringLiteral("手动"));
        ui->label_11->setStyleSheet(QStringLiteral("color: black; font-size: 14px; font-weight: bold;"));
    } else {
        ui->label_11->setText(QStringLiteral("自动"));
        ui->label_11->setStyleSheet(QStringLiteral("color: red; font-size: 14px; font-weight: bold;"));
    }

    m_isCapturing = false;
    m_trackingEngine.stopTracking();
    m_trackedRect = cv::Rect2d();
    m_isTargetTracked = false;
    m_hasSelectedTarget = false;
    m_isSelecting = false;
    m_offsetX = 0;
    m_offsetY = 0;
    sendCommand(0x01);

    ui->pushButton_3->setEnabled(false);
    QTimer::singleShot(100, this, [this]() { ui->pushButton_3->setEnabled(true); });
}

void MainWindow::sendCommand(uint8_t cmd)
{
    m_serialController.sendCommand(cmd);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_trackingEngine.stopTracking();
    if (m_serialController.isOpen()) {
        sendCommand(0x12);
        sendCommand(0x02);
        m_serialController.closeSerial();
    }
    m_cameraManager.closeCamera();
    event->accept();
}
