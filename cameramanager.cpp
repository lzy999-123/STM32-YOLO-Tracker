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
    m_captureSession->setVideoSink(m_videoSink);
    connect(m_videoSink, &QVideoSink::videoFrameChanged, this, &CameraManager::handleNewVideoFrame);

    m_cameraCheckTimer = new QTimer(this);
    m_cameraCheckTimer->setInterval(1000);
    connect(m_cameraCheckTimer, &QTimer::timeout, this, &CameraManager::checkCameraStatus);
}

CameraManager::~CameraManager()
{
    m_cameraCheckTimer->stop();
    safeDeleteCamera();
}

void CameraManager::startChecking()
{
    m_cameraCheckTimer->start();
}

void CameraManager::safeDeleteCamera()
{
    QMutexLocker locker(&m_cameraMutex);
    if (m_camera) {
        disconnect(m_camera, &QCamera::activeChanged, this, nullptr);
        disconnect(m_camera, &QCamera::errorOccurred, this, nullptr);
        m_camera->stop();
        m_captureSession->setCamera(nullptr);
        m_camera->deleteLater();
        m_camera = nullptr;
    }
}

void CameraManager::openCamera(const QString &cameraId)
{
    if (m_cameraState == CameraState::Opening || m_cameraState == CameraState::Closing) return;

    m_cameraState = CameraState::Opening;
    emit stateChanged(m_cameraState);
    m_currentCameraId = cameraId;

    try {
        QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
        QCameraDevice selectedCamera;
        for (const QCameraDevice &device : std::as_const(cameras)) {
            if (device.id() == cameraId) {
                selectedCamera = device;
                break;
            }
        }
        
        if (!selectedCamera.isNull()) {
            QMutexLocker locker(&m_cameraMutex);
            if (m_camera) {
                m_camera->stop();
                m_camera->deleteLater();
            }
            m_camera = new QCamera(selectedCamera);
            m_captureSession->setCamera(m_camera);
            
            connect(m_camera, &QCamera::activeChanged, this, &CameraManager::onCameraActiveChanged);
            connect(m_camera, &QCamera::errorOccurred, this, &CameraManager::onCameraErrorOccurred);
            
            m_lastFrameTime = QDateTime::currentMSecsSinceEpoch();
            m_camera->start();
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
    if (m_cameraState == CameraState::Opening || m_cameraState == CameraState::Closing) return;
    m_cameraState = CameraState::Closing;
    emit stateChanged(m_cameraState);

    safeDeleteCamera();

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

    if (cameraIsOpen && (currentTime - m_lastFrameTime > 2000)) {
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
