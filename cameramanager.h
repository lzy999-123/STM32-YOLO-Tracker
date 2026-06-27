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

/**
 * @brief 摄像头管理器类
 * 负责摄像头的打开、关闭、状态监控，以及视频帧流的获取分发。
 */
class CameraManager : public QObject
{
    Q_OBJECT

public:
    /// @brief 摄像头状态枚举
    enum class CameraState { 
        Idle,       ///< 空闲状态
        Opening,    ///< 正在打开
        Open,       ///< 成功打开并正在运行
        Closing,    ///< 正在关闭
        Error       ///< 发生错误
    };

    explicit CameraManager(QObject *parent = nullptr);
    ~CameraManager();

    /**
     * @brief 开启指定摄像头
     * @param cameraId 摄像头的设备ID
     */
    void openCamera(const QString &cameraId);
    
    /// @brief 关闭摄像头
    void closeCamera();
    
    /// @brief 紧急停止摄像头（非正常断开时调用）
    void emergencyStop();

    /// @brief 获取当前摄像头的状态
    CameraState state() const;
    
    /// @brief 获取当前使用的摄像头ID
    QString currentCameraId() const;
    
    /// @brief 启动摄像头状态巡检定时器（用于检测断开或卡死情况）
    void startChecking();

signals:
    /**
     * @brief 当有新的视频帧到达时发出此信号
     * @param frame Qt 的原始视频帧对象
     */
    void frameReady(const QVideoFrame &frame);
    
    /**
     * @brief 摄像头状态改变时发出此信号
     * @param state 新的摄像头状态
     */
    void stateChanged(CameraManager::CameraState state);
    
    /**
     * @brief 发生摄像头错误时发出此信号
     * @param errorMsg 错误提示信息
     */
    void cameraError(const QString &errorMsg);

private slots:
    /// @brief 摄像头激活状态改变的响应槽
    void onCameraActiveChanged(bool active);
    
    /// @brief 摄像头发生内部错误的响应槽
    void onCameraErrorOccurred(QCamera::Error error, const QString &errorString);
    
    /// @brief 接收 QVideoSink 传递来的新视频帧
    void handleNewVideoFrame(const QVideoFrame &frame);
    
    /// @brief 定时器调用的状态巡检函数，检查是否有画面超时卡死
    void checkCameraStatus();

private:
    /// @brief 安全释放摄像头相关资源
    void safeDeleteCamera();

    QCamera *m_camera;                      ///< 摄像头设备对象
    QMediaCaptureSession *m_captureSession; ///< 媒体捕获会话，用于连接设备与输出
    QVideoSink *m_videoSink;                ///< 视频帧输出槽
    QTimer *m_cameraCheckTimer;             ///< 用于定时检查连接状态的定时器

    CameraState m_cameraState;              ///< 当前摄像头状态
    QString m_currentCameraId;              ///< 当前摄像头 ID
    QMutex m_cameraMutex;                   ///< 多线程安全锁

    qint64 m_lastFrameTime;                 ///< 上一帧接收的时间戳，用于超时判定
    int m_retryCount;                       ///< 重连尝试次数
};

#endif // CAMERAMANAGER_H
