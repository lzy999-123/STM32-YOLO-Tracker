#ifndef DNNTHREAD_H
#define DNNTHREAD_H

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <atomic>
#include <opencv2/dnn.hpp>
#include <opencv2/opencv.hpp>
#include <onnxruntime_cxx_api.h>

/**
 * @brief DNN 推理线程类
 * 作为项目中的“大脑”，在独立线程中运行 YOLO 模型（基于 ONNX Runtime 或 OpenCV DNN）。
 * 负责目标检测（Detection）以及辅助追踪的恢复和校验。
 */
class DnnThread : public QThread
{
    Q_OBJECT
#ifdef TRACKING_CONCURRENCY_TEST
    friend class TrackingConcurrencyTest;
#endif
public:
    /**
     * @brief 构造函数
     * @param modelFileName YOLO模型文件的路径
     * @param parent 父对象
     */
    explicit DnnThread(const QString &modelFileName, QObject *parent = nullptr);
    ~DnnThread();

    /// @brief 模型加载状态：加载在 run()（推理线程）中进行，构造函数不再阻塞 GUI。
    enum class ModelState { Loading = 0, Ready = 1, Failed = 2 };

    /**
     * @brief 初始化 DNN 跟踪器的锁定目标
     * @param frame 初始化时的完整图像帧
     * @param target 用户框选或追踪引擎锁定的目标初始位置
     * @return 本次跟踪会话 ID；dnnTrackedResult 携带该 ID，旧会话的结果应被丢弃
     */
    quint64 initDnn(const cv::Mat &frame, const cv::Rect2d &target);
    
    /**
     * @brief 传递新的一帧给 DNN 线程进行处理
     * @param frame 新的图像帧
     */
    void updateDnn(const cv::Mat &frame);
    
    /**
     * @brief 请求全图目标检测
     * @param frame 需要检测的图像帧
     * @param requestId 请求ID，用于异步回调时的匹配
     */
    void requestDetections(const cv::Mat &frame, quint64 requestId);
    
    /// @brief 停止并退出 DNN 线程
    void stopDnn();
    
    /// @brief 判断当前线程是否正在处理数据（正在忙碌）
    bool isBusy(); 
    
    /// @brief 判断模型是否已经完成了预热（加载到显存/内存完毕）
    bool isWarmedUp();
    
    /// @brief 判断网络模型当前是否不可用（仍在加载或加载失败）
    bool isNetEmpty() const { return modelState() != ModelState::Ready; }

    /// @brief 模型是否仍在后台加载
    bool isModelLoading() const { return modelState() == ModelState::Loading; }

    /// @brief 模型是否已确定加载失败
    bool isLoadFailed() const { return modelState() == ModelState::Failed; }

    ModelState modelState() const
    {
        return static_cast<ModelState>(m_modelState.load(std::memory_order_acquire));
    }
    
    /// @brief 获取当前加载的模型文件名
    QString modelFileName() const;

signals:
    /**
     * @brief 持续追踪结果的回调信号（用于校验当前目标状态）
     * @param rect 识别到的目标位置
     * @param success 是否成功识别到目标
     * @param className 目标的分类名称；"HOLD" 表示本帧未检测到目标、仅沿用上一位置（不是新测量）
     * @param sessionId 产生该结果的跟踪会话 ID（见 initDnn），用于丢弃旧会话结果
     */
    void dnnTrackedResult(const cv::Rect2d &rect, bool success, const QString &className, quint64 sessionId);

    /**
     * @brief 模型加载完成信号（在推理线程中发出）
     * @param success 是否加载成功
     * @param message 加载结果说明
     */
    void modelLoadFinished(bool success, const QString &message);
    
    /**
     * @brief 模型预热完成信号
     * @param success 预热是否成功
     * @param message 相关消息或错误提示
     */
    void dnnWarmupFinished(bool success, const QString &message);
    
    /**
     * @brief 异步目标检测的结果信号
     * @param rect 检测框位置
     * @param classId 类别ID
     * @param confidence 置信度
     * @param requestId 对应的检测请求ID
     * @param finished 当前请求的所有检测框是否已全部发送完毕（为true表示结束）
     */
    void yoloDetectionResult(
        const cv::Rect2d &rect,
        int classId,
        float confidence,
        quint64 requestId,
        bool finished);

protected:
    /// @brief 线程的核心执行循环
    void run() override;

private:
    /// @brief 在推理线程中加载 ONNX 模型；成功返回 true
    bool loadModel();

    mutable QMutex m_mutex;               ///< 线程安全锁
    std::atomic<int> m_modelState{static_cast<int>(ModelState::Loading)}; ///< 模型加载状态
    QWaitCondition m_workAvailable;       ///< 有新推理任务时唤醒线程，避免空闲轮询
    QString m_modelFileName;              ///< 模型文件路径
    QString m_modelLoadError;             ///< 模型加载错误信息
    Ort::Env m_ortEnv{nullptr};           ///< ONNX Runtime 运行环境
    Ort::Session* m_ortSession = nullptr; ///< ONNX Runtime 推理会话对象
    std::vector<const char*> m_inputNodeNames;  ///< ONNX 输入节点名称
    std::vector<const char*> m_outputNodeNames; ///< ONNX 输出节点名称
    std::vector<std::string> m_inputNodeNamesStr;
    std::vector<std::string> m_outputNodeNamesStr;
    
    int m_lockedClassId;                  ///< 锁定的目标类别ID（用于持续追踪同类目标）
    int m_dnnMissCount;                   ///< 连续丢失目标的帧数计数
    bool m_useYolo;                       ///< 是否启用 YOLO 作为核心追踪策略
    std::vector<std::string> m_classNames;///< COCO 等数据集的类别名称列表
    cv::Rect2d m_lastYoloRect;            ///< 上一帧通过 YOLO 检测到的目标位置

    cv::Mat m_frame;                      ///< 供追踪更新使用的当前帧
    cv::Mat m_initFrame;                  ///< 供初始化的图像帧
    cv::Mat m_detectFrame;                ///< 供全图检测使用的图像帧
    cv::Rect2d m_initRect;                ///< 初始化时的目标框
    quint64 m_detectionRequestId = 0;     ///< 当前正在处理的检测请求ID
    quint64 m_sessionId = 0;              ///< 当前跟踪会话 ID，initDnn/stopDnn 时更新
    
    bool m_isTracking;                    ///< 线程运行标志位
    bool m_needInit;                      ///< 是否需要执行初始化标志
    bool m_needDetection = false;         ///< 是否有待处理的检测请求
    bool m_hasNewFrame;                   ///< 是否有新的视频帧等待处理
    bool m_isComputing;                   ///< 内部推理计算状态标志
    bool m_isWarmedUp;                    ///< 模型是否预热完毕标志
};

#endif // DNNTHREAD_H
