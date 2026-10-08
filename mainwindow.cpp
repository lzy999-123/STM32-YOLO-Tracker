#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QCameraDevice>
#include <QComboBox>
#include <QDateTime>
#include <QMediaDevices>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QScopeGuard>
#include <QSerialPortInfo>
#include <QTimer>

#include <algorithm>

namespace {
constexpr int kRealtimeWidth = 1280;
constexpr int kRealtimeHeight = 720;

QString selectedCameraSource(const QComboBox *comboBox)
{
    const int index = comboBox->currentIndex();
    const QString editText = comboBox->currentText().trimmed();
    if (index >= 0 && editText == comboBox->itemText(index)) {
        return comboBox->itemData(index).toString();
    }
    return editText;
}

QString normalizedRtspSource(QString source)
{
    if (source.isEmpty() || source.startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive)) {
        return source;
    }

    source.prepend(QStringLiteral("rtsp://"));
    const int authorityStart = source.indexOf(QStringLiteral("://")) + 3;
    if (source.indexOf(QLatin1Char('/'), authorityStart) < 0) {
        source.append(QStringLiteral("/live/0"));
    }
    return source;
}
}

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
    // 手动步进固定为每次 0.5°，避免 UI 数值与下位机步进协议不一致。
    ui->doubleSpinBox->setDecimals(2);
    ui->doubleSpinBox->setRange(0.5, 0.5);
    ui->doubleSpinBox->setSingleStep(0.5);
    ui->doubleSpinBox->setValue(0.5);
    ui->doubleSpinBox->setReadOnly(true);
    for (QPushButton *button : {ui->pushButton_4, ui->pushButton_5,
                                ui->pushButton_6, ui->pushButton_7}) {
        button->setEnabled(false);
        button->setToolTip(QStringLiteral("仅手动模式可用，每次转动 0.5°"));
    }
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
        updateTrackingControlAvailability();
    });

    // 串口链路状态信号（协议 v2）：断线/重连/下位机在线/命令失败
    connect(&m_serialController, &SerialController::connectionLost, this,
            [this](const QString &reason) {
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【串口】连接丢失：%1，自动重连中...").arg(reason));
        ui->LED1->setStyleSheet(QStringLiteral("background-color:yellow"));
        ui->lineEdit->setText(QStringLiteral("重连中..."));
        ui->pushButton_2->setEnabled(false);
        ui->pushButton_3->setEnabled(false);
        updateManualControlAvailability();
        updateTrackingControlAvailability();
    });
    connect(&m_serialController, &SerialController::reconnected, this, [this]() {
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【串口】自动重连成功"));
        ui->LED1->setStyleSheet(QStringLiteral("background-color:green"));
        ui->lineEdit->setText(QStringLiteral("已连接"));
        ui->pushButton_2->setEnabled(true);
        ui->pushButton_3->setEnabled(true);
        updateManualControlAvailability();
        updateTrackingControlAvailability();
    });
    connect(&m_serialController, &SerialController::deviceOnlineChanged, this,
            [this](bool online) {
        if (online) {
            ui->plainTextEdit_2->appendPlainText(QStringLiteral("【串口】下位机已上线"));
            m_lastRemoteMode = -1; // 强制下一帧遥测刷新模式显示（label_11 可能停留在"离线"）
        } else {
            ui->plainTextEdit_2->appendPlainText(
                QStringLiteral("【串口】下位机无响应（遥测超时）"));
            ui->label_11->setText(QStringLiteral("离线"));
            ui->label_11->setStyleSheet(
                QStringLiteral("color: gray; font-size: 14px; font-weight: bold;"));
        }
        updateManualControlAvailability();
        updateTrackingControlAvailability();
    });
    connect(&m_serialController, &SerialController::commandFailed, this,
            [this](uint8_t cmd) {
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【串口】命令 0x%1 发送失败（无 ACK）")
                .arg(cmd, 2, 16, QLatin1Char('0')));
    });

    ui->comboBox_4->clear();
    ui->comboBox_4->addItem(QStringLiteral("YOLO26s（精度优先）"), QStringLiteral("yolo26s.onnx"));
    ui->comboBox_4->addItem(QStringLiteral("YOLO26n（速度优先）"), QStringLiteral("yolo26n.onnx"));
    ui->comboBox_4->addItem(QStringLiteral("特征跟踪 CSRT+ORB（任意物体）"), QStringLiteral("feature"));
    connect(ui->comboBox_4, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTrackingModelChanged);
    m_trackingEngine.preloadYoloModels();
    m_trackingEngine.setCurrentModel(currentTrackingModelFileName());

    ui->comboBox_3->clear();
    ui->comboBox_3->setEditable(true);

    // 网线直连模式
    ui->comboBox_3->addItem(QStringLiteral("【网线直连】Luckfox 原生720P@30主码流 (192.168.8.93)"),
                            QStringLiteral("rtsp://192.168.8.93/live/0"));

    // USB 直连模式（172.32.0.93，720P 极速低延迟）
    ui->comboBox_3->addItem(QStringLiteral("【USB直连】Luckfox 原生720P@30主码流 (172.32.0.93)"),
                            QStringLiteral("rtsp://172.32.0.93/live/0"));
    ui->comboBox_3->addItem(QStringLiteral("【USB直连】Luckfox备用子码流 (当前实测704×576@30，不满足720P)"),
                            QStringLiteral("rtsp://172.32.0.93/live/1"));

    ui->comboBox_3->addItem(QStringLiteral("Luckfox 自动识别"),
                            QStringLiteral("luckfox:auto"));

    // 添加本地 USB 硬件摄像头
    QList<QCameraDevice> cameraDevices;
    try {
        cameraDevices = QMediaDevices::videoInputs();
    } catch (...) {
    }

    for (const QCameraDevice &device : cameraDevices) {
        ui->comboBox_3->addItem(QStringLiteral("【本地免驱720P】") + device.description(),
                                QString::fromUtf8(device.id()));
    }

    // 默认通过设备发现获取热点当前分配的地址。
    ui->comboBox_3->setCurrentIndex(3);
    ui->comboBox_3->setEnabled(true);
    ui->pushButton_9->setEnabled(true);
    ui->pushButton_9->setText(QStringLiteral("打开摄像头"));

    connect(&m_luckfoxDiscovery, &LuckfoxDiscovery::found, this, [this](const QString &url) {
        ui->comboBox_3->setEnabled(true);
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】已找到 Luckfox：%1，正在连接...").arg(url));
        m_cameraManager.openCamera(url);
    });
    connect(&m_luckfoxDiscovery, &LuckfoxDiscovery::failed, this, [this](const QString &message) {
        ui->comboBox_3->setEnabled(true);
        ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
        ui->pushButton_9->setEnabled(true);
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】%1").arg(message));
    });

    QPushButton *btnRotate = new QPushButton(this);
    btnRotate->setGeometry(415, 380, 80, 31);
    btnRotate->setText(QStringLiteral("画面: 正常"));
    btnRotate->setToolTip(QStringLiteral("点击循环切换画面方向：正常 -> 180° -> 水平镜像 -> 垂直翻转 -> 90° -> 270°"));
    connect(btnRotate, &QPushButton::clicked, this, [this, btnRotate]() {
        int mode = static_cast<int>(m_cameraManager.rotationMode());
        mode = (mode + 1) % 6;
        m_cameraManager.setRotationMode(static_cast<CameraManager::RotationMode>(mode));
        switch (mode) {
            case 0: btnRotate->setText(QStringLiteral("画面: 正常")); break;
            case 1: btnRotate->setText(QStringLiteral("画面: 180°")); break;
            case 2: btnRotate->setText(QStringLiteral("画面: 水平")); break;
            case 3: btnRotate->setText(QStringLiteral("画面: 垂直")); break;
            case 4: btnRotate->setText(QStringLiteral("画面: 90°")); break;
            case 5: btnRotate->setText(QStringLiteral("画面: 270°")); break;
        }
    });

    connect(ui->comboBox_3, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onCameraChanged);
    connect(&m_cameraManager, &CameraManager::frameReady,
            this, &MainWindow::handleNewVideoFrame);
    connect(&m_cameraManager, &CameraManager::matReady,
            this, &MainWindow::handleNewMatFrame);
    connect(&m_cameraManager, &CameraManager::stateChanged, this,
            [this](CameraManager::CameraState state) {
        if (state == CameraManager::CameraState::Idle) {
            ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
            ui->pushButton_9->setEnabled(true);
        } else if (state == CameraManager::CameraState::Open) {
            ui->pushButton_9->setText(QStringLiteral("关闭摄像头"));
            ui->pushButton_9->setEnabled(true);
            if (m_cameraManager.currentCameraId().startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive)) {
                ui->plainTextEdit_2->appendPlainText(QStringLiteral("【图像通道】RTSP 视频流已连接"));
            }
        } else if (state == CameraManager::CameraState::Opening) {
            m_lastFpsCalcTime = 0;
            m_fpsFrameCount = 0;
            m_displayedFps = 0;
            m_lastFpsLogTime = 0;
            m_lastDisplayFrameTime = 0;
            ui->pushButton_9->setText(QStringLiteral("正在打开..."));
            ui->pushButton_9->setEnabled(false);
        } else if (state == CameraManager::CameraState::Closing) {
            ui->pushButton_9->setText(QStringLiteral("正在关闭..."));
            ui->pushButton_9->setEnabled(false);
        } else {
            ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
            ui->pushButton_9->setEnabled(true);
        }
        updateTrackingControlAvailability();
    });
    connect(&m_cameraManager, &CameraManager::cameraError, this, [this](const QString &errorMsg) {
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】%1").arg(errorMsg));
        ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
        ui->pushButton_9->setEnabled(true);
        updateTrackingControlAvailability();
    });
    m_cameraManager.startChecking();
    updateTrackingControlAvailability();
}

