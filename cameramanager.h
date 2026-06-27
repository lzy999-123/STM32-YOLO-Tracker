#ifndef CAMERAMANAGER_H
#define CAMERAMANAGER_H

#include <QObject>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QVideoSink>
#include <QVideoFrame>
#include <QTimer>
#include <QMutex>
#include <QString>
#include <QImage>

class CameraManager : public QObject
{
    Q_OBJECT

public:
    enum class CameraState { Idle, Opening, Open, Closing, Error };

    explicit CameraManager(QObject *parent = nullptr);
    ~CameraManager();

    void openCamera(const QString &cameraId);
    void closeCamera();
    void emergencyStop();

    CameraState state() const;
    QString currentCameraId() const;
    void startChecking();

signals:
    void frameReady(const QVideoFrame &frame);
    void stateChanged(CameraManager::CameraState state);
    void cameraError(const QString &errorMsg);

private slots:
    void onCameraActiveChanged(bool active);
    void onCameraErrorOccurred(QCamera::Error error, const QString &errorString);
    void handleNewVideoFrame(const QVideoFrame &frame);
    void checkCameraStatus();

private:
    void safeDeleteCamera();

    QCamera *m_camera;
    QMediaCaptureSession *m_captureSession;
    QVideoSink *m_videoSink;
    QTimer *m_cameraCheckTimer;

    CameraState m_cameraState;
    QString m_currentCameraId;
    QMutex m_cameraMutex;

    qint64 m_lastFrameTime;
    int m_retryCount;
};

#endif // CAMERAMANAGER_H
