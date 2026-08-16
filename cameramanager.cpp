#include "cameramanager.h"
#include <QMediaDevices>
#include <QCameraDevice>
#include <QDateTime>
#include <QDebug>
#include <QMutexLocker>
#include <QCoreApplication>

#include <utility>

CameraManager::CameraManager(QObject *parent)
    : QObject(parent),
      m_camera(nullptr),
      m_captureSession(new QMediaCaptureSession(this)),
      m_videoSink(new QVideoSink(this)),
      m_cameraState(CameraState::Idle),
      m_lastFrameTime(0),
      m_retryCount(0)
{
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

void CameraManager::openCamera(const QString &cameraId)
{
    // 防止重复调用
    if (m_cameraState == CameraState::Opening || m_cameraState == CameraState::Closing) return;

    m_cameraState = CameraState::Opening;
    emit stateChanged(m_cameraState);
    m_currentCameraId = cameraId;

    try {
        // 获取系统中所有可用的视频输入设备
        QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
        QCameraDevice selectedCamera;
        
        // 遍历比对，找到用户选中的那一个设备
        for (const QCameraDevice &device : std::as_const(cameras)) {
            if (device.id() == cameraId.toUtf8()) {
                selectedCamera = device;
                break;
            }
        }
        
        if (!selectedCamera.isNull()) {
            safeDeleteCamera();
            QCamera *camera = new QCamera(selectedCamera, this);
            {
                QMutexLocker locker(&m_cameraMutex);
                m_camera = camera;
            }
            // 实例化新的相机并绑定到捕获会话
            m_captureSession->setCamera(camera);
            
            // 连接状态和错误回调
            connect(camera, &QCamera::activeChanged, this, &CameraManager::onCameraActiveChanged);
            connect(camera, &QCamera::errorOccurred, this, &CameraManager::onCameraErrorOccurred);
            
            m_lastFrameTime = QDateTime::currentMSecsSinceEpoch();
            camera->start(); // 正式启动画面采集
        } else {
            // 没有找到匹配的设备
            m_cameraState = CameraState::Idle;
            emit stateChanged(m_cameraState);
            emit cameraError("Cannot find specified camera.");
        }
    } catch (...) {
        // 异常处理：捕获未知系统报错
        m_cameraState = CameraState::Idle;
        emit stateChanged(m_cameraState);
        emit cameraError("Exception while opening camera.");
    }
}

void CameraManager::closeCamera()
{
    if (m_cameraState == CameraState::Opening || m_cameraState == CameraState::Closing) return;
    m_cameraState = CameraState::Closing;
    emit stateChanged(m_cameraState);

    safeDeleteCamera(); // 执行清理流程

    m_cameraState = CameraState::Idle;
    emit stateChanged(m_cameraState);
}

void CameraManager::emergencyStop()
{
    if (m_cameraState == CameraState::Closing || m_cameraState == CameraState::Idle) return;
    // 强制中断相机连接，将其置于 Error 状态
    m_cameraState = CameraState::Error;
    emit stateChanged(m_cameraState);
    safeDeleteCamera();
}

void CameraManager::onCameraActiveChanged(bool active)
{
    if (active) {
        // 摄像头成功激活
        m_cameraState = CameraState::Open;
        m_retryCount = 0; // 重置重连次数
        emit stateChanged(m_cameraState);
    } else {
        // 摄像头非正常断开或意外关闭
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
    // 收到底层错误时立刻中止并报错
    m_cameraState = CameraState::Error;
    emit stateChanged(m_cameraState);
    emit cameraError(errorString);
    safeDeleteCamera();
}

void CameraManager::handleNewVideoFrame(const QVideoFrame &frame)
{
    // 更新最后一次收到画面的时间戳，用于心跳检测
    m_lastFrameTime = QDateTime::currentMSecsSinceEpoch();
    if (m_cameraState == CameraState::Opening) {
        m_cameraState = CameraState::Open;
        emit stateChanged(m_cameraState);
    }
    // 将帧抛出给主界面处理
    emit frameReady(frame);
}

void CameraManager::checkCameraStatus()
{
    if (m_cameraState == CameraState::Opening || m_cameraState == CameraState::Closing) return;

    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    bool cameraIsOpen = (m_cameraState == CameraState::Open);

    // 如果处于 Open 状态，但超过 2 秒没有收到任何画面帧
    if (cameraIsOpen && (currentTime - m_lastFrameTime > 2000)) {
        qWarning() << "Camera timeout detected, attempting recovery...";
        emergencyStop();
        m_retryCount++;
        // 尝试自动重连 3 次
        if (m_retryCount < 3) {
            openCamera(m_currentCameraId);
        } else {
            // 重连失败，向 UI 发出严重错误警报
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
