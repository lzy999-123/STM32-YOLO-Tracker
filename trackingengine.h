#ifndef TRACKINGENGINE_H
#define TRACKINGENGINE_H

#include <QObject>
#include <QMutex>
#include <QSize>
#include <QHash>
#include <QString>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <opencv2/tracking.hpp>
#include <opencv2/features2d.hpp>
#include "dnnthread.h"

/**
 * @brief 核心目标追踪引擎类
 * 结合了传统的特征点追踪（如 OpenCV Tracker）和深度学习（YOLO/DNN）机制，
 * 实现目标的高效锁定、抗遮挡追踪和丢失恢复。
 */
class TrackingEngine : public QObject
{
    Q_OBJECT
#ifdef TRACKING_CONCURRENCY_TEST
    friend class TrackingConcurrencyTest;
#endif
public:
    explicit TrackingEngine(QObject *parent = nullptr);
    ~TrackingEngine();

    /**
     * @brief 引擎初始化
     * @param frameSize 视频流的画面尺寸，用于边界检查
     */
    void init(const QSize &frameSize);
    
    /**
     * @brief 设置当前所选的追踪后端
     * @param useFeatureTracking 是否使用传统特征点追踪（false 表示纯 YOLO）
     * @param modelFileName 当前启用的模型文件
     */
    void setTrackingBackend(bool useFeatureTracking, const QString &modelFileName);
    
    /**
     * @brief 启动对特定目标的追踪
     * @param frame 当前帧图像
     * @param targetRect 框选的目标初始区域
     * @param useFeatureTracking 是否混合特征点追踪
     * @param modelFileName 模型文件名
     */
    void startTracking(const cv::Mat &frame, const cv::Rect2d &targetRect, bool useFeatureTracking, const QString &modelFileName);
    
    /// @brief 停止当前的追踪任务
    void stopTracking();
    
    /**
     * @brief 送入新的视频帧进行追踪计算
     * @param frame 新的画面帧
     */
    void processFrame(const cv::Mat &frame);
    
    /// @brief 动态更新画面尺寸（防止追踪框越界）
    void setFrameSize(const QSize &size);
    
    /// @brief 预加载常用的 YOLO 模型以节省切换时间
    void preloadYoloModels();
    
    /// @brief 切换当前正在使用的 YOLO 模型
    void setCurrentModel(const QString &modelFileName);
    
    /// @brief 检查 YOLO 线程是否可用
    bool yoloReady() const;
    
    /// @brief 检查 YOLO 模型是否预热完毕
    bool yoloWarmedUp() const;

    /// @brief 是否正在执行追踪任务
    bool isTracking() const;
    
    /// @brief 获取当前追踪到的目标位置
    cv::Rect2d currentTrackedRect() const;
    
    /// @brief 获取目标中心相对于画面中心的 X 轴偏移量
    int currentOffsetX() const;
    
    /// @brief 获取目标中心相对于画面中心的 Y 轴偏移量
    int currentOffsetY() const;
    
    /// @brief 是否启用了特征点混合追踪
    bool isFeatureTrackingSelected() const;

signals:
    /**
     * @brief 追踪成功信号，向主界面和串口控制器发送目标位置
     * @param rect 当前目标的坐标与尺寸
     * @param offsetX X轴偏差
     * @param offsetY Y轴偏差
     */
    void targetTracked(const cv::Rect2d &rect, int offsetX, int offsetY);
    
    /// @brief 目标丢失信号
    void targetLost();
    
    /// @brief 输出系统日志信号（例如恢复策略的判定过程）
    void logMessage(const QString &msg);

private slots:
    /// @brief 接收来自 DNN 线程的持续追踪结果
    void onDnnResultReceived(const cv::Rect2d &dnnRect, bool success, const QString &className = "");
    
    /// @brief 接收 YOLO 全局检测结果，用于目标丢失时的找回
    void onYoloDetectionResult(
        const cv::Rect2d &rect,
        int classId,
        float confidence,
        quint64 requestId,
        bool finished);

private:
    /// @brief 内部方法：重启指定的 DNN 模型线程
    void restartDnnThread(const QString &modelFileName);
    
    /// @brief 内部方法：获取或创建指定的 DNN 线程
    DnnThread* ensureDnnThread(const QString &modelFileName);
    
    /// @brief 重置传统特征点追踪器状态
    void resetFeatureTracker();
    