MainWindow::~MainWindow()
{
    m_luckfoxDiscovery.cancel();
    stopTrackingSafely();
    m_cameraManager.closeCamera();
    if (m_serialController.isOpen()) {
        m_serialController.closeSerial();
    }
    delete ui;
}

void MainWindow::dispatchTrackingFrame(const cv::Mat &frame)
{
    if (frame.empty()) return;

    std::unique_lock<std::mutex> lock(m_trackingWorkerMutex);
    m_pendingTrackingFrame = frame;

    if (m_trackingWorkerRunning.load(std::memory_order_acquire)) {
        return;
    }

    if (m_trackingWorker.joinable()) {
        lock.unlock();
        m_trackingWorker.join();
        lock.lock();
    }

    m_trackingWorkerRunning.store(true, std::memory_order_release);
    m_trackingWorker = std::thread(&MainWindow::trackingWorkerLoop, this);
}

void MainWindow::trackingWorkerLoop()
{
    for (;;) {
        cv::Mat frame;
        {
            std::lock_guard<std::mutex> lock(m_trackingWorkerMutex);
            if (m_pendingTrackingFrame.empty()) {
                m_trackingWorkerRunning.store(false, std::memory_order_release);
                return;
            }
            frame = m_pendingTrackingFrame;
            m_pendingTrackingFrame.release();
        }

        // CSRT/ORB 可能耗时数十毫秒，放在这里执行不会阻塞 Qt GUI 重绘。
        m_trackingEngine.setFrameSize(QSize(frame.cols, frame.rows));
        m_trackingEngine.processFrame(frame);
    }
}

