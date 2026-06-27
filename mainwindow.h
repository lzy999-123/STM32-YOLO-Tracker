#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSerialPort>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QTimer>
#include <QImage>
#include <QPixmap>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QVideoWidget>
#include <QVideoSink>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QMutex>
#include <QThread>
#include <QRect>
#include <QCloseEvent>
#include <QHash>

#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/tracking.hpp>
#include <onnxruntime_cxx_api.h>
#include <vector>
#include <string>

using namespace cv;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE


// ==========================================
// 大脑：DNN 深度学习纠错线程
// ==========================================
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
    
    double m_relX;
    double m_relY;
    double m_relW;
    double m_relH;
};



// ==========================================
// 主窗口类
// ==========================================
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    enum class CameraState { Idle, Opening, Open, Closing, Error };
    enum class TrackingBackend { Yolo, Feature };

    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    static cv::Mat QImageToCvMat(const QImage& qImage);
    cv::Mat QVideoFrameToCvMat(const QVideoFrame &frame);
    static QImage CvMatToQImage(const cv::Mat& mat);

private slots:
    void emergencyCameraStop();
    void on_pushButton_clicked();
    void on_pushButton_8_clicked();
    void messlot();
    void onCameraChanged(int index);
    void on_pushButton_9_clicked();
    void handleNewVideoFrame(const QVideoFrame &frame);
    void processLatestVideoFrame();
    void on_btnSelectTarget_clicked();
    void on_btnStartTracking_clicked();
    void on_btnStopTracking_clicked();
    void on_pushButton_2_clicked();
    void on_pushButton_3_clicked();
    void onDnnResultReceived(const cv::Rect2d &dnnRect, bool success, const QString &className = "");
    void onYoloDetectionResult(
        const cv::Rect2d &rect,
        int classId,
        float confidence,
        quint64 requestId,
        bool finished);
    void onTrackingModelChanged(int index);