    /// @brief 初始化传统特征点追踪器
    bool initFeatureTracker(const cv::Mat &frame, const cv::Rect2d &target);
    
    /// @brief 更新一帧特征点追踪并返回是否成功
    bool updateFeatureTracker(const cv::Mat &frame);
    
    /// @brief 执行目标丢失恢复策略总入口
    bool recoverFeatureTracker(const cv::Mat &frame);
    
    /// @brief 建立目标特征模板库（颜色直方图、ORB特征点等），用于后续比对
    void buildFeatureReference(const cv::Mat &frame, const cv::Rect &target);
    
    /// @brief 计算目标框与原始模板的图像相似度
    double featurePatchSimilarity(const cv::Mat &frame, const cv::Rect &target) const;
    
    /// @brief 计算目标框的颜色直方图相似度
    double featureColorSimilarity(const cv::Mat &frame, const cv::Rect &target) const;
    
    /// @brief 计算目标框宽高的形状相似度
    double featureShapeSimilarity(const cv::Rect &target) const;
    
    /// @brief 综合评分系统：对候选目标的各项特征进行加权打分
    double featureCandidateScore(
        const cv::Mat &frame,
        const cv::Rect &target,
        double *templateScore = nullptr,
        double *colorScore = nullptr,
        double *shapeScore = nullptr) const;
    
    /// @brief 将特征追踪框与 YOLO 检测框进行对齐
    cv::Rect alignFeatureRectInYoloCandidate(const cv::Rect &yoloRect, const cv::Size &frameSize) const;
    
    /// @brief 评估 YOLO 提供的候选目标是否符合原始特征
    bool evaluateYoloFeatureCandidate(
        const cv::Mat &frame,
        const cv::Rect &yoloRect,
        cv::Rect &alignedRect,
        double *candidateScore = nullptr) const;
    
    /// @brief 校验候选框是否通过各项阈值测试
    bool featureCandidatePasses(
        const cv::Mat &frame,
        const cv::Rect &target,
        bool recoveryMode,
        double *candidateScore = nullptr) const;
    
    /// @brief YOLO恢复模式下的特殊阈值校验
    bool featureCandidatePassesYoloRecovery(
        const cv::Mat &frame,
        const cv::Rect &target,
        double *candidateScore = nullptr) const;
    
    /// @brief 确认并应用通过校验的找回目标框
    bool confirmFeatureRecoveryCandidate(
        const cv::Mat &frame,
        const cv::Rect &target,
        const QString &methodName,
        double candidateScore);
    
    /// @brief 向 DNN 线程请求识别当前目标的类别，用于后续跨物体找回
    void requestFeatureYoloTargetClassification(const cv::Mat &frame);
    
    /// @brief 向 DNN 线程请求全图检测以寻找丢失的目标
    void requestFeatureYoloRecovery(const cv::Mat &frame);
    
    /// @brief 获取下一个用于校验请求的自增ID
    quint64 nextFeatureYoloRequestId();
    
    /// @brief 重置寻找候选框的暂存状态
    void resetFeatureRecoveryCandidate();
    
    /// @brief 重置类别判定状态
    void resetFeatureYoloTargetClassification();
    
    /// @brief 传统恢复算法：ORB 特征点匹配找回
    bool recoverFeatureTrackerByOrb(const cv::Mat &frame, cv::Rect &recoveredRect) const;
    
    /// @brief 传统恢复算法：模板匹配 (Template Matching) 找回
    bool recoverFeatureTrackerByTemplate(const cv::Mat &frame, cv::Rect &recoveredRect) const;
    
    /// @brief 传统恢复算法：颜色直方图反向投影找回
    bool recoverFeatureTrackerByColor(const cv::Mat &frame, cv::Rect &recoveredRect) const;
    
    /// @brief 在确定的区域重新启动特征追踪器
    bool restartFeatureTrackerFromRect(const cv::Mat &frame, const cv::Rect &target, const QString &methodName);

private:
    mutable std::recursive_mutex m_stateMutex;
    cv::Size m_frameSize;                 ///< 记录视频画面尺寸
    bool m_useFeatureTracking = false;    ///< 当前是否在用特征追踪模式
    QString m_currentModelName;           ///< 当前选用的 YOLO 模型名称