void MainWindow::waitForTrackingWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_trackingWorkerMutex);
        m_pendingTrackingFrame.release();
    }
    if (m_trackingWorker.joinable()) {
        m_trackingWorker.join();
    }
    m_trackingWorkerRunning.store(false, std::memory_order_release);
}

void MainWindow::stopTrackingSafely()
{
    waitForTrackingWorker();
    m_trackingEngine.stopTracking();
}

QString MainWindow::currentTrackingModelFileName() const
{
    if (!ui || !ui->comboBox_4) {
        return QStringLiteral("yolo26s.onnx");
    }

    const QString modelFileName = ui->comboBox_4->currentData().toString();
    if (modelFileName.isEmpty() || modelFileName == QStringLiteral("feature")) {
        return QStringLiteral("yolo26s.onnx");
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
    stopTrackingSafely();
    if (wasAutoTracking) {
        sendCommand(SerialController::CmdTrackOff);
    }

    if (isFeatureTrackingSelected()) {
        m_trackingEngine.setTrackingBackend(true, QStringLiteral("feature"));
        m_trackingEngine.setCurrentModel(QStringLiteral("yolo26s.onnx"));
        ui->btnStartTracking->setEnabled(true);
        ui->btnStartTracking->setToolTip(QString());
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【系统】已切换到特征跟踪：CSRT 主跟踪 + YOLO 候选重捕 + 外观校验。"));
    } else {
        const QString modelFileName = currentTrackingModelFileName();
        m_trackingEngine.setTrackingBackend(false, modelFileName);
        m_trackingEngine.setCurrentModel(modelFileName);
    }
    updateTrackingControlAvailability();
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
            m_lastRemoteMode = -1;
            disconnect(&m_serialController, &SerialController::telemetryReceived, this, nullptr);
            connect(&m_serialController, &SerialController::telemetryReceived, this,
                    [this](int remoteMode, float angle1, float angle2) {
                ui->plainTextEdit_3->setPlainText(QString::number(angle1, 'f', 1));
                ui->plainTextEdit_4->setPlainText(QString::number(angle2, 'f', 1));

                if (remoteMode == m_lastRemoteMode) {
                    updateManualControlAvailability();
                    updateTrackingControlAvailability();
                    return;
                }
                m_lastRemoteMode = remoteMode;

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
                stopTrackingSafely();
                m_trackedRect = cv::Rect2d();
                m_isTargetTracked = false;
                m_offsetX = 0;
                m_offsetY = 0;
                updateManualControlAvailability();
                updateTrackingControlAvailability();
            });

            ui->pushButton->setText(QStringLiteral("关闭串口"));
            ui->LED1->setStyleSheet(QStringLiteral("background-color:green"));
            ui->pushButton_2->setEnabled(true);
            ui->pushButton_3->setEnabled(true);
            updateManualControlAvailability();
            updateTrackingControlAvailability();
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
        m_lastRemoteMode = -1;
        ui->pushButton_2->setEnabled(false);
        ui->pushButton_3->setEnabled(false);
        updateManualControlAvailability();
        updateTrackingControlAvailability();
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
    QString camId = ui->comboBox_3->itemData(index).toString();
    if (camId == QStringLiteral("luckfox:auto")) {
        if (m_cameraManager.state() == CameraManager::CameraState::Open ||
            m_cameraManager.state() == CameraManager::CameraState::Opening) {
            on_pushButton_9_clicked();
        }
        return;
    }
    if (camId.isEmpty()) {
        camId = ui->comboBox_3->currentText().trimmed();
        if (camId == ui->comboBox_3->itemText(index)) {
            return;
        }
    }

    if (camId == m_cameraManager.currentCameraId() && m_cameraManager.state() == CameraManager::CameraState::Open) {
        return;
    }

    // 只有当摄像头处于打开或正在打开状态时，切换下拉框才自动平滑重连新流
    if (m_cameraManager.state() == CameraManager::CameraState::Open ||
        m_cameraManager.state() == CameraManager::CameraState::Opening) {

        // 清理待处理残留帧，防止新旧不同分辨率画面交错闪烁
        {
            QMutexLocker locker(&m_pendingFrameMutex);
            m_hasPendingMat = false;
            m_pendingMat.release();
            m_pendingVideoFrame = QVideoFrame();
            m_frameDispatchPending = false;
        }
        m_lastFrame.release();

        if (camId.contains("172.32.0.93")) {
            ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】切换 -> USB 直连模式（720P 低延迟极速引擎）"));
        } else if (camId.startsWith("rtsp://", Qt::CaseInsensitive)) {
            ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】切换 -> RTSP 网络视频流模式（720P低延迟）"));
        } else {
            ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】切换 -> 本地物理摄像头（720P 高帧率模式）"));
        }

        m_cameraManager.openCamera(camId);
    }
}

void MainWindow::on_pushButton_9_clicked()
{
    if (m_luckfoxDiscovery.isActive()) {
        m_luckfoxDiscovery.cancel();
        ui->comboBox_3->setEnabled(true);
        ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】已取消查找"));
        return;
    }
    if (m_cameraManager.state() == CameraManager::CameraState::Idle ||
        m_cameraManager.state() == CameraManager::CameraState::Error) {
        ui->pushButton_9->setText(QStringLiteral("正在打开..."));
        ui->pushButton_9->setEnabled(false);

        const QString source = selectedCameraSource(ui->comboBox_3);
        if (source == QStringLiteral("luckfox:auto")) {
            ui->comboBox_3->setEnabled(false);
            ui->pushButton_9->setText(QStringLiteral("取消查找"));
            ui->pushButton_9->setEnabled(true);
            ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】正在自动查找 Luckfox..."));
            m_luckfoxDiscovery.start();
            return;
        }
        const int selectedIndex = ui->comboBox_3->currentIndex();
        const bool presetSelected = selectedIndex >= 0 &&
            ui->comboBox_3->currentText().trimmed() == ui->comboBox_3->itemText(selectedIndex);
        const QString camId = presetSelected ? source : normalizedRtspSource(source);
        if (camId.isEmpty()) {
            ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
            ui->pushButton_9->setEnabled(true);
            QMessageBox::warning(this, QStringLiteral("提示"),
                                 QStringLiteral("请输入 Luckfox 的 Wi-Fi IP 或 RTSP 地址。"));
            return;
        }

        if (camId.contains("172.32.0.93")) {
            ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】识别为 USB 直连模式，启用 720P 低延迟极速引擎"));
        } else if (camId.startsWith("rtsp://", Qt::CaseInsensitive)) {
            ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】识别为 RTSP 网络视频流，启用低缓冲取流"));
        } else {
            ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】启用本地物理摄像头（锁定 720P 低延迟模式）"));
        }

        m_cameraManager.openCamera(camId);
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
    stopTrackingSafely();
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
    return bgr;
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
        return QImage(
            reinterpret_cast<const uchar*>(mat.data),
            mat.cols,
            mat.rows,
            static_cast<qsizetype>(mat.step),
            QImage::Format_BGR888); // 消除 .copy() 冗余深拷贝，实现零拷贝内存视图
    }
    if (mat.type() == CV_8UC1) {
        return QImage(
            reinterpret_cast<const uchar*>(mat.data),
            mat.cols,
            mat.rows,
            static_cast<qsizetype>(mat.step),
            QImage::Format_Grayscale8);
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

void MainWindow::handleNewMatFrame(const cv::Mat &mat)
{
    if (mat.empty()) return;

    bool shouldDispatch = false;
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        m_pendingMat = mat;
        m_hasPendingMat = true;
        if (!m_frameDispatchPending) {
            m_frameDispatchPending = true;
            shouldDispatch = true;
        }
    }

    if (shouldDispatch) {
        processLatestVideoFrame();
    }
}

void MainWindow::processLatestVideoFrame()
{
    const auto finishFrameDispatch = qScopeGuard([this]() {
        bool hasPendingFrame = false;
        {
            QMutexLocker locker(&m_pendingFrameMutex);
            hasPendingFrame = m_hasPendingMat || m_pendingVideoFrame.isValid();
            if (!hasPendingFrame) {
                m_frameDispatchPending = false;
            }
        }
        if (hasPendingFrame) {
            QMetaObject::invokeMethod(
                this, &MainWindow::processLatestVideoFrame, Qt::QueuedConnection);
        }
    });

    cv::Mat cvMat;
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        if (m_hasPendingMat) {
            cvMat = m_pendingMat;
            m_hasPendingMat = false;
        } else if (m_pendingVideoFrame.isValid()) {
            QVideoFrame frame = m_pendingVideoFrame;
            m_pendingVideoFrame = QVideoFrame();
            try {
                cvMat = QVideoFrameToCvMat(frame);
                if (!cvMat.empty()) {
                    const int rot = static_cast<int>(m_cameraManager.rotationMode());
                    if (rot == 1) {
                        cv::flip(cvMat, cvMat, -1);
                    } else if (rot == 2) {
                        cv::flip(cvMat, cvMat, 1);
                    } else if (rot == 3) {
                        cv::flip(cvMat, cvMat, 0);
                    } else if (rot == 4) {
                        cv::rotate(cvMat, cvMat, cv::ROTATE_90_CLOCKWISE);
                    } else if (rot == 5) {
                        cv::rotate(cvMat, cvMat, cv::ROTATE_90_COUNTERCLOCKWISE);
                    }
                }
            } catch (...) {
                return;
            }
        }
    }

    if (m_cameraManager.state() != CameraManager::CameraState::Open || cvMat.empty()) return;

    // RTSP 已在后台合并为最新帧；无线到包有抖动，不能再按 20ms 间隔丢帧。
    // 本地摄像头保留上限，避免高帧率设备占满 GUI。
    const qint64 frameNow = QDateTime::currentMSecsSinceEpoch();
    if (!m_cameraManager.currentCameraId().startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive) &&
        m_lastDisplayFrameTime > 0 && frameNow - m_lastDisplayFrameTime < 20) {
        return;
    }
    m_lastDisplayFrameTime = frameNow;

    // 720P 极速约束：无论来自本地 USB 还是网络流，严格保障画面上限为 720P，全链路零负载延迟
    if (cvMat.cols > kRealtimeWidth || cvMat.rows > kRealtimeHeight) {
        const double scale = std::min(static_cast<double>(kRealtimeWidth) / cvMat.cols,
                                      static_cast<double>(kRealtimeHeight) / cvMat.rows);
        const int newW = cvRound(cvMat.cols * scale);
        const int newH = cvRound(cvMat.rows * scale);
        cv::resize(cvMat, cvMat, cv::Size(newW, newH), 0, 0, cv::INTER_LINEAR);
    }

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

    m_lastFrame = cvMat;

    const QSize labelSize = ui->imageLabel->size();
    if (labelSize.width() >= 50 && labelSize.height() >= 50) {
        const QSize imageSize(cvMat.cols, cvMat.rows);
        if (m_renderCanvas.size() != labelSize) {
            m_renderCanvas = QPixmap(labelSize);
        }

        const QSize scaledImageSize = imageSize.scaled(labelSize, Qt::KeepAspectRatio);
        const int xOffset = (labelSize.width() - scaledImageSize.width()) / 2;
        const int yOffset = (labelSize.height() - scaledImageSize.height()) / 2;

        // 关键优化：使用 OpenCV AVX2 高性能缩放替代 QPainter 大图慢速软渲染缩放
        cv::Mat displayMat;
        cv::resize(cvMat, displayMat, cv::Size(scaledImageSize.width(), scaledImageSize.height()), 0, 0, cv::INTER_LINEAR);
        const QImage img = CvMatToQImage(displayMat);
        if (!img.isNull()) {
            QPainter painter(&m_renderCanvas);
            // 视频画面是像素内容，抗锯齿只会增加每帧栅格绘制开销。
            painter.setRenderHint(QPainter::Antialiasing, false);
            m_renderCanvas.fill(Qt::black);
            painter.drawImage(QPoint(xOffset, yOffset), img);

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

            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (m_lastFpsCalcTime == 0) {
                m_lastFpsCalcTime = now;
            }
            ++m_fpsFrameCount;
            if (now - m_lastFpsCalcTime >= 1000) {
                m_displayedFps = qRound(m_fpsFrameCount * 1000.0 / (now - m_lastFpsCalcTime));
                m_fpsFrameCount = 0;
                m_lastFpsCalcTime = now;
                if (now - m_lastFpsLogTime >= 5000) {
                    qDebug() << "[Video] displayed_fps=" << m_displayedFps;
                    m_lastFpsLogTime = now;
                }
            }

            QFont fpsFont = painter.font();
            fpsFont.setPointSize(10);
            fpsFont.setBold(true);
            painter.setFont(fpsFont);
            painter.setPen(Qt::black);
            painter.drawText(32, 72, QStringLiteral("FPS: %1").arg(m_displayedFps));
            painter.setPen(Qt::yellow);
            painter.drawText(30, 70, QStringLiteral("FPS: %1").arg(m_displayedFps));

            painter.end();
            ui->imageLabel->setPixmap(m_renderCanvas);
        }
    }

    // YOLO 本身已经在 DnnThread 中异步执行；CSRT/ORB 的更新则交给单独线程，
    // 否则 setPixmap 之后 GUI 仍会被同步跟踪计算挡住，屏幕实际重绘会继续延迟。
    if (m_isCapturing && isFeatureTrackingSelected()) {
        dispatchTrackingFrame(cvMat);
    } else {
        m_trackingEngine.setFrameSize(QSize(cvMat.cols, cvMat.rows));
        m_trackingEngine.processFrame(cvMat);
    }
    if (m_cameraManager.state() != CameraManager::CameraState::Open) return;

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

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_lastInfoUpdateTime >= 100) {
        m_lastInfoUpdateTime = nowMs;
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
    if (!m_serialController.isOpen()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先连接串口！"));
        return;
    }
    {
        QMutexLocker locker(&m_modeMutex);
        if (m_currentMode != 1) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先切换到自动模式！"));
            return;
        }
    }
    if (m_cameraManager.state() != CameraManager::CameraState::Open) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先打开摄像头！"));
        return;
    }

    stopTrackingSafely();
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
    updateTrackingControlAvailability();
    ui->plainTextEdit_2->appendPlainText(QStringLiteral("【提示】正在框选，请在画面内拖动鼠标..."));
}

