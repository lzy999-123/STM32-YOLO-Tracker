#ifndef TRACKINGENGINE_H
#define TRACKINGENGINE_H

#include <QObject>
#include <QMutex>
#include <QSize>
#include <QHash>
#include <QString>
#include <opencv2/opencv.hpp>
#include <opencv2/tracking.hpp>
#include <opencv2/features2d.hpp>
#include "dnnthread.h"

class TrackingEngine : public QObject
{
    Q_OBJECT
public:
    explicit TrackingEngine(QObject *parent = nullptr);
    ~TrackingEngine();

    void init(const QSize &frameSize);
    
    // User actions
    void setTrackingBackend(bool useFeatureTracking, const QString &modelFileName);
    void startTracking(const cv::Mat &frame, const cv::Rect2d &targetRect, bool useFeatureTracking, const QString &modelFileName);
    void stopTracking();
    void processFrame(const cv::Mat &frame);
    void setFrameSize(const QSize &size);
    void preloadYoloModels();
    void setCurrentModel(const QString &modelFileName);
    bool yoloReady() const;
    bool yoloWarmedUp() const;

    // Getters
    bool isTracking() const;
    cv::Rect2d currentTrackedRect() const;
    int currentOffsetX() const;
    int currentOffsetY() const;
    bool isFeatureTrackingSelected() const;

signals:
    void targetTracked(const cv::Rect2d &rect, int offsetX, int offsetY);
    void targetLost();
    void logMessage(const QString &msg);

private slots:
    void onDnnResultReceived(const cv::Rect2d &dnnRect, bool success, const QString &className = "");
    void onYoloDetectionResult(
        const cv::Rect2d &rect,
        int classId,
        float confidence,
        quint64 requestId,
        bool finished);

private:
    void restartDnnThread(const QString &modelFileName);
    DnnThread* ensureDnnThread(const QString &modelFileName);
    
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
    bool featureCandidatePassesYoloRecovery(
        const cv::Mat &frame,
        const cv::Rect &target,
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

private:
    cv::Size m_frameSize;
    bool m_useFeatureTracking = false;
    QString m_currentModelName;

    bool m_isTargetTracked = false;
    bool m_isCapturing = false;
    cv::Rect2d m_trackedRect;
    cv::Rect2d m_selectedRect;
    int m_offsetX = 0;
    int m_offsetY = 0;
    int m_lostFrameCount = 0;

    DnnThread *m_dnnThread = nullptr;
    QHash<QString, DnnThread*> m_dnnThreads;

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
    double m_featureYoloClassifyBestScore = 0.0;
    int m_featureYoloClassifyBestClassId = -1;
    float m_featureYoloClassifyBestConfidence = 0.0f;
    cv::Rect m_featureYoloClassifyBestBox;
    bool m_featureYoloClassKnown = false;
    bool m_featureUseYoloRecovery = false;
    int m_featureTargetYoloClassId = -1;
    QString m_featureTargetYoloClassName;
    cv::Rect m_featureTargetYoloBox;
    double m_featureYoloRelX = 0.0;
    double m_featureYoloRelY = 0.0;
    double m_featureYoloRelW = 0.0;
    double m_featureYoloRelH = 0.0;
    qint64 m_lastFeatureRecoveryStrategyLogTime = 0;
    QString m_lastFeatureRecoveryStrategyLog;

    // Constants
    static constexpr double kFeatureMinSimilarityScore = 0.5;
    static constexpr double kFeatureYoloRecoveryScoreThreshold = 0.6;
    static constexpr int kMaxYoloAppearanceCandidatesPerFrame = 4;
    static constexpr double kFeatureYoloClassifyMinSelectedCoverage = 0.35;
    static constexpr double kFeatureYoloClassifyMinCandidateCoverage = 0.22;
    static constexpr double kFeatureYoloClassifyMinSizeSimilarity = 0.18;
    static constexpr int DEAD_ZONE = 18;

    float confidenceThresholdForClass(int classId, int targetClassId) const;
    cv::Rect boundedIntRect(const cv::Rect2d &rect, const cv::Size &size) const;
};

#endif // TRACKINGENGINE_H