    bool m_isTargetTracked = false;       ///< 追踪锁定状态
    bool m_isCapturing = false;           ///< 流程开启标志
    cv::Rect2d m_trackedRect;             ///< 追踪框位置
    cv::Rect2d m_selectedRect;            ///< 初始选定框位置
    int m_offsetX = 0;                    ///< 当前 X 轴偏移量
    int m_offsetY = 0;                    ///< 当前 Y 轴偏移量
    int m_lostFrameCount = 0;             ///< 连续丢失帧数计数

    DnnThread *m_dnnThread = nullptr;     ///< 当前指向的 DNN 线程
    QHash<QString, DnnThread*> m_dnnThreads; ///< 保存预热模型的字典列表

    cv::Ptr<cv::Tracker> m_featureTracker; ///< OpenCV 传统追踪器实例
    bool m_featureTrackerReady = false;   
    cv::Mat m_featureTemplate;            ///< 初始目标图像模板
    cv::Mat m_featureColorHist;           ///< 初始目标的颜色直方图
    std::vector<cv::KeyPoint> m_featureReferenceKeypoints; ///< 目标关键点参考集合
    cv::Mat m_featureReferenceDescriptors; ///< 目标关键点描述子
    cv::Size m_featureReferenceSize;      ///< 目标初始大小
    cv::Rect2d m_featureLastRect;         ///< 上一帧的目标位置
    qint64 m_lastFeatureRecoveryAttemptTime = 0; ///< 上次执行重捕搜索的时间
    cv::Rect2d m_featureRecoveryCandidateRect; 
    QString m_featureRecoveryCandidateMethod; 
    int m_featureUnreliableCount = 0;     ///< 结果不可靠计数器（应对追踪漂移）
    int m_featureRecoveryCandidateCount = 0; 
    
    // YOLO 协作恢复模块相关成员变量
    bool m_featureYoloRecoveryPending = false;
    quint64 m_featureYoloRecoveryRequestId = 0;
    cv::Mat m_featureYoloRecoveryFrame;
    cv::Rect m_featureYoloBestCandidate;
    double m_featureYoloBestScore = 0.0;
    int m_featureYoloBestClassId = -1;
    float m_featureYoloBestConfidence = 0.0f;
    qint64 m_lastFeatureYoloRecoveryRequestTime = 0;
    int m_featureYoloCandidatesEvaluated = 0;
    quint64 m_featureYoloNextRequestId = 0;
    
    // YOLO 目标定性分类模块成员变量
    bool m_featureYoloClassifyPending = false;
    quint64 m_featureYoloClassifyRequestId = 0;
    cv::Mat m_featureYoloClassifyFrame;
    double m_featureYoloClassifyBestScore = 0.0;
    int m_featureYoloClassifyBestClassId = -1;
    float m_featureYoloClassifyBestConfidence = 0.0f;
    cv::Rect m_featureYoloClassifyBestBox;
    bool m_featureYoloClassKnown = false;
    bool m_featureUseYoloRecovery = false;
    int m_featureTargetYoloClassId = -1;
    QString m_featureTargetYoloClassName;
    cv::Rect m_featureTargetYoloBox;
    
    // 相对尺寸与比例约束
    double m_featureYoloRelX = 0.0;
    double m_featureYoloRelY = 0.0;
    double m_featureYoloRelW = 0.0;
    double m_featureYoloRelH = 0.0;
    qint64 m_lastFeatureRecoveryStrategyLogTime = 0;
    QString m_lastFeatureRecoveryStrategyLog;
    qint64 m_lastFeatureYoloRecoveryLogTime = 0;

    // 常量定义，包含各项相似度与阈值的参数设置
    static constexpr double kFeatureMinSimilarityScore = 0.5;
    static constexpr double kFeatureYoloRecoveryScoreThreshold = 0.6;
    static constexpr int kMaxYoloAppearanceCandidatesPerFrame = 4;
    static constexpr double kFeatureYoloClassifyMinSelectedCoverage = 0.35;
    static constexpr double kFeatureYoloClassifyMinCandidateCoverage = 0.22;
    static constexpr double kFeatureYoloClassifyMinSizeSimilarity = 0.18;
    static constexpr int DEAD_ZONE = 18; ///< 偏移死区（在此像素范围内不进行云台微调）

    /// @brief 针对特定类别获取适配的置信度阈值
    float confidenceThresholdForClass(int classId, int targetClassId) const;
    
    /// @brief 将矩形框安全限制在画面内，防止访问越界
    cv::Rect boundedIntRect(const cv::Rect2d &rect, const cv::Size &size) const;
};

#endif // TRACKINGENGINE_H