void MainWindow::on_btnStartTracking_clicked()
{
    if (!m_serialController.isOpen()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先连接串口！"));
        return;
    }
    {
        QMutexLocker locker(&m_modeMutex);
        if (m_currentMode != 1) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先切换到自动模式！"));
            return;
        }
    }
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
            QStringLiteral("【错误】YOLO26 模型加载失败，请检查程序目录或当前工作目录中的 yolo26s.onnx / yolo26n.onnx！"));
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
    waitForTrackingWorker();
    m_trackingEngine.startTracking(
        currentFrameClone,
        m_selectedRect,
        useFeatureTracking,
        useFeatureTracking ? QStringLiteral("feature") : currentTrackingModelFileName());
    updateTrackingControlAvailability();

    if (ui->label_11->text() == QStringLiteral("自动")) {
        sendCommand(SerialController::CmdTrackOn);
    }
    ui->plainTextEdit_2->appendPlainText(
        useFeatureTracking ? QStringLiteral("开始特征跟踪（CSRT + ORB）！")
                           : QStringLiteral("开始纯 YOLO26 跟踪！"));
}

void MainWindow::on_btnStopTracking_clicked()
{
    cv::Mat currentFrameClone;
    if (!m_lastFrame.empty()) {
        currentFrameClone = m_lastFrame.clone();
    }

    m_isCapturing = false;
    m_wasTrackingBeforeDisconn = false;
    stopTrackingSafely();
    m_trackedRect = cv::Rect2d();
    m_isTargetTracked = false;
    m_offsetX = 0;
    m_offsetY = 0;
    updateTrackingControlAvailability();

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
        sendCommand(SerialController::CmdTrackOff);
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
    updateTrackingControlAvailability();

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
    stopTrackingSafely();
    m_trackedRect = cv::Rect2d();
    m_isTargetTracked = false;
    m_hasSelectedTarget = false;
    m_isSelecting = false;
    m_offsetX = 0;
    m_offsetY = 0;
    sendCommand(SerialController::CmdCenter);
    updateTrackingControlAvailability();
}

