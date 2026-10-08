#include "cameramanager.h"
#include <QMediaDevices>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QDateTime>
#include <QDebug>
#include <QMutexLocker>
#include <QCoreApplication>
#include <opencv2/opencv.hpp>
#include <utility>
#include <chrono>

namespace {
constexpr int kRealtimeMaxWidth = 1280;
constexpr int kRealtimeMaxHeight = 720;
}

CameraManager::CameraManager(QObject *parent)
    : QObject(parent),
      m_camera(nullptr),
      m_captureSession(new QMediaCaptureSession(this)),
      m_videoSink(new QVideoSink(this)),
      m_cameraState(CameraState::Idle),
      m_isRtsp(false),
      m_rtspRunning(false),
      m_lastFrameTime(0),
      m_retryCount(0)
{
    qRegisterMetaType<QVideoFrame>("QVideoFrame");
    qRegisterMetaType<cv::Mat>("cv::Mat");

    // 配置捕获会话的输出流向为 QVideoSink
    m_captureSession->setVideoSink(m_videoSink);
    // 监听视频帧改变信号，当有新帧时触发 handleNewVideoFrame
    connect(m_videoSink, &QVideoSink::videoFrameChanged, this, &CameraManager::handleNewVideoFrame);

    // 初始化相机防卡死巡检定时器，每秒检查一次
    m_cameraCheckTimer = new QTimer(this);
    m_cameraCheckTimer->setInterval(1000);
    connect(m_cameraCheckTimer, &QTimer::timeout, this, &CameraManager::checkCameraStatus);
}

CameraManager::~CameraManager()
{
    m_cameraCheckTimer->stop();
    safeDeleteCamera(); // 确保安全释放摄像头资源
}

void CameraManager::startChecking()
{
    m_cameraCheckTimer->start(); // 启动定时巡检
}

void CameraManager::safeDeleteCamera()
{
    // 先停止 RTSP 流（若正在运行）
    stopRtspStream();

    QCamera *camera = nullptr;
    {
        QMutexLocker locker(&m_cameraMutex);
        camera = std::exchange(m_camera, nullptr);
    }
    if (camera) {
        // 先摘除成员指针和信号，再停止设备，避免停止过程中重入清理逻辑。
        disconnect(camera, nullptr, this, nullptr);
        m_captureSession->setCamera(nullptr);
        camera->stop();
        camera->deleteLater();
    }
}

void CameraManager::startRtspStream(const QString &url)
{
    stopRtspStream(); // 确保彻底作废任何前序会话

    m_isRtsp = true;
    m_rtspRunning = true;
    m_rtspFormatErrorReported.store(false, std::memory_order_release);
    const quint64 newSession = ++m_rtspSessionId;
    m_lastFrameTime = QDateTime::currentMSecsSinceEpoch();
    m_rtspThread = std::make_unique<std::thread>(&CameraManager::rtspWorkerLoop, this, url, newSession);
}

void CameraManager::stopRtspStream()
{
    m_rtspSessionId.fetch_add(1); // 递增会话ID，旧线程在当前帧或下次循环检查时立即退出
    m_rtspRunning.store(false);

    {
        std::lock_guard<std::mutex> lock(m_rtspMatMutex);
        m_latestRtspMat.release();
        m_hasNewRtspMat.store(false, std::memory_order_release);
    }
    m_rtspDispatchPending.store(false, std::memory_order_release);

    if (m_rtspThread) {
        if (m_rtspThread->joinable()) {
            // 在独立后台线程中安全等待 worker 退出并释放资源，绝不阻塞 Qt GUI 主线程！
            std::thread([t = std::move(m_rtspThread)]() mutable {
                if (t && t->joinable()) {
                    t->join();
                }
            }).detach();
        } else {
            m_rtspThread.reset();
        }
    }
    m_isRtsp = false;
}

void CameraManager::notifyNewMat(quint64 sessionId)
{
    m_rtspDispatchPending.store(false, std::memory_order_release);
    if (sessionId != m_rtspSessionId.load()) {
        return; // 已经切换到新视频流，废弃旧流残留通知！
    }

    cv::Mat mat;
    {
        std::lock_guard<std::mutex> lock(m_rtspMatMutex);
        if (!m_hasNewRtspMat.load(std::memory_order_acquire)) return;
        mat = m_latestRtspMat;
        m_hasNewRtspMat.store(false, std::memory_order_release);
    }
    if (!mat.empty() && sessionId == m_rtspSessionId.load()) {
        emit matReady(mat);
    }
}

