#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "serialcontroller.h"
#include "cameramanager.h"
#include "luckfoxdiscovery.h"
#include "trackingengine.h"
#include <QMouseEvent>
#include <QPaintEvent>
#include <QTimer>
#include <QImage>
#include <QPixmap>
#include <QMutex>
#include <QRect>
#include <QCloseEvent>

#include <atomic>
#include <mutex>
#include <thread>

#include <opencv2/opencv.hpp>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE


/**
 * @brief Qt 桌面主窗口类
 * 负责整个程序的界面呈现、用户交互（如鼠标框选）、各子模块（相机、串口、引擎）的管理调度，
 * 并负责汇总数据进行界面 UI 的实时重绘。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    /// @brief 追踪后端枚举：YOLO 或 传统特征点混合
    enum class TrackingBackend { Yolo, Feature };

    MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /**
     * @brief 图像格式转换工具：QImage 转 OpenCV cv::Mat
     */
    static cv::Mat QImageToCvMat(const QImage& qImage);
    
    /**
     * @brief 图像格式转换工具：QVideoFrame 转 OpenCV cv::Mat
     */
    cv::Mat QVideoFrameToCvMat(const QVideoFrame &frame);
    
    /**
     * @brief 图像格式转换工具：OpenCV cv::Mat 转 QImage
     */
    static QImage CvMatToQImage(const cv::Mat& mat);

private slots:
    // UI 按钮与控件事件的槽函数
    void on_pushButton_clicked();               ///< 可能是连接/断开串口等按钮
    void on_pushButton_8_clicked();             ///< 某控制按钮
    void onCameraChanged(int index);            ///< 用户切换摄像头下拉框
    void on_pushButton_9_clicked();             ///< 开启/关闭摄像头等按钮
    
    /// @brief 接收底层摄像头传来的最新视频帧
    void handleNewVideoFrame(const QVideoFrame &frame);

    /// @brief 接收直接解码的高性能 cv::Mat 视频帧（零内存拷贝）
    void handleNewMatFrame(const cv::Mat &mat);
    
    /// @brief 对接收的视频帧进行真正的显示与逻辑处理
    void processLatestVideoFrame();
    
    void on_btnSelectTarget_clicked();          ///< 点击“选择目标”按钮
    void on_btnStartTracking_clicked();         ///< 点击“开始追踪”按钮
    void on_btnStopTracking_clicked();          ///< 点击“停止追踪”按钮
    void on_pushButton_2_clicked();             ///< 串口云台测试/移动控制指令
    void on_pushButton_3_clicked();             ///< 串口云台测试/移动控制指令
    void on_pushButton_4_clicked();             ///< 手动模式：向上步进
    void on_pushButton_5_clicked();             ///< 手动模式：向左步进
    void on_pushButton_6_clicked();             ///< 手动模式：向右步进
    void on_pushButton_7_clicked();             ///< 手动模式：向下步进
    void onTrackingModelChanged(int index);     ///< 用户切换 YOLO 模型下拉框