void MainWindow::on_pushButton_3_clicked()
{
    if (!m_serialController.isOpen()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先连接串口！"));
        return;
    }

    {
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
        stopTrackingSafely();
        m_trackedRect = cv::Rect2d();
        m_isTargetTracked = false;
        m_hasSelectedTarget = false;
        m_isSelecting = false;
        m_offsetX = 0;
        m_offsetY = 0;
        sendCommand(SerialController::CmdSwitchMode);
    }
    updateManualControlAvailability();

    ui->pushButton_3->setEnabled(false);
    QTimer::singleShot(100, this, [this]() { ui->pushButton_3->setEnabled(true); });
}

void MainWindow::updateManualControlAvailability()
{
    bool manual = false;
    {
        QMutexLocker locker(&m_modeMutex);
        manual = (m_currentMode == 0);
    }

    // 遥测离线时 label_11 会显示“离线”，此时不允许误发手动步进命令。
    const bool enabled = m_serialController.isOpen() && manual &&
                         ui->label_11->text() == QStringLiteral("手动");
    for (QPushButton *button : {ui->pushButton_4, ui->pushButton_5,
                                ui->pushButton_6, ui->pushButton_7}) {
        button->setEnabled(enabled);
    }
}

void MainWindow::updateTrackingControlAvailability()
{
    const bool cameraReady =
        m_cameraManager.state() == CameraManager::CameraState::Open;

    bool autoMode = false;
    {
        QMutexLocker locker(&m_modeMutex);
        autoMode = (m_currentMode == 1);
    }

    const bool serialReady = m_serialController.isOpen();
    const bool modelReady = isFeatureTrackingSelected() ||
                            (m_trackingEngine.yoloReady() &&
                             m_trackingEngine.yoloWarmedUp());

    // 追踪属于自动模式功能；停止按钮保留给当前追踪状态，便于串口异常时先停止本地线程。
    const bool canSelect = cameraReady && serialReady && autoMode &&
                           !m_isCapturing && !m_isSelecting;
    const bool canStart = cameraReady && serialReady && autoMode &&
                          m_hasSelectedTarget && !m_isCapturing && modelReady;
    const bool canStop = m_isCapturing;

    ui->btnSelectTarget->setEnabled(canSelect);
    ui->btnStartTracking->setEnabled(canStart);
    ui->btnStopTracking->setEnabled(canStop);

    if (!cameraReady) {
        ui->btnSelectTarget->setToolTip(QStringLiteral("请先打开摄像头"));
        ui->btnStartTracking->setToolTip(QStringLiteral("请先打开摄像头"));
    } else if (!serialReady) {
        ui->btnSelectTarget->setToolTip(QStringLiteral("请先连接串口"));
        ui->btnStartTracking->setToolTip(QStringLiteral("请先连接串口"));
    } else if (!autoMode) {
        ui->btnSelectTarget->setToolTip(QStringLiteral("自动模式下才能进行目标追踪"));
        ui->btnStartTracking->setToolTip(QStringLiteral("自动模式下才能开始追踪"));
    } else if (!m_hasSelectedTarget) {
        ui->btnStartTracking->setToolTip(QStringLiteral("请先选择目标"));
    } else if (!modelReady) {
        ui->btnStartTracking->setToolTip(QStringLiteral("追踪模型尚未准备好"));
    } else {
        ui->btnSelectTarget->setToolTip(QStringLiteral("在画面中框选目标"));
        ui->btnStartTracking->setToolTip(QStringLiteral("开始自动追踪"));
    }
    ui->btnStopTracking->setToolTip(canStop ? QStringLiteral("停止当前追踪")
                                            : QStringLiteral("当前没有正在进行的追踪"));
}