void CameraManager::rtspWorkerLoop(const QString &url, quint64 sessionId)
{
    if (sessionId != m_rtspSessionId.load() || !m_rtspRunning.load()) return;

    // 画面只要求 720P，全链路极致压榨延迟（目标 0 延迟）
    // 1. 底层解码参数深度调优：
    //    - fflags;nobuffer + flush_packets;1: 解复用器绝对零缓冲，收到包立即交付解码器
    //    - flags;low_delay: 激活低延迟解码，不等待 B 帧参考
    //    - max_delay;0: 强制最大解复用延时为 0 微秒（默认是 500000 即 0.5s！）
    //    - reorder_queue_size;0: 禁用 RTP 乱序重排缓冲区
    //    - probesize;32 + analyzeduration;0: 极速分析无探针耗时
    //    - sync;ext: 外部时钟无同步停顿
    //    - threads;1: 单线程解码杜绝多线程解码管道自带的 N 帧延迟排队！
    // 画面要求 720P，全链路极致压榨延迟（目标毫秒级实时）
    // 1. 底层解码参数深度调优：
    //    - rtsp_transport;tcp: 优先稳定 TCP 传输，杜绝 UDP 超时回退卡顿与花屏
    //    - buffer_size;102400: 严控底层 socket 接收缓存大小，杜绝操作系统无序堆积历史帧
    //    - fflags;nobuffer + flush_packets;1: 解复用器绝对零缓冲，收到包立即交付解码器
    //    - flags;low_delay: 激活低延迟解码，不等待 B 帧参考
    //    - max_delay;0: 强制最大解复用延时为 0 微秒
    //    - reorder_queue_size;0: 禁用 RTP 乱序重排缓冲区
    //    - probesize;32 + analyzeduration;0: 极速分析无探针耗时
    //    - sync;ext: 外部时钟无同步停顿
    //    - threads;1: 单线程解码杜绝多线程解码管道自带的 N 帧延迟排队！
    static const std::string s_tcpOptions =
        "rtsp_transport;tcp|"
        "buffer_size;102400|"
        "avioflags;direct|"
        "fflags;nobuffer|"
        "flags;low_delay|"
        "max_delay;0|"
        "reorder_queue_size;0|"
        "probesize;32|"
        "analyzeduration;0|"
        "sync;ext|"
        "flush_packets;1|"
        "threads;1|"
        "timeout;2000000";

    static const std::string s_udpOptions =
        "rtsp_transport;udp|"
        "buffer_size;102400|"
        "fifo_size;200000|"
        "avioflags;direct|"
        "fflags;nobuffer|"
        "flags;low_delay|"
        "max_delay;0|"
        "reorder_queue_size;0|"
        "probesize;32|"
        "analyzeduration;0|"
        "sync;ext|"
        "flush_packets;1|"
        "threads;1|"
        "timeout;2000000";

    auto setFfmpegOptions = [](const std::string &options) {
        qputenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", QByteArray::fromStdString(options));
        _putenv_s("OPENCV_FFMPEG_CAPTURE_OPTIONS", options.c_str());
        static std::string environmentValue;
        environmentValue = "OPENCV_FFMPEG_CAPTURE_OPTIONS=" + options;
        _putenv(environmentValue.c_str());
    };

    // 针对 Luckfox RTSP 架构，优先采用 TCP interleaved 避免握手回退损耗
    setFfmpegOptions(s_tcpOptions);

    qDebug() << "[CameraManager] 启用 720P 零延迟极速取流方案 (优先TCP):" << url;

    cv::VideoCapture cap;
    cap.open(url.toStdString(), cv::CAP_FFMPEG);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    if (!cap.isOpened() && m_rtspRunning.load() && sessionId == m_rtspSessionId.load()) {
        qWarning() << "[CameraManager] TCP RTSP 打开失败，回退 UDP:" << url;
        cap.release();
        setFfmpegOptions(s_udpOptions);
        cap.open(url.toStdString(), cv::CAP_FFMPEG);
        cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    }

    if (sessionId != m_rtspSessionId.load() || !m_rtspRunning.load()) {
        cap.release();
        return;
    }

    if (!cap.isOpened()) {
        if (sessionId != m_rtspSessionId.load()) return;
        qWarning() << "Failed to open RTSP stream:" << url;
        QMetaObject::invokeMethod(this, [this, sessionId]() {
            if (sessionId != m_rtspSessionId.load()) return;
            m_cameraState = CameraState::Error;
            emit stateChanged(m_cameraState);
            emit cameraError(QStringLiteral("无法连接 RTSP 视频流，请检查设备 IP 与网络状态"));
        }, Qt::QueuedConnection);
        m_rtspRunning.store(false);
        return;
    }

    // RTSP 能够成功打开即表示无线图像通道已经建立。
    // 缓存冲刷和首帧格式校验属于后续媒体处理，不应延迟连接状态上报。
    m_lastFrameTime.store(QDateTime::currentMSecsSinceEpoch());
    QMetaObject::invokeMethod(this, [this, sessionId]() {
        if (sessionId != m_rtspSessionId.load() || !m_rtspRunning.load()) return;
        m_cameraState = CameraState::Open;
        m_retryCount = 0;
        emit stateChanged(m_cameraState);
    }, Qt::QueuedConnection);

    // 关键核心：冲刷排空建联期间在 TCP 协议栈中积压的 100~150 帧历史陈旧帧（消除建联产生的巨大初始延迟）
    // 算法原理：
    // 首帧存在约 50ms 解码器初始化耗时；
    // 积压的旧帧在内存中读取极快（< 10ms）；
    // 连续排空超过 10 帧后，当单次 grab 耗时 > 20ms 时，说明底层缓存已彻底排空，正在等待摄像头最新拍摄的实时帧！
    int drained = 0;
    const auto tDrainStart = std::chrono::steady_clock::now();
    while (m_rtspRunning.load() && sessionId == m_rtspSessionId.load() && drained < 500) {
        const auto tGrabStart = std::chrono::steady_clock::now();
        if (!cap.grab()) break;
        const auto tElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tGrabStart).count();
        drained++;
        if (drained > 10 && tElapsedMs > 20) {
            qDebug() << "[CameraManager] 成功冲刷排空" << drained << "帧历史陈旧积压缓存，直达 0 延迟实时画面，耗时:"
                     << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tDrainStart).count() << "ms";
            break;
        }
    }

    if (sessionId != m_rtspSessionId.load() || !m_rtspRunning.load()) {
        cap.release();
        return;
    }

    // 立即刷新时间戳，防止开流耗时被误判为超时断流
    m_lastFrameTime.store(QDateTime::currentMSecsSinceEpoch());

    cv::Mat frame;
    while (m_rtspRunning.load() && sessionId == m_rtspSessionId.load()) {
        if (!cap.grab()) {
            if (!m_rtspRunning.load() || sessionId != m_rtspSessionId.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // 持续实时追赶机制：
        // 正常 30 FPS 下每帧间隔约 33ms。若缓冲区中积压了旧帧，连续 grab 耗时通常极短（< 10ms）。
        // 只要耗时极短且能抓取成功，说明读的是内存历史缓存，我们快速循环 grab 丢弃，
        // 直到某一次 grab 产生真实网络等待（> 15ms）或已跳过 8 帧，确保 retrieve 的必定是摄像机传感器刚刚产生的最新物理帧！
        int skipped = 0;
        while (skipped < 8 && m_rtspRunning.load() && sessionId == m_rtspSessionId.load()) {
            const auto tCheckStart = std::chrono::steady_clock::now();
            if (!cap.grab()) break;
            const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tCheckStart).count();
            skipped++;
            if (dtMs > 15) {
                // 等待了网络数据，说明已经抓到了当前时刻最新产生的实时帧！
                break;
            }
        }

        if (!cap.retrieve(frame) || frame.empty()) {
            continue;
        }

        if (sessionId != m_rtspSessionId.load()) break;

        m_lastFrameTime.store(QDateTime::currentMSecsSinceEpoch());

        // Luckfox 必须直接输出原生 1280x720。这里拒绝其他尺寸，避免把低分辨率放大
        // 或把高分辨率缩小后误认为设备端已经满足 720p 传输要求。
        if (frame.cols != kRealtimeMaxWidth || frame.rows != kRealtimeMaxHeight) {
            const int actualWidth = frame.cols;
            const int actualHeight = frame.rows;
            if (!m_rtspFormatErrorReported.exchange(true, std::memory_order_acq_rel)) {
                QMetaObject::invokeMethod(this, [this, sessionId, actualWidth, actualHeight]() {
                    if (sessionId != m_rtspSessionId.load()) return;
                    m_rtspRunning.store(false, std::memory_order_release);
                    m_cameraState = CameraState::Error;
                    emit stateChanged(m_cameraState);
                    emit cameraError(
                        QStringLiteral("Luckfox RTSP规格不匹配：收到 %1×%2，要求原生 1280×720@30；请先修改设备端编码器配置")
                            .arg(actualWidth)
                            .arg(actualHeight));
                }, Qt::QueuedConnection);
            }
            break;
        }

        // 画面方向变换处理（在 720P 下执行，极速无负担）
        const int rot = m_rotationMode.load();
        if (rot == 1) {
            cv::flip(frame, frame, -1);
        } else if (rot == 2) {
            cv::flip(frame, frame, 1);
        } else if (rot == 3) {
            cv::flip(frame, frame, 0);
        } else if (rot == 4) {
            cv::rotate(frame, frame, cv::ROTATE_90_CLOCKWISE);
        } else if (rot == 5) {
            cv::rotate(frame, frame, cv::ROTATE_90_COUNTERCLOCKWISE);
        }

        if (sessionId != m_rtspSessionId.load()) break;

        // 存入最新帧（移动语义，零冗余拷贝）
        {
            std::lock_guard<std::mutex> lock(m_rtspMatMutex);
            if (sessionId != m_rtspSessionId.load()) break;
            m_latestRtspMat = std::move(frame);
            m_hasNewRtspMat.store(true, std::memory_order_release);
        }

        bool expected = false;
        if (m_rtspDispatchPending.compare_exchange_strong(expected, true)) {
            QMetaObject::invokeMethod(this, [this, sessionId]() {
                notifyNewMat(sessionId);
            }, Qt::QueuedConnection);
        }
    }

    cap.release();
}

