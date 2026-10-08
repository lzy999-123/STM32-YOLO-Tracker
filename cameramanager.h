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
#include <atomic>
#include <thread>
#include <memory>
#include <mutex>
#include <vector>
#include <opencv2/core.hpp>

Q_DECLARE_METATYPE(cv::Mat)

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

    /// @brief 画面旋转/翻转模式枚举
    enum class RotationMode {
        Normal = 0,     ///< 正常（不变换）
        Rotate180 = 1,  ///< 旋转 180 度
        MirrorH = 2,    ///< 水平镜像
        MirrorV = 3,    ///< 垂直翻转
        Rotate90 = 4,   ///< 顺时针 90 度
        Rotate270 = 5   ///< 逆时针 90 度
    };

    /// @brief 设置画面旋转/翻转模式
    void setRotationMode(RotationMode mode);
    RotationMode rotationMode() const;

    /// @brief 设置画面是否翻转 180 度（兼容旧接口）
    void setFlip180(bool flip);
    bool isFlip180() const;

signals:
    /**
     * @brief 当有新的视频帧到达时发出此信号
     * @param frame Qt 的原始视频帧对象
     */
    void frameReady(const QVideoFrame &frame);
    
    /**
     * @brief 当 RTSP 解码出新的 cv::Mat 帧时发出此信号（零内存拷贝高速通路）
     * @param mat OpenCV 原始图像
     */
    void matReady(const cv::Mat &mat);

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

    /// @brief 主线程槽函数，用于无阻塞分发最新 RTSP cv::Mat 帧
    void notifyNewMat(quint64 sessionId);

private:
    /// @brief 安全释放摄像头相关资源
    void safeDeleteCamera();

    /// @brief 启动 RTSP 网络流拉流
    void startRtspStream(const QString &url);

    /// @brief 停止 RTSP 网络流拉流
    void stopRtspStream();

    /// @brief RTSP 后台取流线程主循环
    void rtspWorkerLoop(const QString &url, quint64 sessionId);

    QCamera *m_camera;                      ///< 摄像头设备对象
    QMediaCaptureSession *m_captureSession; ///< 媒体捕获会话，用于连接设备与输出
    QVideoSink *m_videoSink;                ///< 视频帧输出槽
    QTimer *m_cameraCheckTimer;             ///< 用于定时检查连接状态的定时器

    CameraState m_cameraState;              ///< 当前摄像头状态
    QString m_currentCameraId;              ///< 当前摄像头 ID
    QMutex m_cameraMutex;                   ///< 多线程安全锁

    std::atomic<quint64> m_rtspSessionId{0}; ///< 当前活跃的 RTSP 会话唯一 ID（切流时立即作废旧线程）
    std::atomic<bool> m_isRtsp{false};       ///< 当前是否为 RTSP 网络流
    std::atomic<bool> m_rtspRunning{false};  ///< RTSP 线程运行标志
    std::atomic<bool> m_rtspFormatErrorReported{false}; ///< 是否已报告 RTSP 原生规格不匹配
    std::atomic<int> m_rotationMode{0};     ///< 画面旋转/翻转模式（0: 正常, 1: 180°, 2: 水平, 3: 垂直, 4: 90°, 5: 270°）
    std::unique_ptr<std::thread> m_rtspThread; ///< RTSP 后台解码线程
    std::vector<std::thread> m_retiredRtspThreads; ///< 取消的线程在销毁对象前统一等待结束

    std::mutex m_rtspMatMutex;              ///< RTSP 帧高速保护锁
    cv::Mat m_latestRtspMat;                ///< 最新的 RTSP 画面帧
    std::atomic<bool> m_hasNewRtspMat{false}; ///< 是否有未消费的最新帧
    std::atomic<bool> m_rtspDispatchPending{false}; ///< 是否已有待处理的分发通知

    std::atomic<qint64> m_lastFrameTime{0};  ///< 上一帧接收的时间戳，用于超时判定
    int m_retryCount;                       ///< 重连尝试次数
};

#endif // CAMERAMANAGER_H