protected:
    void closeEvent(QCloseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void testDNN();
    void sendCommand(uint8_t cmd);
    void restartDnnThread(const QString &modelFileName);
    void preloadYoloModels();
    DnnThread *ensureDnnThread(const QString &modelFileName);
    QString currentTrackingModelFileName() const;
    TrackingBackend currentTrackingBackend() const;
    bool isFeatureTrackingSelected() const;
    void resetFeatureTracker();
    bool initFeatureTracker(const cv::Mat &frame, const cv::Rect2d &target);
    bool updateFeatureTracker(const cv::Mat &frame);
    bool recoverFeatureTracker(const cv::Mat &frame);
    void buildFeatureReference(const cv::Mat &frame, const cv::Rect &target);
    double featurePatchSimilarity(const cv::Mat &frame, const cv::Rect &target) const;
    double featureColorSimilarity(const cv::Mat &frame, const cv::Rect &target) const;
    double featureShapeSimilarity(const cv::Rect &target) const;
    double featureCandidateScore(
        const cv::Mat &frame,
        const cv::Rect &target,
        double *templateScore = nullptr,
        double *colorScore = nullptr,
        double *shapeScore = nullptr) const;
    cv::Rect alignFeatureRectInYoloCandidate(const cv::Rect &yoloRect, const cv::Size &frameSize) const;
    bool evaluateYoloFeatureCandidate(
        const cv::Mat &frame,
        const cv::Rect &yoloRect,
        cv::Rect &alignedRect,
        double *candidateScore = nullptr) const;
    bool featureCandidatePasses(
        const cv::Mat &frame,
        const cv::Rect &target,
        bool recoveryMode,
        double *candidateScore = nullptr) const;
    bool confirmFeatureRecoveryCandidate(
        const cv::Mat &frame,
        const cv::Rect &target,
        const QString &methodName,
        double candidateScore);
    void requestFeatureYoloTargetClassification(const cv::Mat &frame);
    void requestFeatureYoloRecovery(const cv::Mat &frame);
    quint64 nextFeatureYoloRequestId();
    void resetFeatureRecoveryCandidate();
    void resetFeatureYoloTargetClassification();
    bool recoverFeatureTrackerByOrb(const cv::Mat &frame, cv::Rect &recoveredRect) const;
    bool recoverFeatureTrackerByTemplate(const cv::Mat &frame, cv::Rect &recoveredRect) const;
    bool recoverFeatureTrackerByColor(const cv::Mat &frame, cv::Rect &recoveredRect) const;
    bool restartFeatureTrackerFromRect(const cv::Mat &frame, const cv::Rect &target, const QString &methodName);

    Ui::MainWindow *ui;
    QSerialPort m_serial;
    QByteArray m_rx_buffer;

    QCamera *m_camera;
    QMediaCaptureSession *m_captureSession;
    QVideoWidget *m_videoWidget;
    QVideoSink *m_videoSink;
    cv::Mat m_lastFrame;
    QMutex m_pendingFrameMutex;
    QVideoFrame m_pendingVideoFrame;
    bool m_frameDispatchPending = false;

    bool m_isCapturing;
    bool m_isSelecting;
    bool m_hasSelectedTarget;
    QPoint m_selectStart;
    QPoint m_selectEnd;
    QPoint m_selectStartImg;
    QPoint m_selectEndImg;
    cv::Rect2d m_selectedRect;
    cv::Rect2d m_trackedRect;

    DnnThread *m_dnnThread;
    QHash<QString, DnnThread*> m_dnnThreads;

    QMutex m_modeMutex;
    int m_currentMode;

    QMutex m_frameSizeMutex;
    QSize m_frameSize;
    bool m_isTargetTracked;
    cv::Ptr<cv::Tracker> m_featureTracker;
    bool m_featureTrackerReady = false;
    cv::Mat m_featureTemplate;
    cv::Mat m_featureColorHist;
    std::vector<cv::KeyPoint> m_featureReferenceKeypoints;
    cv::Mat m_featureReferenceDescriptors;
    cv::Size m_featureReferenceSize;
    cv::Rect2d m_featureLastRect;
    cv::Rect2d m_featureRecoveryCandidateRect;
    QString m_featureRecoveryCandidateMethod;
    int m_featureUnreliableCount = 0;
    int m_featureRecoveryCandidateCount = 0;
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
    bool m_featureYoloClassifyPending = false;
    quint64 m_featureYoloClassifyRequestId = 0;
    cv::Mat m_featureYoloClassifyFrame;
    bool m_featureYoloClassKnown = false;
    bool m_featureUseYoloRecovery = false;
    int m_featureTargetYoloClassId = -1;
    QString m_featureTargetYoloClassName;
    cv::Rect m_featureTargetYoloBox;
    double m_featureYoloRelX = 0.0;
    double m_featureYoloRelY = 0.0;
    double m_featureYoloRelW = 1.0;
    double m_featureYoloRelH = 1.0;
    int m_featureYoloClassifyBestClassId = -1;
    float m_featureYoloClassifyBestConfidence = 0.0f;
    double m_featureYoloClassifyBestScore = 0.0;
    cv::Rect m_featureYoloClassifyBestBox;
    qint64 m_lastFeatureRecoveryStrategyLogTime = 0;
    QString m_lastFeatureRecoveryStrategyLog;

    int16_t m_offsetX;
    int16_t m_offsetY;

    QMutex m_cameraMutex;
    QTimer *m_cameraCheckTimer;
    cv::Rect2d m_lastSelectedRect;
    bool m_wasTrackingBeforeDisconn;
    qint64 m_lastFrameTime;
    QString m_currentCameraId;

    bool m_forceResetTracking;
    bool m_waitingForRecover;

    CameraState m_cameraState = CameraState::Idle;
    QPixmap m_renderCanvas;
    qint64 m_lastSerialSendTime;
    int m_dnnFrameSkipCounter = 0;
    int m_lostFrameCount = 0;
};

#endif // MAINWINDOW_H
