#ifndef DNNTHREAD_H
#define DNNTHREAD_H

#include <QThread>
#include <QMutex>
#include <QImage>
#include <opencv2/dnn.hpp>
#include <opencv2/opencv.hpp>
#include <onnxruntime_cxx_api.h>

class DnnThread : public QThread
{
    Q_OBJECT
public:
    explicit DnnThread(const QString &modelFileName, QObject *parent = nullptr);
    ~DnnThread();

    void initDnn(const cv::Mat &frame, const cv::Rect2d &target);
    void updateDnn(const cv::Mat &frame);
    void requestDetections(const cv::Mat &frame, quint64 requestId);
    void stopDnn();
    bool isBusy(); // 判断大脑是否正在忙碌
    bool isWarmedUp();
    bool isNetEmpty() { return m_ortSession == nullptr; }
    QString modelFileName() const { return m_modelFileName; }

signals:
    void dnnTrackedResult(const cv::Rect2d &rect, bool success, const QString &className = "");
    void dnnWarmupFinished(bool success, const QString &message);
    void yoloDetectionResult(
        const cv::Rect2d &rect,
        int classId,
        float confidence,
        quint64 requestId,
        bool finished);

protected:
    void run() override;

private:
    QMutex m_mutex;
    QString m_modelFileName;
    QString m_modelLoadError;
    Ort::Env m_ortEnv{nullptr};
    Ort::Session* m_ortSession = nullptr;
    std::vector<const char*> m_inputNodeNames;
    std::vector<const char*> m_outputNodeNames;
    std::vector<std::string> m_inputNodeNamesStr;
    std::vector<std::string> m_outputNodeNamesStr;
    int m_lockedClassId;                  // 锁定的目标类别ID
    int m_dnnMissCount;
    bool m_useYolo;                       // 是否使用YOLO跟踪
    std::vector<std::string> m_classNames;// COCO类别名称列表
    cv::Rect2d m_lastYoloRect;            // 上一帧的YOLO目标位置

    cv::Mat m_frame;
    cv::Mat m_initFrame;
    cv::Mat m_detectFrame;
    cv::Rect2d m_initRect;
    quint64 m_detectionRequestId = 0;
    bool m_isTracking;
    bool m_needInit;
    bool m_needDetection = false;
    bool m_hasNewFrame;
    bool m_isComputing;
    bool m_isWarmedUp;
    
};

#endif // DNNTHREAD_H