void MainWindow::sendManualStep(uint8_t cmd, const QString &direction)
{
    bool manual = false;
    {
        QMutexLocker locker(&m_modeMutex);
        manual = (m_currentMode == 0);
    }

    if (!m_serialController.isOpen()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先连接串口！"));
        return;
    }
    if (!manual || ui->label_11->text() != QStringLiteral("手动")) {
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【手动控制】当前不是手动模式，忽略%1操作").arg(direction));
        updateManualControlAvailability();
        return;
    }

    // 下位机收到一个命令只改变一次目标角度，固定为 0.5°。
    sendCommand(cmd);
    ui->plainTextEdit_2->appendPlainText(
        QStringLiteral("【手动控制】%1 0.5°").arg(direction));
}

void MainWindow::on_pushButton_4_clicked()
{
    sendManualStep(SerialController::CmdManualUp, QStringLiteral("向上"));
}

void MainWindow::on_pushButton_5_clicked()
{
    sendManualStep(SerialController::CmdManualLeft, QStringLiteral("向左"));
}

void MainWindow::on_pushButton_6_clicked()
{
    sendManualStep(SerialController::CmdManualRight, QStringLiteral("向右"));
}

void MainWindow::on_pushButton_7_clicked()
{
    sendManualStep(SerialController::CmdManualDown, QStringLiteral("向下"));
}

void MainWindow::sendCommand(uint8_t cmd)
{
    m_serialController.sendCommand(cmd);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    stopTrackingSafely();
    if (m_serialController.isOpen()) {
        // 发送停止跟踪+回中并等待字节写出；即使丢包，下位机链路超时保护也会兜底回中
        m_serialController.shutdownGimbal();
        m_serialController.closeSerial();
    }
    m_cameraManager.closeCamera();
    event->accept();
}