protected:
    /// @brief 拦截窗口关闭事件，确保各线程和底层硬件资源安全释放
    void closeEvent(QCloseEvent *event) override;
    
    // 以下为重写的鼠标与绘制事件，实现画框交互与结果展示
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    /// @brief 测试 DNN 环境可用性的内部方法
    void testDNN();
    
    /// @brief 向串口发送命令字的内部包装方法
    void sendCommand(uint8_t cmd);

    /// @brief 更新四个手动步进按钮的可用状态
    void updateManualControlAvailability();

    /// @brief 根据摄像头、串口、模式和追踪状态更新追踪按钮
    void updateTrackingControlAvailability();

    /// @brief 将特征跟踪帧交给独立线程，始终只保留最新帧
    void dispatchTrackingFrame(const cv::Mat &frame);

    /// @brief 等待后台特征跟踪帧处理结束
    void waitForTrackingWorker();

    /// @brief 安全停止跟踪，避免后台帧仍在访问引擎
    void stopTrackingSafely();

    /// @brief 执行一次固定 0.5° 的手动步进
    void sendManualStep(uint8_t cmd, const QString &direction);
    
    /// @brief 获取 UI 上当前选中的模型文件名
    QString currentTrackingModelFileName() const;
    
    /// @brief 获取 UI 上当前选用的追踪后端类型
    TrackingBackend currentTrackingBackend() const;
    
    /// @brief 判断当前是否选中了特征追踪模式
    bool isFeatureTrackingSelected() const;

    Ui::MainWindow *ui;                      ///< UI 界面对象
    SerialController m_serialController;     ///< 串口控制器模块
    CameraManager m_cameraManager;           ///< 摄像头管理器模块
    LuckfoxDiscovery m_luckfoxDiscovery;
    TrackingEngine m_trackingEngine;         ///< 目标追踪引擎核心模块
    
    cv::Mat m_lastFrame;                     ///< 转换后的 OpenCV 图像缓存
    QMutex m_pendingFrameMutex;              ///< 线程安全保护锁
    QVideoFrame m_pendingVideoFrame;         ///< 缓存未处理的最新的视频帧
    cv::Mat m_pendingMat;                    ///< 缓存未处理的最新的 cv::Mat（用于 RTSP 高性能零拷贝通道）
    bool m_hasPendingMat = false;            ///< 是否有待处理的 cv::Mat
    bool m_frameDispatchPending = false;     ///< 是否有待调度的视频帧

    bool m_isCapturing;                      ///< 摄像头是否正在取景中
    bool m_isSelecting;                      ///< 用户是否正在用鼠标画框
    bool m_hasSelectedTarget;                ///< 是否已经成功选定了追踪区域
    
    // 鼠标选取在 UI 和原图上的坐标记录
    QPoint m_selectStart;
    QPoint m_selectEnd;
    QPoint m_selectStartImg;
    QPoint m_selectEndImg;
    cv::Rect2d m_selectedRect;               ///< 用户画好的选区
    cv::Rect2d m_trackedRect;                ///< 引擎返回的当前追踪框

    QMutex m_modeMutex;
    int m_currentMode;                       ///< 系统的当前运行模式

    QMutex m_frameSizeMutex;
    QSize m_frameSize;                       ///< 当前画面分辨率
    bool m_isTargetTracked;                  ///< 当前是否锁定到目标

    int16_t m_offsetX;                       ///< UI 计算的云台相对中心点X轴偏移
    int16_t m_offsetY;                       ///< UI 计算的云台相对中心点Y轴偏移
    cv::Rect2d m_lastSelectedRect;           ///< 上次框选记录
    bool m_wasTrackingBeforeDisconn;         ///< 断开连接前是否处于追踪状态

    bool m_forceResetTracking;               ///< 是否强制重置追踪状态
    bool m_waitingForRecover;                ///< 是否正处于丢失后等待恢复阶段
    QPixmap m_renderCanvas;                  ///< 界面重绘用的像素画布缓存
    qint64 m_lastSerialSendTime;             ///< 上次通过串口发送数据的毫秒时间戳
    int m_lastRemoteMode = -1;               ///< 最近一次遥测模式，重连时会重置
    qint64 m_lastFpsCalcTime = 0;            ///< FPS 统计窗口起始时间
    int m_fpsFrameCount = 0;                 ///< 当前 FPS 窗口内帧数
    int m_displayedFps = 0;                  ///< 最近一次计算出的显示帧率
    qint64 m_lastInfoUpdateTime = 0;         ///< 追踪信息面板的节流时间戳
    qint64 m_lastDisplayFrameTime = 0;      ///< 最近一次显示帧时间戳，限制界面最多 30 FPS
    int m_dnnFrameSkipCounter = 0;           ///< 跳帧计数器（降低刷新率）
    int m_lostFrameCount = 0;                ///< 丢失目标的持续帧数

    // CSRT/ORB 更新可能超过一帧周期，不能阻塞 GUI 线程。这里采用单线程、最新帧覆盖策略。
    std::mutex m_trackingWorkerMutex;
    cv::Mat m_pendingTrackingFrame;
    std::thread m_trackingWorker;
    std::atomic<bool> m_trackingWorkerRunning{false};
    void trackingWorkerLoop();
};

#endif // MAINWINDOW_H