void CameraManager::openCamera(const QString &cameraId)
{
    // 如果当前正在开启或已经处于开启状态，先彻底关停旧设备以平滑切换
    if (m_cameraState == CameraState::Opening || m_cameraState == CameraState::Open) {
        safeDeleteCamera();
    }

    m_cameraState = CameraState::Opening;
    emit stateChanged(m_cameraState);
    m_currentCameraId = cameraId;

    // 若为 RTSP 网络流地址
    if (cameraId.startsWith("rtsp://", Qt::CaseInsensitive)) {
        startRtspStream(cameraId);
        return;
    }

    // 否则走本地 USB 物理摄像头通道
    try {
        QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
        QCameraDevice selectedCamera;
        
        for (const QCameraDevice &device : std::as_const(cameras)) {
            if (device.id() == cameraId.toUtf8()) {
                selectedCamera = device;
                break;
            }
        }
        
        if (!selectedCamera.isNull()) {
            safeDeleteCamera();
            QCamera *camera = new QCamera(selectedCamera, this);

            // 720P 极速零延迟配置：遍历相机格式，精准锁定 1280x720 最高帧率模式
            const auto formats = selectedCamera.videoFormats();
            QCameraFormat bestFormat;
            int bestScore = -1;
            for (const auto &fmt : formats) {
                const QSize res = fmt.resolution();
                const float fps = fmt.maxFrameRate();
                int score = 0;
                // 精准匹配 1280x720
                if (res.width() == 1280 && res.height() == 720) {
                    score += 10000;
                } else {
                    int dist = std::abs(res.width() - 1280) + std::abs(res.height() - 720);
                    score += std::max(0, 5000 - dist * 2);
                }
                // 优先最高帧率 (如 60FPS / 30FPS)
                score += static_cast<int>(fps * 100);
                // 优先低延迟低带宽编解码格式 (MJPEG / NV12)
                if (fmt.pixelFormat() == QVideoFrameFormat::Format_Jpeg) {
                    score += 500;
                } else if (fmt.pixelFormat() == QVideoFrameFormat::Format_NV12) {
                    score += 300;
                }
                if (score > bestScore) {
                    bestScore = score;
                    bestFormat = fmt;
                }
            }
            if (!bestFormat.isNull()) {
                camera->setCameraFormat(bestFormat);
                qDebug() << "[CameraManager] USB 相机已锁定 720P 零延迟格式:"
                         << bestFormat.resolution() << "@" << bestFormat.maxFrameRate() << "FPS"
                         << bestFormat.pixelFormat();
            }

            {
                QMutexLocker locker(&m_cameraMutex);
                m_camera = camera;
            }
            m_captureSession->setCamera(camera);
            
            connect(camera, &QCamera::activeChanged, this, &CameraManager::onCameraActiveChanged);
            connect(camera, &QCamera::errorOccurred, this, &CameraManager::onCameraErrorOccurred);
            
            m_lastFrameTime = QDateTime::currentMSecsSinceEpoch();
            camera->start();
        } else {
            m_cameraState = CameraState::Idle;
            emit stateChanged(m_cameraState);
            emit cameraError("Cannot find specified camera.");
        }
    } catch (...) {
        m_cameraState = CameraState::Idle;
        emit stateChanged(m_cameraState);
        emit cameraError("Exception while opening camera.");
    }
}

