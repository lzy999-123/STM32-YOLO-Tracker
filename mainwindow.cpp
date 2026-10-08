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
#include <QUrl>
#include <QStatusBar>
#include <QLabel>
#include <QMenu>
#include <QResizeEvent>
#include <QTimer>

#include <algorithm>
#include <utility>

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

MainWindow::MainWindow(QWidget *parent, bool autoConnect)
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
    , m_lastControlSendTime(0)
    , m_dnnFrameSkipCounter(0)
    , m_lostFrameCount(0)
{
    ui->setupUi(this);
    cv::setNumThreads(4);
    setWindowTitle(QStringLiteral("Luckfox 无线云台跟踪系统"));

    ui->imageLabel->setAlignment(Qt::AlignCenter);
    ui->imageLabel->setScaledContents(false);
    ui->imageLabel->setStyleSheet(QStringLiteral("background-color: black; border: none;"));
    setMinimumSize(1100, 700);
    ui->plainTextEdit_2->setMaximumBlockCount(500);
    ui->plainTextEdit->setPlainText(QStringLiteral("X：—\nY：—"));
    ui->label_11->setText(QStringLiteral("离线"));
    QPixmap initPlaceholder(ui->imageLabel->size());
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
    ui->label_11->setText(QStringLiteral("离线"));
    ui->label_11->setStyleSheet(QStringLiteral("color: black; font-size: 14px; font-weight: bold;"));

    setupNetworkUi();
    testDNN();

    // CSRT 在后台线程运行，targetTracked/targetLost 以排队方式送达；停止或重新开始后，
    // 旧会话残留的信号仍可能到达。按会话代号丢弃旧信号，避免停止后继续下发旧偏移。
    connect(&m_trackingEngine, &TrackingEngine::targetTracked, this,
            [this](const cv::Rect2d &rect, int offsetX, int offsetY,
                   quint64 generation, quint64 resultSeq) {
        if (generation != m_trackingEngine.trackingGeneration() || !m_isCapturing) return;
        m_trackedRect = rect;
        m_isTargetTracked = true;
        m_offsetX = static_cast<int16_t>(offsetX);
        m_offsetY = static_cast<int16_t>(offsetY);
        if (resultSeq != m_lastTrackResultSeq) {
            m_lastTrackResultSeq = resultSeq;
            m_trackResultPending = true;
        }
        flushTrackData();
    });
    connect(&m_trackingEngine, &TrackingEngine::targetLost, this, [this](quint64 generation) {
        if (generation != m_trackingEngine.trackingGeneration()) return;
        const bool wasTracked = m_isTargetTracked;
        m_isTargetTracked = false;
        m_trackedRect = cv::Rect2d();
        m_offsetX = 0;
        m_offsetY = 0;
        // 刚丢失时补发一次零偏移，让下位机立即停止，而不是等待超时。
        if (wasTracked && m_isCapturing) {
            m_trackResultPending = true;
            flushTrackData();
        }
    });
    connect(&m_trackingEngine, &TrackingEngine::logMessage, this, [this](const QString &msg) {
        if (ui->plainTextEdit_2) {
            ui->plainTextEdit_2->appendPlainText(msg);
        }
        updateTrackingControlAvailability();
    });

    connect(&m_networkController, &NetworkController::connectionLost, this,
            [this](const QString &reason) {
        m_modeSwitchPending = false;
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【无线控制】%1").arg(reason));
        stopLocalControl();
        updateGimbalStatus();
    });
    connect(&m_networkController, &NetworkController::reconnected, this, [this] {
        qInfo() << "[Device] control_connected";
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【无线控制】Luckfox 转发服务已连接，等待 STM32 遥测"));
        updateGimbalStatus();
    });
    connect(&m_networkController, &NetworkController::deviceOnlineChanged, this, [this](bool online) {
        qInfo() << "[Device] stm32_online=" << online;
        if (!online) stopLocalControl();
        m_lastRemoteMode = -1;
        updateGimbalStatus();
    });
    connect(&m_networkController, &NetworkController::commandFailed, this, [this](uint8_t cmd) {
        m_modeSwitchPending = false;
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【无线控制】命令 0x%1 未得到 STM32 确认")
            .arg(cmd, 2, 16, QLatin1Char('0')));
        stopLocalControl();
        updateGimbalStatus();
    });
    connect(&m_networkController, &NetworkController::telemetryReceived, this,
            [this](int mode, float horizontal, float vertical) {
        const int flags = m_networkController.telemetryFlags();
        QString alarm;
        if (flags & NetworkController::FlagFault) alarm += QStringLiteral(" · 编码器故障");
        if (flags & NetworkController::FlagEmergencyStop) alarm += QStringLiteral(" · 急停");
        if (flags & NetworkController::FlagLinkLost) alarm += QStringLiteral(" · 链路丢失");
        m_gimbalStatus->setText(QStringLiteral("云台在线 · 水平 %1° / 垂直 %2°%3")
            .arg(horizontal, 0, 'f', 1).arg(vertical, 0, 'f', 1).arg(alarm));
        if (mode != m_lastRemoteMode) {
            m_modeSwitchPending = false;
            m_lastRemoteMode = mode;
            { QMutexLocker locker(&m_modeMutex); m_currentMode = mode; }
            stopLocalControl();
        }
        ui->label_11->setText(mode == 0 ? QStringLiteral("手动") : QStringLiteral("自动"));
        updateManualControlAvailability();
        updateTrackingControlAvailability();
    });
    connect(&m_networkController, &NetworkController::telemetryFlagsChanged, this, [this](int flags) {
        qInfo() << "[Device] telemetry_flags=" << flags;
        if (flags & (NetworkController::FlagFault | NetworkController::FlagEmergencyStop)) {
            ui->plainTextEdit_2->appendPlainText(QStringLiteral("【云台】%1，已停止本地跟踪")
                .arg(flags & NetworkController::FlagFault ? QStringLiteral("下位机报告编码器/系统故障")
                                                          : QStringLiteral("下位机处于急停状态")));
            stopLocalControl();
        } else if (flags & NetworkController::FlagLinkLost) {
            ui->plainTextEdit_2->appendPlainText(QStringLiteral("【云台】下位机报告链路丢失"));
        }
        updateGimbalStatus();
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
        m_deviceUrl = url;
        finishDeviceDiscovery();
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【设备连接】已找到 Luckfox：%1").arg(QUrl(url).host()));
        openNetworkControl(url);
        const bool videoRequested = std::exchange(m_openVideoAfterDiscovery, false);
        const bool addressChanged = selectedCameraSource(ui->comboBox_3) == QStringLiteral("luckfox:auto") &&
            m_cameraManager.state() == CameraManager::CameraState::Open &&
            url != m_cameraManager.currentCameraId();
        if (videoRequested || addressChanged) m_cameraManager.openCamera(url);
        else if (m_cameraManager.state() == CameraManager::CameraState::Idle ||
                 m_cameraManager.state() == CameraManager::CameraState::Error)
            ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
        ui->statusbar->showMessage(QStringLiteral("设备地址已刷新，控制通道正在确认连接"), 4000);
    });
    connect(&m_luckfoxDiscovery, &LuckfoxDiscovery::failed, this, [this](const QString &message) {
        finishDeviceDiscovery();
        if (std::exchange(m_openVideoAfterDiscovery, false)) {
            ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
            ui->pushButton_9->setEnabled(true);
            ui->lineEdit->setText(QStringLiteral("未找到"));
            ui->LED1->setStyleSheet(QStringLiteral("background-color: red"));
        }
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【设备连接】%1").arg(message));
        ui->statusbar->showMessage(QStringLiteral("未找到设备，请检查同一 Wi-Fi；程序会自动重试"), 6000);
    });
    connect(&m_luckfoxDiscovery, &LuckfoxDiscovery::lockedDeviceMissing, this,
            [this](const QString &otherId) {
        // 延后到事件循环处理，避免在设备发现的定时器回调里打开模态对话框并重入 start()。
        QTimer::singleShot(0, this, [this, otherId] {
            const auto answer = QMessageBox::question(this, QStringLiteral("设备发现"),
                QStringLiteral("已记住的 Luckfox（%1）多次未响应，但发现了另一台设备 %2。\n"
                               "是否忘记旧设备并连接新设备？")
                    .arg(m_luckfoxDiscovery.lockedDeviceId(), otherId));
            if (answer != QMessageBox::Yes) return;
            m_luckfoxDiscovery.forgetDevice();
            ui->plainTextEdit_2->appendPlainText(QStringLiteral("【设备连接】已忘记旧设备，重新搜索"));
            m_luckfoxDiscovery.start();
        });
    });

    connect(ui->comboBox_3, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onCameraChanged);
    connect(&m_cameraManager, &CameraManager::frameReady,
            this, &MainWindow::handleNewVideoFrame);
    connect(&m_cameraManager, &CameraManager::matReady,
            this, &MainWindow::handleNewMatFrame);
    connect(&m_cameraManager, &CameraManager::stateChanged, this,
            [this](CameraManager::CameraState state) {
        QString status;
        QString color = QStringLiteral("orange");
        if (state == CameraManager::CameraState::Idle) { status = QStringLiteral("未连接"); color = QStringLiteral("red"); }
        else if (state == CameraManager::CameraState::Open) { status = QStringLiteral("已连接"); color = QStringLiteral("green"); }
        else if (state == CameraManager::CameraState::Opening) status = QStringLiteral("连接中");
        else if (state == CameraManager::CameraState::Closing) status = QStringLiteral("关闭中");
        else { status = QStringLiteral("连接失败"); color = QStringLiteral("red"); }
        ui->lineEdit->setText(status);
        ui->LED1->setStyleSheet(QStringLiteral("background-color: %1").arg(color));
        if (state == CameraManager::CameraState::Idle || state == CameraManager::CameraState::Closing || state == CameraManager::CameraState::Error) {
            stopLocalControl();
            if (m_networkController.isOpen()) m_networkController.sendCommand(NetworkController::CmdTrackOff);
        }
        if (state == CameraManager::CameraState::Idle) {
            ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
            ui->pushButton_9->setEnabled(true);
        } else if (state == CameraManager::CameraState::Open) {
            openNetworkControl(m_cameraManager.currentCameraId());
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
    for (QWidget *widget : ui->centralwidget->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        m_baseGeometries.append(qMakePair(widget, widget->geometry()));
    m_cameraManager.startChecking();
    updateTrackingControlAvailability();
    m_deviceReconnectTimer.setInterval(5000);
    connect(&m_deviceReconnectTimer, &QTimer::timeout, this, [this] {
        if (!m_networkController.isConnected() && !m_luckfoxDiscovery.isActive() &&
            selectedCameraSource(ui->comboBox_3) == QStringLiteral("luckfox:auto"))
            startDeviceDiscovery();
    });
    if (autoConnect) {
        m_deviceReconnectTimer.start();
        QTimer::singleShot(0, this, &MainWindow::startDeviceDiscovery);
    }
}

MainWindow::~MainWindow()
{
    // 先断开各工作对象到本窗口的连接：停止过程以及成员析构时发出的信号
    // 不能再进入访问 ui 的槽函数。
    disconnect(&m_trackingEngine, nullptr, this, nullptr);
    disconnect(&m_cameraManager, nullptr, this, nullptr);
    disconnect(&m_networkController, nullptr, this, nullptr);
    disconnect(&m_luckfoxDiscovery, nullptr, this, nullptr);
    disconnect(&m_hostWifi, nullptr, this, nullptr);
    m_deviceReconnectTimer.stop();
    m_luckfoxDiscovery.cancel();
    m_isCapturing = false;
    stopTrackingSafely();
    m_cameraManager.closeCamera();
    m_networkController.disconnectDevice();
    delete ui;
    ui = nullptr;
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
    // 所有停止路径都先清除本地跟踪结果，再等待后台帧结束并停止引擎；
    // 引擎会递增会话代号，之后到达的旧 targetTracked 会被丢弃。
    m_isTargetTracked = false;
    m_trackedRect = cv::Rect2d();
    m_offsetX = 0;
    m_offsetY = 0;
    m_trackResultPending = false;
    waitForTrackingWorker();
    m_trackingEngine.stopTracking();
}

QPoint MainWindow::controlOffsets() const
{
    // 偏移是在经过方向变换后的画面上计算的。约定：
    // - 旋转 180°/90°/270° 视为“安装方向校正”：校正后的画面与云台真实方向一致，
    //   偏移保持在校正后的画面坐标系中，不再取反（下位机按正装相机标定方向）。
    // - 水平镜像 / 垂直翻转只是显示偏好，真实场景并未镜像：下发前把对应轴取反，
    //   保证云台仍朝真实目标方向转动。
    int x = m_offsetX;
    int y = m_offsetY;
    const CameraManager::RotationMode mode = m_cameraManager.rotationMode();
    if (mode == CameraManager::RotationMode::MirrorH) x = -x;
    if (mode == CameraManager::RotationMode::MirrorV) y = -y;
    return QPoint(x, y);
}

void MainWindow::flushTrackData()
{
    if (!m_trackResultPending || !m_isCapturing || !m_networkController.isOpen()) return;
    {
        QMutexLocker modeLocker(&m_modeMutex);
        if (m_currentMode != 1) return;
    }
    // 只在有新结果时发送，且保留 20ms 最小间隔；被限流的结果在下一帧补发最新值。
    const qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    if (currentTime - m_lastControlSendTime < 20) return;
    const QPoint offsets = controlOffsets();
    m_networkController.sendTrackData(static_cast<int16_t>(std::clamp(offsets.x(), -32768, 32767)),
                                      static_cast<int16_t>(std::clamp(offsets.y(), -32768, 32767)));
    m_lastControlSendTime = currentTime;
    m_trackResultPending = false;
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
        sendCommand(NetworkController::CmdTrackOff);
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

void MainWindow::on_pushButton_8_clicked()
{
    qInfo() << "[Device] refresh_requested";
    m_hostWifi.refresh();
    ui->plainTextEdit_2->appendPlainText(QStringLiteral("【刷新】正在刷新 Wi-Fi 和设备连接状态..."));
    ui->statusbar->showMessage(QStringLiteral("正在刷新设备连接..."));
    if (selectedCameraSource(ui->comboBox_3) == QStringLiteral("luckfox:auto")) {
        startDeviceDiscovery();
    } else {
        const QString source = selectedCameraSource(ui->comboBox_3);
        if (source.startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive) ||
            QHostAddress(source).protocol() == QAbstractSocket::IPv4Protocol)
            openNetworkControl(normalizedRtspSource(source));
        ui->statusbar->showMessage(QStringLiteral("已刷新 Wi-Fi 和所选设备的控制连接"), 4000);
    }
}

void MainWindow::startDeviceDiscovery()
{
    ui->pushButton_8->setText(QStringLiteral("刷新中..."));
    ui->pushButton_8->setEnabled(false);
    ui->plainTextEdit_2->appendPlainText(QStringLiteral("【设备连接】正在查找 Luckfox..."));
    if (!m_networkController.isConnected()) m_gimbalStatus->setText(QStringLiteral("云台：正在查找 Luckfox"));
    m_luckfoxDiscovery.start();
}

void MainWindow::finishDeviceDiscovery()
{
    ui->pushButton_8->setText(QStringLiteral("刷新"));
    ui->pushButton_8->setEnabled(true);
    ui->comboBox_3->setEnabled(true);
}

void MainWindow::setupNetworkUi()
{
    ui->label_3->setText(QStringLiteral("读取中…"));
    ui->label_9->setText(QStringLiteral("未发现"));
    ui->label_3->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ui->label_9->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ui->lineEdit->setToolTip(QStringLiteral("此处仅表示视频通道状态；云台状态见底部"));
    ui->LED1->setFocusPolicy(Qt::NoFocus);
    ui->LED1->setAttribute(Qt::WA_TransparentForMouseEvents);
    ui->pushButton_8->setToolTip(QStringLiteral("刷新电脑 Wi-Fi 信息并重新查找 Luckfox"));
    m_gimbalStatus = new QLabel(QStringLiteral("云台：控制通道未连接"), this);
    m_gimbalStatus->setObjectName(QStringLiteral("gimbalStatus"));
    ui->statusbar->addWidget(m_gimbalStatus, 1);
    auto *orientation = new QPushButton(QStringLiteral("画面方向"), this);
    auto *menu = new QMenu(orientation);
    const QStringList names{QStringLiteral("正常"), QStringLiteral("旋转 180°"),
        QStringLiteral("水平镜像"), QStringLiteral("垂直翻转"), QStringLiteral("旋转 90°"), QStringLiteral("旋转 270°")};
    for (int mode = 0; mode < names.size(); ++mode) {
        auto *action = menu->addAction(names[mode]);
        connect(action, &QAction::triggered, this, [this, mode] {
            stopLocalControl();
            if (m_networkController.isOpen()) m_networkController.sendCommand(NetworkController::CmdTrackOff);
            m_cameraManager.setRotationMode(static_cast<CameraManager::RotationMode>(mode));
        });
    }
    orientation->setMenu(menu);
    ui->statusbar->addPermanentWidget(orientation);
    connect(&m_hostWifi, &HostWifiMonitor::nameChanged, this, [this](const QString &name) {
        ui->label_3->setToolTip(name);
        ui->label_3->setText(ui->label_3->fontMetrics().elidedText(name, Qt::ElideRight, ui->label_3->width()));
    });
    m_hostWifi.refresh();
}

void MainWindow::openNetworkControl(const QString &source)
{
    const QUrl url(source);
    if (url.scheme().compare(QStringLiteral("rtsp"), Qt::CaseInsensitive) != 0) {
        return;
    }
    if (ui->label_9->text() != url.host()) {
        stopLocalControl();
        if (m_networkController.isOpen()) m_networkController.sendCommand(NetworkController::CmdTrackOff);
    }
    ui->label_9->setText(url.host());
    ui->label_9->setToolTip(url.host());
    m_networkController.connectToDevice(url.host());
}

void MainWindow::stopLocalControl()
{
    m_isCapturing = m_isSelecting = m_hasSelectedTarget = m_isTargetTracked = false;
    m_wasTrackingBeforeDisconn = false;
    m_selectedRect = m_trackedRect = cv::Rect2d();
    m_offsetX = m_offsetY = 0;
    stopTrackingSafely();
    updateManualControlAvailability();
    updateTrackingControlAvailability();
}

void MainWindow::updateGimbalStatus()
{
    if (!m_networkController.isOpen()) {
        ui->label_11->setText(QStringLiteral("离线"));
        m_gimbalStatus->setText(m_networkController.isConnected()
            ? QStringLiteral("Luckfox 控制已连接 · STM32 离线")
            : QStringLiteral("云台：控制通道未连接"));
    }
    updateManualControlAvailability();
    updateTrackingControlAvailability();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (m_baseGeometries.isEmpty()) return;
    const int dx = width() - 1100, dy = height() - 700;
    for (const auto &entry : m_baseGeometries) {
        QWidget *widget = entry.first;
        QRect rect = entry.second;
        if (widget == ui->imageLabel) {
            rect.setSize(QSize(16, 9).scaled(QSize(832 + dx, 468 + dy), Qt::KeepAspectRatio));
        } else if (rect.x() >= 860) {
            rect.translate(dx, 0);
            if (widget == ui->plainTextEdit_2) rect.setHeight(rect.height() + dy);
        }
        widget->setGeometry(rect);
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

        openNetworkControl(camId);
        m_cameraManager.openCamera(camId);
    }
}

void MainWindow::on_pushButton_9_clicked()
{
    if (m_openVideoAfterDiscovery) {
        m_luckfoxDiscovery.cancel();
        finishDeviceDiscovery();
        ui->pushButton_9->setText(QStringLiteral("打开摄像头"));
        m_openVideoAfterDiscovery = false;
        ui->pushButton_9->setText(m_cameraManager.state() == CameraManager::CameraState::Open
            ? QStringLiteral("关闭摄像头") : QStringLiteral("打开摄像头"));
        if (m_cameraManager.state() != CameraManager::CameraState::Open) {
            ui->lineEdit->setText(QStringLiteral("未连接"));
            ui->LED1->setStyleSheet(QStringLiteral("background-color: red"));
        }
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】已取消查找"));
        return;
    }
    if (m_cameraManager.state() == CameraManager::CameraState::Idle ||
        m_cameraManager.state() == CameraManager::CameraState::Error) {
        ui->pushButton_9->setText(QStringLiteral("正在打开..."));
        ui->pushButton_9->setEnabled(false);

        const QString source = selectedCameraSource(ui->comboBox_3);
        if (source == QStringLiteral("luckfox:auto")) {
            if (!m_deviceUrl.isEmpty() && m_networkController.isConnected()) {
                m_cameraManager.openCamera(m_deviceUrl);
                return;
            }
            ui->comboBox_3->setEnabled(false);
            ui->pushButton_9->setText(QStringLiteral("取消查找"));
            ui->pushButton_9->setEnabled(true);
            m_openVideoAfterDiscovery = true;
            ui->lineEdit->setText(QStringLiteral("查找中"));
            ui->LED1->setStyleSheet(QStringLiteral("background-color: orange"));
            ui->plainTextEdit_2->appendPlainText(QStringLiteral("【摄像头】正在自动查找 Luckfox..."));
            if (!m_luckfoxDiscovery.isActive()) startDeviceDiscovery();
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

        openNetworkControl(camId);
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

    // 720P 极速约束：无论来自本地 USB 还是网络流，严格保障画面上限为 720P，限制处理尺寸；实际延迟需独立测量
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

    flushTrackData();

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_lastInfoUpdateTime >= 100) {
        m_lastInfoUpdateTime = nowMs;
        if (m_isCapturing && !m_trackedRect.empty()) {
            // 显示实际下发的控制偏移（已处理镜像/翻转）。
            const QPoint control = controlOffsets();
            QString info;
            info += QStringLiteral("X 偏移：%1\n").arg(control.x());
            info += QStringLiteral("Y 偏移：%1\n").arg(control.y());
            info += (control.x() > 0) ? QStringLiteral("舵机X → 向右转\n")
                                      : (control.x() < 0) ? QStringLiteral("舵机X → 向左转\n")
                                                          : QStringLiteral("舵机X → 居中\n");
            info += (control.y() > 0) ? QStringLiteral("舵机Y → 向下转\n")
                                      : (control.y() < 0) ? QStringLiteral("舵机Y → 向上转\n")
                                                          : QStringLiteral("舵机Y → 居中\n");
            ui->plainTextEdit->setPlainText(info);
        } else if (!m_waitingForRecover) {
            ui->plainTextEdit->setPlainText(QStringLiteral("X：—\nY：—"));
        }
    }
}

void MainWindow::on_btnSelectTarget_clicked()
{
    if (!m_networkController.isOpen()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先连接 Luckfox 并等待 STM32 云台上线！"));
        return;
    }
    bool autoMode = false;
    {
        QMutexLocker locker(&m_modeMutex);
        autoMode = (m_currentMode == 1);
    }
    // warning() 会开启嵌套事件循环，期间遥测/视频回调也会读取模式。
    // 必须先释放非递归模式锁，否则同一个 GUI 线程会再次等待自己的锁。
    if (!autoMode) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先切换到自动模式！"));
        return;
    }
    if (m_cameraManager.state() != CameraManager::CameraState::Open) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先打开摄像头！"));
        return;
    }

    m_isCapturing = false;
    stopTrackingSafely();
    m_hasSelectedTarget = false;
    m_trackedRect = cv::Rect2d();
    m_isTargetTracked = false;
    m_offsetX = 0;
    m_offsetY = 0;
    m_isSelecting = true;
    m_selectDragActive = false;
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
    if (!m_networkController.isOpen()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先连接 Luckfox 并等待 STM32 云台上线！"));
        return;
    }
    bool autoMode = false;
    {
        QMutexLocker locker(&m_modeMutex);
        autoMode = (m_currentMode == 1);
    }
    if (!autoMode) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先切换到自动模式！"));
        return;
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
    if (!useFeatureTracking && m_trackingEngine.yoloLoading()) {
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【提示】YOLO 模型正在后台加载，请稍候再开始跟踪。"));
        return;
    }
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
    m_isTargetTracked = false;
    m_trackedRect = cv::Rect2d();
    m_offsetX = 0;
    m_offsetY = 0;
    m_trackResultPending = false;
    waitForTrackingWorker();
    m_trackingEngine.startTracking(
        currentFrameClone,
        m_selectedRect,
        useFeatureTracking,
        useFeatureTracking ? QStringLiteral("feature") : currentTrackingModelFileName());
    updateTrackingControlAvailability();

    if (ui->label_11->text() == QStringLiteral("自动")) {
        sendCommand(NetworkController::CmdTrackOn);
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
        sendCommand(NetworkController::CmdTrackOff);
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
    m_selectDragActive = false;
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
    // 按在黑边（letterbox）区域时夹到图像边缘，与移动/松开的处理一致，
    // 避免沿用上一次框选遗留的起点。
    localPos.setX(std::clamp(localPos.x(), displayedImageRect.left(), displayedImageRect.right()));
    localPos.setY(std::clamp(localPos.y(), displayedImageRect.top(), displayedImageRect.bottom()));

    const double scaleX = static_cast<double>(imageSize.width()) / scaledImageSize.width();
    const double scaleY = static_cast<double>(imageSize.height()) / scaledImageSize.height();
    m_selectStartImg = QPoint(
        static_cast<int>((localPos.x() - xOffset) * scaleX),
        static_cast<int>((localPos.y() - yOffset) * scaleY));
    m_selectEndImg = m_selectStartImg;
    m_selectDragActive = true;
}

void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_isSelecting) {
        QMainWindow::mouseMoveEvent(event);
        return;
    }

    QPoint localPos = ui->imageLabel->mapFromGlobal(event->globalPosition().toPoint());
    if (!m_selectDragActive || !ui->imageLabel->rect().contains(localPos)) return;
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
    if (!m_selectDragActive || !ui->imageLabel->rect().contains(localPos) ||
        event->button() != Qt::LeftButton) {
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
    m_selectDragActive = false;

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
    if (!m_networkController.isOpen()) return;
    stopLocalControl();
    sendCommand(NetworkController::CmdCenter);
}

void MainWindow::on_pushButton_3_clicked()
{
    if (!m_networkController.isOpen()) return;
    m_modeSwitchPending = true;
    stopLocalControl();
    sendCommand(NetworkController::CmdSwitchMode);
    // 实际模式只由 STM32 遥测更新，不能先在 UI 中假定切换成功。
    ui->pushButton_3->setEnabled(false);
    QTimer::singleShot(500, this, [this] {
        m_modeSwitchPending = false;
        updateManualControlAvailability();
        updateTrackingControlAvailability();
    });
}

void MainWindow::updateManualControlAvailability()
{
    ui->pushButton_2->setEnabled(m_networkController.isOpen());
    ui->pushButton_3->setEnabled(m_networkController.isOpen() && !m_modeSwitchPending);
    bool manual = false;
    {
        QMutexLocker locker(&m_modeMutex);
        manual = (m_currentMode == 0);
    }

    // 遥测离线时 label_11 会显示“离线”，此时不允许误发手动步进命令。
    const bool enabled = m_networkController.isOpen() && !m_modeSwitchPending && manual &&
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

    const bool controlReady = m_networkController.isOpen() && !m_modeSwitchPending;
    const bool modelReady = isFeatureTrackingSelected() ||
                            (m_trackingEngine.yoloReady() &&
                             m_trackingEngine.yoloWarmedUp());

    // 追踪属于自动模式功能；停止按钮保留给当前追踪状态，便于网络控制异常时先停止本地线程。
    const bool canSelect = cameraReady && controlReady && autoMode &&
                           !m_isCapturing && !m_isSelecting;
    const bool canStart = cameraReady && controlReady && autoMode &&
                          m_hasSelectedTarget && !m_isCapturing && modelReady;
    const bool canStop = m_isCapturing;

    ui->btnSelectTarget->setEnabled(canSelect);
    ui->btnStartTracking->setEnabled(canStart);
    ui->btnStopTracking->setEnabled(canStop);

    if (!cameraReady) {
        ui->btnSelectTarget->setToolTip(QStringLiteral("请先打开摄像头"));
        ui->btnStartTracking->setToolTip(QStringLiteral("请先打开摄像头"));
    } else if (!controlReady) {
        ui->btnSelectTarget->setToolTip(QStringLiteral("请先连接 Luckfox 并等待云台上线"));
        ui->btnStartTracking->setToolTip(QStringLiteral("请先连接 Luckfox 并等待云台上线"));
    } else if (!autoMode) {
        ui->btnSelectTarget->setToolTip(QStringLiteral("自动模式下才能进行目标追踪"));
        ui->btnStartTracking->setToolTip(QStringLiteral("自动模式下才能开始追踪"));
    } else if (!m_hasSelectedTarget) {
        ui->btnStartTracking->setToolTip(QStringLiteral("请先选择目标"));
    } else if (!modelReady) {
        ui->btnStartTracking->setToolTip(m_trackingEngine.yoloLoading()
                                             ? QStringLiteral("加载模型中，请稍候")
                                             : QStringLiteral("追踪模型尚未准备好"));
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

    if (!m_networkController.isOpen()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先连接 Luckfox 并等待 STM32 云台上线！"));
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
    sendManualStep(NetworkController::CmdManualUp, QStringLiteral("向上"));
}

void MainWindow::on_pushButton_5_clicked()
{
    sendManualStep(NetworkController::CmdManualLeft, QStringLiteral("向左"));
}

void MainWindow::on_pushButton_6_clicked()
{
    sendManualStep(NetworkController::CmdManualRight, QStringLiteral("向右"));
}

void MainWindow::on_pushButton_7_clicked()
{
    sendManualStep(NetworkController::CmdManualDown, QStringLiteral("向下"));
}

void MainWindow::sendCommand(uint8_t cmd)
{
    m_networkController.sendCommand(cmd);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_deviceReconnectTimer.stop();
    m_luckfoxDiscovery.cancel();
    stopLocalControl();
    m_networkController.shutdownGimbal();
    m_cameraManager.closeCamera();
    event->accept();
}