void CameraManager::closeCamera()
{
    if (m_cameraState == CameraState::Idle || m_cameraState == CameraState::Closing) return;
    m_cameraState = CameraState::Closing;
    emit stateChanged(m_cameraState);

    safeDeleteCamera(); // 执行清理流程（含停止 RTSP 线程）

    m_cameraState = CameraState::Idle;
    emit stateChanged(m_cameraState);
}

void CameraManager::emergencyStop()
{
    if (m_cameraState == CameraState::Closing || m_cameraState == CameraState::Idle) return;
    m_cameraState = CameraState::Error;
    emit stateChanged(m_cameraState);
    safeDeleteCamera();
}

void CameraManager::onCameraActiveChanged(bool active)
{
    if (active) {
        m_cameraState = CameraState::Open;
        m_retryCount = 0;
        emit stateChanged(m_cameraState);
    } else {
        if (m_cameraState != CameraState::Closing && m_cameraState != CameraState::Idle) {
            m_cameraState = CameraState::Error;
            emit stateChanged(m_cameraState);
        }
    }
}

void CameraManager::onCameraErrorOccurred(QCamera::Error error, const QString &errorString)
{
    Q_UNUSED(error);
    qWarning() << "Camera Error:" << errorString;
    m_cameraState = CameraState::Error;
    emit stateChanged(m_cameraState);
    emit cameraError(errorString);
    safeDeleteCamera();
}

void CameraManager::handleNewVideoFrame(const QVideoFrame &frame)
{
    m_lastFrameTime = QDateTime::currentMSecsSinceEpoch();
    if (m_cameraState == CameraState::Opening) {
        m_cameraState = CameraState::Open;
        emit stateChanged(m_cameraState);
    }
    emit frameReady(frame);
}

void CameraManager::checkCameraStatus()
{
    if (m_cameraState == CameraState::Opening || m_cameraState == CameraState::Closing) return;

    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    bool cameraIsOpen = (m_cameraState == CameraState::Open);

    // 如果处于 Open 状态，但超过 5 秒没有收到任何画面帧
    if (cameraIsOpen && (currentTime - m_lastFrameTime.load() > 5000)) {
        qWarning() << "Camera timeout detected, attempting recovery...";
        emergencyStop();
        m_retryCount++;
        if (m_retryCount < 3) {
            openCamera(m_currentCameraId);
        } else {
            emit cameraError("Camera connection lost completely.");
        }
    }
}

CameraManager::CameraState CameraManager::state() const
{
    return m_cameraState;
}

QString CameraManager::currentCameraId() const
{
    return m_currentCameraId;
}

void CameraManager::setRotationMode(RotationMode mode)
{
    m_rotationMode.store(static_cast<int>(mode));
}

CameraManager::RotationMode CameraManager::rotationMode() const
{
    return static_cast<RotationMode>(m_rotationMode.load());
}

void CameraManager::setFlip180(bool flip)
{
    setRotationMode(flip ? RotationMode::Rotate180 : RotationMode::Normal);
}

bool CameraManager::isFlip180() const
{
    return m_rotationMode.load() == static_cast<int>(RotationMode::Rotate180);
}
