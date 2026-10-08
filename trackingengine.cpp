#include "trackingengine.h"
#include <QDateTime>
#include <QDebug>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kBottleClassId = 39;
constexpr float kDefaultConfidenceThreshold = 0.25f;
constexpr float kBottleConfidenceThreshold = 0.12f;
constexpr float kLockedClassConfidenceThreshold = 0.12f;
constexpr int kFeatureUnreliableLimit = 3;
constexpr int kOrbMinGoodMatches = 8;
constexpr int kOrbMinInliers = 6;
constexpr double kFeatureHardRejectSimilarity = 0.12;
constexpr double kFeatureRecoverMinTemplateSimilarity = 0.45;
constexpr double kFeatureRecoverMinColorSimilarity = 0.22;
constexpr double kFeatureRecoverMinCombinedScore = 0.50;
constexpr double kFeatureTrackMinTemplateSimilarity = 0.24;
constexpr double kFeatureTrackMinColorSimilarity = 0.18;
constexpr double kFeatureTrackMinCombinedScore = 0.36;
constexpr double kFeatureMinShapeSimilarity = 0.45;
constexpr double kFeatureYoloRecoverMinTemplateSimilarity = 0.16;
constexpr double kFeatureYoloRecoverMinColorSimilarity = 0.08;
constexpr double kFeatureYoloRecoverMinCombinedScore = 0.24;
constexpr int kFeatureRecoveryConfirmFrames = 1;
constexpr qint64 kFeatureYoloRecoveryIntervalMs = 100;
constexpr qint64 kFeatureRecoveryIntervalMs = 80;
constexpr qint64 kFeatureRecoveryStrategyLogIntervalMs = 1800;
constexpr int kFeatureCompareMaxSide = 160;
constexpr double kColorRecoverThreshold = 1.10;

float confidenceThresholdForClass(int classId, int lockedClassId)
{
    float threshold = kDefaultConfidenceThreshold;
    if (classId == kBottleClassId) {
        threshold = std::min(threshold, kBottleConfidenceThreshold);
    }
    if (classId == lockedClassId) {
        threshold = std::min(threshold, kLockedClassConfidenceThreshold);
    }
    return threshold;
}

cv::Rect boundedRect(const cv::Rect2d &rect, const cv::Size &frameSize)
{
    if (frameSize.width <= 0 || frameSize.height <= 0) {
        return cv::Rect();
    }

    const int left = std::clamp(
        static_cast<int>(std::floor(rect.x)), 0, frameSize.width);
    const int top = std::clamp(
        static_cast<int>(std::floor(rect.y)), 0, frameSize.height);
    const int right = std::clamp(
        static_cast<int>(std::ceil(rect.x + rect.width)), 0, frameSize.width);
    const int bottom = std::clamp(
        static_cast<int>(std::ceil(rect.y + rect.height)), 0, frameSize.height);

    if (right <= left || bottom <= top) {
        return cv::Rect();
    }
    return cv::Rect(left, top, right - left, bottom - top);
}

cv::Mat featureMatchImage(const cv::Mat &image)
{
    if (image.empty()) {
        return cv::Mat();
    }

    cv::Mat gray;
    if (image.channels() == 1) {
        gray = image.clone();
    } else if (image.channels() == 3) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else if (image.channels() == 4) {
        cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
    } else {
        image.convertTo(gray, CV_8U);
    }

    if (gray.depth() != CV_8U) {
        cv::Mat converted;
        gray.convertTo(converted, CV_8U);
        gray = converted;
    }
    if (gray.cols >= 3 && gray.rows >= 3) {
        cv::GaussianBlur(gray, gray, cv::Size(3, 3), 0);
    }
    return gray;
}

double medianValue(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }

    const size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    double value = values[mid];
    if (values.size() % 2 == 0) {
        std::nth_element(values.begin(), values.begin() + mid - 1, values.end());
        value = (value + values[mid - 1]) * 0.5;
    }
    return value;
}
}

TrackingEngine::TrackingEngine(QObject *parent) : QObject(parent), m_dnnThread(nullptr) {
    m_dnnThreads.clear();
}

TrackingEngine::~TrackingEngine()
{
    stopTracking();
    for (DnnThread *thread : std::as_const(m_dnnThreads)) {
        if (!thread) {
            continue;
        }
        thread->requestInterruption();
        thread->stopDnn();
        thread->wait();
    }
    m_dnnThreads.clear();
    m_dnnThread = nullptr;
}

float TrackingEngine::confidenceThresholdForClass(int classId, int targetClassId) const
{
    return ::confidenceThresholdForClass(classId, targetClassId);
}

cv::Rect TrackingEngine::boundedIntRect(const cv::Rect2d &rect, const cv::Size &size) const
{
    return boundedRect(rect, size);
}

void TrackingEngine::onDnnResultReceived(const cv::Rect2d &dnnRect, bool success, const QString &className) {
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    if (!m_isCapturing) return;

    QString resultName = className;
    const bool isRecoverResult = resultName.startsWith(QStringLiteral("RECOVER:"));
    if (resultName.startsWith(QStringLiteral("LOCK:"))) {
        QString actualName = resultName.mid(5);
        emit logMessage(QString("【系统】YOLO 已识别并锁定目标: %1").arg(actualName));
        resultName = actualName;
        // 注意：这里不再 early return，而是继续执行后面的逻辑以初始化 m_trackedRect 和状态
    } else if (isRecoverResult) {
        QString actualName = resultName.mid(8);
        emit logMessage(QString("【系统】YOLO 重捕完成，已重新锁定目标: %1").arg(actualName));
        resultName = actualName;
    }

    bool isReasonable = true;
    if (dnnRect.width < 10 || dnnRect.height < 10 ||
        dnnRect.width > m_frameSize.width * 0.8 ||
        dnnRect.height > m_frameSize.height * 0.8) {
        isReasonable = false;
    }

    const int DEAD_ZONE = 18;
    const bool isFeatureResult = (resultName == QStringLiteral("FEATURE"));
    bool currentSuccess = (success && isReasonable);

    if (currentSuccess) {
        m_isTargetTracked = true;
        m_lostFrameCount = 0; // 目标找回，清零

        if (m_trackedRect.empty() || isRecoverResult) {
            m_trackedRect = dnnRect;
        } else {
            const double oldCenterX = m_trackedRect.x + m_trackedRect.width * 0.5;
            const double oldCenterY = m_trackedRect.y + m_trackedRect.height * 0.5;
            const double newCenterX = dnnRect.x + dnnRect.width * 0.5;
            const double newCenterY = dnnRect.y + dnnRect.height * 0.5;
            const double movement = std::hypot(
                newCenterX - oldCenterX, newCenterY - oldCenterY);
            const double targetScale = std::max(
                20.0, std::max(dnnRect.width, dnnRect.height));
            const double movementRatio = movement / targetScale;

            double positionAlpha = isFeatureResult ? 0.45 : 1.0;
            if (isFeatureResult) {
                if (movementRatio > 0.18) {
                    positionAlpha = 1.0;
                } else if (movementRatio > 0.08) {
                    positionAlpha = 0.82;
                } else if (movementRatio > 0.03) {
                    positionAlpha = 0.65;
                }
            }
            const double sizeAlpha = isFeatureResult ? std::min(0.75, positionAlpha) : 0.22;
            const double sizeDeadZone =
                isFeatureResult
                    ? 0.0
                    : std::max(4.0, std::max(m_trackedRect.width, m_trackedRect.height) * 0.04);

            m_trackedRect.x += (dnnRect.x - m_trackedRect.x) * positionAlpha;
            m_trackedRect.y += (dnnRect.y - m_trackedRect.y) * positionAlpha;
            if (isFeatureResult || std::abs(dnnRect.width - m_trackedRect.width) > sizeDeadZone) {
                m_trackedRect.width += (dnnRect.width - m_trackedRect.width) * sizeAlpha;
            }
            if (isFeatureResult || std::abs(dnnRect.height - m_trackedRect.height) > sizeDeadZone) {
                m_trackedRect.height += (dnnRect.height - m_trackedRect.height) * sizeAlpha;
            }
        }

        int cx = m_frameSize.width / 2;
        int cy = m_frameSize.height / 2;
        int tx = cvRound(m_trackedRect.x + m_trackedRect.width / 2.0);
        int ty = cvRound(m_trackedRect.y + m_trackedRect.height / 2.0);

        int16_t offset_x = tx - cx;
        int16_t offset_y = ty - cy;

        if (abs(offset_x) < DEAD_ZONE) offset_x = 0;
        if (abs(offset_y) < DEAD_ZONE) offset_y = 0;

        m_offsetX = offset_x;
        m_offsetY = offset_y;
    } else {
        m_lostFrameCount++;
        const int MAX_LOST_TOLERANCE = isFeatureResult
                                           ? 0
                                           : 15; // YOLO 容忍短暂识别丢失；特征漂移立即清框

        if (m_lostFrameCount > MAX_LOST_TOLERANCE) {
            m_isTargetTracked = false;
            m_offsetX = 0;
            m_offsetY = 0;

            // 彻底丢失时的响应：清除框，并输出日志提醒
            if (m_lostFrameCount == MAX_LOST_TOLERANCE + 1) {
                emit logMessage("【警告】目标丢失！已清除追踪框，正在尝试找回...");
                m_trackedRect = cv::Rect2d(); // 立刻将宽和高变成0，界面不再画框
            }
        }
        // 如果 lostFrameCount <= MAX_LOST_TOLERANCE，则保持 m_isTargetTracked 为 true，
        // 且保留 m_trackedRect、m_offsetX、m_offsetY 的上一次值，起到防抖和惯性预测的作用。
    }


}

void TrackingEngine::onYoloDetectionResult(
    const cv::Rect2d &rect,
    int classId,
    float confidence,
    quint64 requestId,
    bool finished)
{
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    if (m_featureYoloClassifyPending &&
        requestId == m_featureYoloClassifyRequestId &&
        !m_featureYoloClassifyFrame.empty()) {
        if (!finished) {
            const cv::Rect candidate = boundedIntRect(
                rect,
                cv::Size(m_featureYoloClassifyFrame.cols, m_featureYoloClassifyFrame.rows));
            if (candidate.width <= 0 || candidate.height <= 0) {
                return;
            }

            const cv::Rect selected = boundedIntRect(m_selectedRect, m_featureYoloClassifyFrame.size());
            const cv::Rect intersection = candidate & selected;
            const double intersectionArea = std::max(0, intersection.area());
            const double selectedArea = std::max(1, selected.area());
            const double candidateArea = std::max(1, candidate.area());
            const double selectionCoverage = intersectionArea / selectedArea;
            const double candidateCoverage = intersectionArea / candidateArea;
            const double sizeSimilarity =
                std::min(selectedArea, candidateArea) / std::max(selectedArea, candidateArea);
            const double confidenceScore = std::clamp(static_cast<double>(confidence), 0.0, 1.0);
            const double score =
                selectionCoverage * 2.4 +
                candidateCoverage * 2.0 +
                sizeSimilarity * 1.6 +
                confidenceScore * 0.8;

            const bool selectionMatchesCandidate =
                selectionCoverage >= kFeatureYoloClassifyMinSelectedCoverage &&
                candidateCoverage >= kFeatureYoloClassifyMinCandidateCoverage &&
                sizeSimilarity >= kFeatureYoloClassifyMinSizeSimilarity;
            if (selectionMatchesCandidate && score > m_featureYoloClassifyBestScore) {
                m_featureYoloClassifyBestScore = score;
                m_featureYoloClassifyBestClassId = classId;
                m_featureYoloClassifyBestConfidence = confidence;
                m_featureYoloClassifyBestBox = candidate;
            }
            return;
        }

        m_featureYoloClassifyPending = false;
        m_featureYoloClassKnown = true;
        if (m_featureYoloClassifyBestClassId >= 0 &&
            m_featureYoloClassifyBestBox.width > 0 &&
            m_featureYoloClassifyBestBox.height > 0) {
            m_featureUseYoloRecovery = true;
            m_featureTargetYoloClassId = m_featureYoloClassifyBestClassId;
            m_featureTargetYoloBox = m_featureYoloClassifyBestBox;

            const cv::Rect selected = boundedIntRect(m_selectedRect, m_featureYoloClassifyFrame.size());
            m_featureYoloRelX =
                (selected.x - m_featureTargetYoloBox.x) /
                std::max(1.0, static_cast<double>(m_featureTargetYoloBox.width));
            m_featureYoloRelY =
                (selected.y - m_featureTargetYoloBox.y) /
                std::max(1.0, static_cast<double>(m_featureTargetYoloBox.height));
            m_featureYoloRelW =
                selected.width / std::max(1.0, static_cast<double>(m_featureTargetYoloBox.width));
            m_featureYoloRelH =
                selected.height / std::max(1.0, static_cast<double>(m_featureTargetYoloBox.height));

            static const char* classNames[] = {
                "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
                "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
                "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
                "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle",
                "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
                "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch", "potted plant", "bed",
                "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave", "oven",
                "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"
            };
            if (m_featureTargetYoloClassId >= 0 &&
                m_featureTargetYoloClassId < static_cast<int>(std::size(classNames))) {
                m_featureTargetYoloClassName =
                    QString::fromLatin1(classNames[m_featureTargetYoloClassId]);
            } else {
                m_featureTargetYoloClassName = QStringLiteral("unknown");
            }

            emit logMessage(
                QStringLiteral("【系统】框选目标属于 YOLO 类别：%1。重捕策略：YOLO 检测该类别 + 初始外观校验。")
                    .arg(m_featureTargetYoloClassName));
        } else {
            m_featureUseYoloRecovery = false;
            m_featureTargetYoloClassId = -1;
            m_featureTargetYoloClassName.clear();
            emit logMessage(
                QStringLiteral("【系统】框选目标不属于当前 YOLO 模型可稳定识别的类别。重捕策略：ORB/模板/颜色传统特征重捕。"));
        }
        m_featureYoloClassifyFrame.release();
        return;
    }

    if (!m_featureYoloRecoveryPending ||
        requestId != m_featureYoloRecoveryRequestId ||
        m_featureYoloRecoveryFrame.empty()) {
        return;
    }

    if (!finished) {
        if (m_featureYoloCandidatesEvaluated >= kMaxYoloAppearanceCandidatesPerFrame ||
            confidence < confidenceThresholdForClass(classId, m_featureTargetYoloClassId) ||
            (m_featureTargetYoloClassId >= 0 && classId != m_featureTargetYoloClassId)) {
            return;
        }
        const cv::Rect candidate = boundedIntRect(
            rect,
            cv::Size(m_featureYoloRecoveryFrame.cols, m_featureYoloRecoveryFrame.rows));
        if (candidate.width < 12 || candidate.height < 12) {
            return;
        }
        m_featureYoloCandidatesEvaluated++;
        double appearanceScore = 0.0;
        cv::Rect alignedCandidate;
        if (evaluateYoloFeatureCandidate(
                m_featureYoloRecoveryFrame,
                candidate,
                alignedCandidate,
                &appearanceScore)) {
            const double confidenceScore = std::clamp(static_cast<double>(confidence), 0.0, 1.0);
            const double score = appearanceScore * 0.82 + confidenceScore * 0.18;
            if (score > m_featureYoloBestScore) {
                m_featureYoloBestScore = score;
                if (m_featureTargetYoloBox.width > 0 && m_featureTargetYoloBox.height > 0) {
                    const cv::Rect2d relAligned(
                        candidate.x + m_featureYoloRelX * candidate.width,
                        candidate.y + m_featureYoloRelY * candidate.height,
                        candidate.width * m_featureYoloRelW,
                        candidate.height * m_featureYoloRelH);
                    const cv::Rect relativeCandidate =
                        boundedIntRect(relAligned, m_featureYoloRecoveryFrame.size());
                    double relativeScore = 0.0;
                    const bool relativeOk =
                        relativeCandidate.width >= 8 &&
                        relativeCandidate.height >= 8 &&
                        featureCandidatePassesYoloRecovery(
                            m_featureYoloRecoveryFrame,
                            relativeCandidate,
                            &relativeScore);
                    m_featureYoloBestCandidate =
                        (relativeOk && relativeScore >= appearanceScore * 0.88)
                            ? relativeCandidate
                            : alignedCandidate;
                } else {
                    m_featureYoloBestCandidate = alignedCandidate;
                }
                m_featureYoloBestClassId = classId;
                m_featureYoloBestConfidence = confidence;
            }
        }
        return;
    }

    m_featureYoloRecoveryPending = false;
    if (m_featureYoloBestCandidate.width <= 0 ||
        m_featureYoloBestCandidate.height <= 0) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastFeatureYoloRecoveryLogTime > 1200) {
            m_lastFeatureYoloRecoveryLogTime = now;
            emit logMessage(
                QStringLiteral("【系统】YOLO 已检测到候选，但与初始目标外观不够一致，继续等待。"));
        }
        m_featureYoloRecoveryFrame.release();
        m_featureYoloBestCandidate = cv::Rect();
        onDnnResultReceived(cv::Rect2d(), false, QStringLiteral("FEATURE"));
        return;
    }

    const QString methodName = QStringLiteral("YOLO 候选+外观校验");
    if (!confirmFeatureRecoveryCandidate(
            m_featureYoloRecoveryFrame,
            m_featureYoloBestCandidate,
            methodName,
            m_featureYoloBestScore)) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastFeatureYoloRecoveryLogTime > 1200) {
            m_lastFeatureYoloRecoveryLogTime = now;
            emit logMessage(
                QStringLiteral("【系统】YOLO 候选已通过外观校验，正在连续确认..."));
        }
        m_featureYoloRecoveryFrame.release();
        onDnnResultReceived(cv::Rect2d(), false, QStringLiteral("FEATURE"));
        return;
    }

    cv::Mat recoveryFrame = m_featureYoloRecoveryFrame.clone();
    const cv::Rect recoveryRect = m_featureYoloBestCandidate;
    m_featureYoloRecoveryFrame.release();
    m_featureYoloBestCandidate = cv::Rect();
    restartFeatureTrackerFromRect(recoveryFrame, recoveryRect, methodName);
}

void TrackingEngine::restartDnnThread(const QString &modelFileName)
{
    m_isCapturing = false;
    m_isTargetTracked = false;
    m_trackedRect = cv::Rect2d();
    m_offsetX = 0;
    m_offsetY = 0;
    resetFeatureTracker();

    DnnThread *thread = ensureDnnThread(modelFileName);
    if (m_dnnThread && m_dnnThread != thread) {
        m_dnnThread->stopDnn();
    }
    m_dnnThread = thread;
    if (thread && !thread->isNetEmpty() && thread->isWarmedUp()) {
        emit logMessage(
            QStringLiteral("【系统】已切换到已预热模型：%1").arg(thread->modelFileName()));
    } else if (thread && !thread->isNetEmpty()) {
        emit logMessage(
            QStringLiteral("【系统】已切换到模型：%1，正在等待预热完成。").arg(modelFileName));
    } else {
        emit logMessage(
            QStringLiteral("【警告】YOLO 模型加载失败：%1").arg(modelFileName));
    }
}

void TrackingEngine::preloadYoloModels()
{
    ensureDnnThread(QStringLiteral("yolo26s.onnx"));
    ensureDnnThread(QStringLiteral("yolo26n.onnx"));
    emit logMessage(
        QStringLiteral("【系统】已启动 YOLO26s 与 YOLO26n 后台预热。"));
}

DnnThread *TrackingEngine::ensureDnnThread(const QString &modelFileName)
{
    const QString key = modelFileName.isEmpty()
                            ? QStringLiteral("yolo26s.onnx")
                            : modelFileName;
    if (DnnThread *existingThread = m_dnnThreads.value(key, nullptr)) {
        return existingThread;
    }

    emit logMessage(
        QStringLiteral("【系统】正在后台加载并预热 YOLO 模型：%1").arg(key));

    DnnThread *thread = new DnnThread(key, this);
    m_dnnThreads.insert(key, thread);
    // 这些回调只处理引擎状态，由 m_stateMutex 保护，不访问 QWidget。
    // 在推理线程处理，避免 GUI 等待 CSRT 占用的状态锁或执行外观重捕。
    // DnnThread 必须先释放输入锁再发结果，保持 state -> input 的锁顺序。
    connect(thread, &DnnThread::dnnTrackedResult,
            this, &TrackingEngine::onDnnResultReceived, Qt::DirectConnection);
    connect(thread, &DnnThread::yoloDetectionResult,
            this, &TrackingEngine::onYoloDetectionResult, Qt::DirectConnection);
    connect(thread, &DnnThread::dnnWarmupFinished, this,
            [this, thread](bool success, const QString &message) {
        emit logMessage(
            QString(success ? "【系统】%1" : "【警告】%1").arg(message));
    });
    // 推理结果直接影响云台控制，低优先级会在相机/UI繁忙时增加结果年龄。
    thread->start(QThread::NormalPriority);
    return thread;
}

void TrackingEngine::resetFeatureTracker()
{
    m_featureTracker.release();
    m_featureTrackerReady = false;
    m_featureTemplate.release();
    m_featureColorHist.release();
    m_featureReferenceKeypoints.clear();
    m_featureReferenceDescriptors.release();
    m_featureReferenceSize = cv::Size();
    m_featureLastRect = cv::Rect2d();
    m_lastFeatureRecoveryAttemptTime = 0;
    resetFeatureRecoveryCandidate();
    m_featureUnreliableCount = 0;
}

bool TrackingEngine::initFeatureTracker(const cv::Mat &frame, const cv::Rect2d &target)
{
    resetFeatureTracker();
    if (frame.empty() || target.width <= 2 || target.height <= 2) {
        return false;
    }

    const cv::Rect boundedTarget = boundedIntRect(target, frame.size());
    if (boundedTarget.width <= 2 || boundedTarget.height <= 2) {
        return false;
    }

    try {
        m_featureTracker = cv::TrackerCSRT::create();
        m_featureTracker->init(frame, boundedTarget);
        m_featureTrackerReady = true;
        m_trackedRect = cv::Rect2d(
            boundedTarget.x, boundedTarget.y, boundedTarget.width, boundedTarget.height);
        m_featureLastRect = m_trackedRect;
        m_featureUnreliableCount = 0;
        buildFeatureReference(frame, boundedTarget);
        m_isTargetTracked = true;
        emit logMessage(
            QStringLiteral("【系统】特征跟踪算法：CSRT 主跟踪；重捕策略将按 YOLO 类别判定自动分流。"));
        return true;
    } catch (const cv::Exception &e) {
        emit logMessage(
            QStringLiteral("【警告】特征跟踪初始化失败：%1").arg(QString::fromLocal8Bit(e.what())));
    } catch (...) {
        emit logMessage(QStringLiteral("【警告】特征跟踪初始化失败。"));
    }

    resetFeatureTracker();
    return false;
}

bool TrackingEngine::updateFeatureTracker(const cv::Mat &frame)
{
    if (!m_featureTrackerReady || !m_featureTracker || frame.empty()) {
        if (!recoverFeatureTracker(frame)) {
            onDnnResultReceived(cv::Rect2d(), false, QStringLiteral("FEATURE"));
            return false;
        }
        return true;
    }

    cv::Rect trackedRect;
    bool ok = false;
    try {
        ok = m_featureTracker->update(frame, trackedRect);
    } catch (const cv::Exception &e) {
        emit logMessage(
            QStringLiteral("【警告】特征跟踪更新失败：%1").arg(QString::fromLocal8Bit(e.what())));
        ok = false;
    } catch (...) {
        ok = false;
    }

    if (!ok || trackedRect.width <= 0 || trackedRect.height <= 0) {
        m_featureTrackerReady = false;
        if (!recoverFeatureTracker(frame)) {
            onDnnResultReceived(cv::Rect2d(), false, QStringLiteral("FEATURE"));
            return false;
        }
        return true;
    }

    cv::Rect boundedRect = trackedRect & cv::Rect(0, 0, frame.cols, frame.rows);
    if (boundedRect.width <= 0 || boundedRect.height <= 0) {
        m_featureTrackerReady = false;
        if (!recoverFeatureTracker(frame)) {
            onDnnResultReceived(cv::Rect2d(), false, QStringLiteral("FEATURE"));
            return false;
        }
        return true;
    }

    double candidateScore = 0.0;
    const bool candidateOk = featureCandidatePasses(
        frame, boundedRect, false, &candidateScore);
    if (!m_featureTemplate.empty() && !candidateOk) {
        m_featureUnreliableCount++;
        if (candidateScore < kFeatureHardRejectSimilarity ||
            m_featureUnreliableCount >= kFeatureUnreliableLimit) {
            m_featureTracker.release();
            m_featureTrackerReady = false;
            if (!recoverFeatureTracker(frame)) {
                onDnnResultReceived(cv::Rect2d(), false, QStringLiteral("FEATURE"));
                return false;
            }
            return true;
        }

        onDnnResultReceived(m_featureLastRect, true, QStringLiteral("FEATURE"));
        return true;
    } else {
        m_featureUnreliableCount = 0;
    }

    m_featureLastRect = cv::Rect2d(
        boundedRect.x, boundedRect.y, boundedRect.width, boundedRect.height);
    onDnnResultReceived(
        m_featureLastRect,
        true,
        QStringLiteral("FEATURE"));
    return true;
}

bool TrackingEngine::recoverFeatureTracker(const cv::Mat &frame)
{
    if (frame.empty()) {
        return false;
    }

    // 丢帧期间不要在每一张画面上重复执行整幅 ORB/多尺度模板搜索。
    // 这些搜索在主线程中运行，连续触发会把相机帧和界面一起拖住。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastFeatureRecoveryAttemptTime < kFeatureRecoveryIntervalMs) {
        return false;
    }
    m_lastFeatureRecoveryAttemptTime = now;

    auto logRecoveryStrategy = [this](const QString &message) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (message != m_lastFeatureRecoveryStrategyLog ||
            now - m_lastFeatureRecoveryStrategyLogTime > kFeatureRecoveryStrategyLogIntervalMs) {
            m_lastFeatureRecoveryStrategyLog = message;
            m_lastFeatureRecoveryStrategyLogTime = now;
            emit logMessage(message);
        }
    };

    if (m_featureUseYoloRecovery) {
        if (m_dnnThread &&
            !m_dnnThread->isNetEmpty() &&
            m_dnnThread->isWarmedUp()) {
            logRecoveryStrategy(
                QStringLiteral("【系统】正在按 YOLO 类别 %1 重捕：只接受同类别检测框，并通过初始外观校验后重新锁定。")
                    .arg(m_featureTargetYoloClassName.isEmpty()
                             ? QStringLiteral("unknown")
                             : m_featureTargetYoloClassName));
            requestFeatureYoloRecovery(frame);
            return false;
        }

        logRecoveryStrategy(
            QStringLiteral("【系统】目标已判定为 YOLO 类别 %1，但 YOLO 辅助暂未就绪；等待 YOLO 恢复，不使用传统特征抢锁。")
                .arg(m_featureTargetYoloClassName.isEmpty()
                         ? QStringLiteral("unknown")
                         : m_featureTargetYoloClassName));
        return false;
    }

    logRecoveryStrategy(
        m_featureYoloClassKnown
            ? QStringLiteral("【系统】正在按传统特征重捕：ORB 特征匹配 + 多尺度模板匹配 + 颜色直方图。")
            : QStringLiteral("【系统】YOLO 类别判定尚未完成；当前暂按传统特征重捕，判定完成后会自动切换策略。"));

    struct RecoveryCandidate {
        cv::Rect rect;
        QString methodName;
        double score = 0.0;
    };

    std::vector<RecoveryCandidate> candidates;
    auto addCandidate = [&](const cv::Rect &rect, const QString &methodName) {
        double score = 0.0;
        if (featureCandidatePasses(frame, rect, true, &score)) {
            candidates.push_back({rect, methodName, score});
        }
    };

    cv::Rect recoveredRect;
    if (recoverFeatureTrackerByOrb(frame, recoveredRect)) {
        addCandidate(recoveredRect, QStringLiteral("ORB 特征匹配"));
    }
    if (recoverFeatureTrackerByTemplate(frame, recoveredRect)) {
        addCandidate(recoveredRect, QStringLiteral("多尺度模板匹配"));
    }
    if (recoverFeatureTrackerByColor(frame, recoveredRect)) {
        addCandidate(recoveredRect, QStringLiteral("颜色直方图"));
    }

    if (candidates.empty()) {
        resetFeatureRecoveryCandidate();
        return false;
    }

    const auto bestIt = std::max_element(
        candidates.begin(),
        candidates.end(),
        [](const RecoveryCandidate &a, const RecoveryCandidate &b) {
            return a.score < b.score;
        });
    if (bestIt == candidates.end()) {
        resetFeatureRecoveryCandidate();
        return false;
    }

    if (!confirmFeatureRecoveryCandidate(
            frame, bestIt->rect, bestIt->methodName, bestIt->score)) {
        onDnnResultReceived(cv::Rect2d(), false, QStringLiteral("FEATURE"));
        return false;
    }

    return restartFeatureTrackerFromRect(frame, bestIt->rect, bestIt->methodName);
}

void TrackingEngine::buildFeatureReference(const cv::Mat &frame, const cv::Rect &target)
{
    m_featureTemplate.release();
    m_featureColorHist.release();
    m_featureReferenceKeypoints.clear();
    m_featureReferenceDescriptors.release();
    m_featureReferenceSize = cv::Size();

    const cv::Rect boundedTarget = target & cv::Rect(0, 0, frame.cols, frame.rows);
    if (frame.empty() || boundedTarget.width < 8 || boundedTarget.height < 8) {
        return;
    }

    const cv::Mat targetPatch = frame(boundedTarget);
    m_featureTemplate = featureMatchImage(targetPatch);
    m_featureReferenceSize = boundedTarget.size();

    try {
        cv::Ptr<cv::ORB> orb = cv::ORB::create(700);
        orb->detectAndCompute(
            m_featureTemplate,
            cv::noArray(),
            m_featureReferenceKeypoints,
            m_featureReferenceDescriptors);
    } catch (...) {
        m_featureReferenceKeypoints.clear();
        m_featureReferenceDescriptors.release();
    }

    try {
        cv::Mat hsv;
        cv::cvtColor(targetPatch, hsv, cv::COLOR_BGR2HSV);
        const int hBins = 30;
        const int sBins = 32;
        const int histSize[] = {hBins, sBins};
        const float hRange[] = {0.0f, 180.0f};
        const float sRange[] = {0.0f, 256.0f};
        const float* ranges[] = {hRange, sRange};
        const int channels[] = {0, 1};
        cv::calcHist(&hsv, 1, channels, cv::Mat(), m_featureColorHist, 2, histSize, ranges);
        cv::normalize(m_featureColorHist, m_featureColorHist, 0.0, 1.0, cv::NORM_MINMAX);
    } catch (...) {
        m_featureColorHist.release();
    }
}

double TrackingEngine::featurePatchSimilarity(const cv::Mat &frame, const cv::Rect &target) const
{
    if (frame.empty() || m_featureTemplate.empty()) {
        return 0.0;
    }

    const cv::Rect boundedTarget = target & cv::Rect(0, 0, frame.cols, frame.rows);
    if (boundedTarget.width < 8 || boundedTarget.height < 8) {
        return 0.0;
    }

    cv::Mat sourcePatch = frame(boundedTarget);
    cv::Mat compactPatch;
    if (std::max(sourcePatch.cols, sourcePatch.rows) > kFeatureCompareMaxSide) {
        const double scale =
            kFeatureCompareMaxSide /
            static_cast<double>(std::max(sourcePatch.cols, sourcePatch.rows));
        cv::resize(sourcePatch, compactPatch, cv::Size(), scale, scale, cv::INTER_AREA);
        sourcePatch = compactPatch;
    }

    cv::Mat patch = featureMatchImage(sourcePatch);
    if (patch.empty()) {
        return 0.0;
    }

    cv::Mat resizedPatch;
    try {
        cv::resize(patch, resizedPatch, m_featureTemplate.size(), 0, 0, cv::INTER_AREA);
        cv::Mat result;
        cv::matchTemplate(resizedPatch, m_featureTemplate, result, cv::TM_CCOEFF_NORMED);
        double maxVal = 0.0;
        cv::minMaxLoc(result, nullptr, &maxVal, nullptr, nullptr);
        return std::isfinite(maxVal) ? maxVal : 0.0;
    } catch (...) {
        return 0.0;
    }
}

double TrackingEngine::featureColorSimilarity(const cv::Mat &frame, const cv::Rect &target) const
{
    if (frame.empty() || m_featureColorHist.empty()) {
        return 0.0;
    }

    const cv::Rect boundedTarget = target & cv::Rect(0, 0, frame.cols, frame.rows);
    if (boundedTarget.width < 8 || boundedTarget.height < 8) {
        return 0.0;
    }

    try {
        cv::Mat hsv;
        cv::Mat hist;
        cv::Mat sourcePatch = frame(boundedTarget);
        cv::Mat compactPatch;
        if (std::max(sourcePatch.cols, sourcePatch.rows) > kFeatureCompareMaxSide) {
            const double scale =
                kFeatureCompareMaxSide /
                static_cast<double>(std::max(sourcePatch.cols, sourcePatch.rows));
            cv::resize(sourcePatch, compactPatch, cv::Size(), scale, scale, cv::INTER_AREA);
            sourcePatch = compactPatch;
        }
        cv::cvtColor(sourcePatch, hsv, cv::COLOR_BGR2HSV);
        const int hBins = 30;
        const int sBins = 32;
        const int histSize[] = {hBins, sBins};
        const float hRange[] = {0.0f, 180.0f};
        const float sRange[] = {0.0f, 256.0f};
        const float* ranges[] = {hRange, sRange};
        const int channels[] = {0, 1};
        cv::calcHist(&hsv, 1, channels, cv::Mat(), hist, 2, histSize, ranges);
        cv::normalize(hist, hist, 0.0, 1.0, cv::NORM_MINMAX);
        const double score = cv::compareHist(m_featureColorHist, hist, cv::HISTCMP_CORREL);
        return std::isfinite(score) ? std::clamp(score, 0.0, 1.0) : 0.0;
    } catch (...) {
        return 0.0;
    }
}

double TrackingEngine::featureShapeSimilarity(const cv::Rect &target) const
{
    if (target.width < 8 || target.height < 8 ||
        m_featureReferenceSize.width < 8 || m_featureReferenceSize.height < 8) {
        return 0.0;
    }

    const double widthRatio =
        std::min(static_cast<double>(target.width), static_cast<double>(m_featureReferenceSize.width)) /
        std::max(static_cast<double>(target.width), static_cast<double>(m_featureReferenceSize.width));
    const double heightRatio =
        std::min(static_cast<double>(target.height), static_cast<double>(m_featureReferenceSize.height)) /
        std::max(static_cast<double>(target.height), static_cast<double>(m_featureReferenceSize.height));
    const double aspect = static_cast<double>(target.width) / std::max(1, target.height);
    const double referenceAspect =
        static_cast<double>(m_featureReferenceSize.width) / std::max(1, m_featureReferenceSize.height);
    const double aspectRatio = std::min(aspect, referenceAspect) / std::max(aspect, referenceAspect);
    return std::clamp(widthRatio * 0.35 + heightRatio * 0.35 + aspectRatio * 0.30, 0.0, 1.0);
}

double TrackingEngine::featureCandidateScore(
    const cv::Mat &frame,
    const cv::Rect &target,
    double *templateScore,
    double *colorScore,
    double *shapeScore) const
{
    const double tScore = featurePatchSimilarity(frame, target);
    const double cScore = featureColorSimilarity(frame, target);
    const double sScore = featureShapeSimilarity(target);
    if (templateScore) *templateScore = tScore;
    if (colorScore) *colorScore = cScore;
    if (shapeScore) *shapeScore = sScore;
    return std::clamp(tScore * 0.55 + cScore * 0.30 + sScore * 0.15, 0.0, 1.0);
}

cv::Rect TrackingEngine::alignFeatureRectInYoloCandidate(const cv::Rect &yoloRect, const cv::Size &frameSize) const
{
    if (yoloRect.width <= 0 || yoloRect.height <= 0 || frameSize.width <= 0 || frameSize.height <= 0) {
        return cv::Rect();
    }

    cv::Rect2d aligned(yoloRect.x, yoloRect.y, yoloRect.width, yoloRect.height);

    if (m_featureReferenceSize.width > 0 && m_featureReferenceSize.height > 0) {
        const double targetAspect =
            static_cast<double>(m_featureReferenceSize.width) / std::max(1, m_featureReferenceSize.height);
        const double yoloAspect =
            static_cast<double>(yoloRect.width) / std::max(1, yoloRect.height);

        if (targetAspect > yoloAspect) {
            const double h = yoloRect.width / targetAspect;
            aligned = cv::Rect2d(
                yoloRect.x,
                yoloRect.y + (yoloRect.height - h) * 0.5,
                yoloRect.width,
                h);
        } else {
            const double w = yoloRect.height * targetAspect;
            aligned = cv::Rect2d(
                yoloRect.x + (yoloRect.width - w) * 0.5,
                yoloRect.y,
                w,
                yoloRect.height);
        }
    }

    return boundedIntRect(aligned, frameSize);
}

bool TrackingEngine::evaluateYoloFeatureCandidate(
    const cv::Mat &frame,
    const cv::Rect &yoloRect,
    cv::Rect &alignedRect,
    double *candidateScore) const
{
    alignedRect = cv::Rect();
    if (frame.empty() || yoloRect.width < 8 || yoloRect.height < 8) {
        return false;
    }

    std::vector<cv::Rect> candidates;
    candidates.push_back(yoloRect & cv::Rect(0, 0, frame.cols, frame.rows));

    const cv::Rect aspectAligned = alignFeatureRectInYoloCandidate(yoloRect, frame.size());
    if (aspectAligned.width >= 8 && aspectAligned.height >= 8) {
        candidates.push_back(aspectAligned);
    }

    const double widthScales[] = {0.65, 0.85};
    const double heightScales[] = {0.65, 0.85};
    for (double ws : widthScales) {
        for (double hs : heightScales) {
            const int w = cvRound(yoloRect.width * ws);
            const int h = cvRound(yoloRect.height * hs);
            if (w < 8 || h < 8) continue;
            const int xOffset = (yoloRect.width - w) / 2;
            const double yAnchors[] = {0.25, 0.50, 0.75};
            for (double anchor : yAnchors) {
                const int yOffset = cvRound((yoloRect.height - h) * anchor);
                const cv::Rect window(yoloRect.x + xOffset, yoloRect.y + yOffset, w, h);
                candidates.push_back(window & cv::Rect(0, 0, frame.cols, frame.rows));
            }
        }
    }

    double bestScore = 0.0;
    cv::Rect bestRect;
    for (const cv::Rect &candidate : candidates) {
        if (candidate.width < 8 || candidate.height < 8) continue;
        double templateScore = 0.0;
        double colorScore = 0.0;
        double shapeScore = 0.0;
        const double score = featureCandidateScore(
            frame, candidate, &templateScore, &colorScore, &shapeScore);
        if (templateScore < kFeatureYoloRecoverMinTemplateSimilarity ||
            colorScore < kFeatureYoloRecoverMinColorSimilarity) {
            continue;
        }
        const double yoloScore = score * 0.85 + shapeScore * 0.15;
        if (yoloScore > bestScore) {
            bestScore = yoloScore;
            bestRect = candidate;
        }
    }

    if (bestRect.width <= 0 ||
        bestRect.height <= 0 ||
        bestScore < kFeatureYoloRecoverMinCombinedScore) {
        return false;
    }

    alignedRect = bestRect;
    if (candidateScore) *candidateScore = bestScore;
    return true;
}

bool TrackingEngine::featureCandidatePasses(
    const cv::Mat &frame,
    const cv::Rect &target,
    bool recoveryMode,
    double *candidateScore) const
{
    double templateScore = 0.0;
    double colorScore = 0.0;
    double shapeScore = 0.0;
    const double score = featureCandidateScore(
        frame, target, &templateScore, &colorScore, &shapeScore);
    if (candidateScore) *candidateScore = score;

    if (target.width < 8 || target.height < 8 ||
        shapeScore < kFeatureMinShapeSimilarity) {
        return false;
    }

    if (recoveryMode) {
        return templateScore >= kFeatureRecoverMinTemplateSimilarity &&
               colorScore >= kFeatureRecoverMinColorSimilarity &&
               score >= kFeatureRecoverMinCombinedScore;
    }

    return templateScore >= kFeatureTrackMinTemplateSimilarity &&
           colorScore >= kFeatureTrackMinColorSimilarity &&
           score >= kFeatureTrackMinCombinedScore;
}

bool TrackingEngine::featureCandidatePassesYoloRecovery(
    const cv::Mat &frame,
    const cv::Rect &target,
    double *candidateScore) const
{
    double templateScore = 0.0;
    double colorScore = 0.0;
    double shapeScore = 0.0;
    const double score = featureCandidateScore(
        frame, target, &templateScore, &colorScore, &shapeScore);
    if (candidateScore) *candidateScore = score;

    return target.width >= 8 &&
           target.height >= 8 &&
           shapeScore >= kFeatureMinShapeSimilarity &&
           templateScore >= kFeatureYoloRecoverMinTemplateSimilarity &&
           colorScore >= kFeatureYoloRecoverMinColorSimilarity &&
           score >= kFeatureYoloRecoverMinCombinedScore;
}

bool TrackingEngine::confirmFeatureRecoveryCandidate(
    const cv::Mat &frame,
    const cv::Rect &target,
    const QString &methodName,
    double candidateScore)
{
    Q_UNUSED(candidateScore);

    const cv::Rect boundedTarget = target & cv::Rect(0, 0, frame.cols, frame.rows);
    if (boundedTarget.width < 8 || boundedTarget.height < 8) {
        resetFeatureRecoveryCandidate();
        return false;
    }

    bool sameCandidate = false;
    if (m_featureRecoveryCandidateRect.width > 0 &&
        m_featureRecoveryCandidateRect.height > 0) {
        const cv::Rect2d currentRect(
            boundedTarget.x, boundedTarget.y, boundedTarget.width, boundedTarget.height);
        const double currentCx = currentRect.x + currentRect.width * 0.5;
        const double currentCy = currentRect.y + currentRect.height * 0.5;
        const double lastCx = m_featureRecoveryCandidateRect.x + m_featureRecoveryCandidateRect.width * 0.5;
        const double lastCy = m_featureRecoveryCandidateRect.y + m_featureRecoveryCandidateRect.height * 0.5;
        const double centerDistance = std::hypot(currentCx - lastCx, currentCy - lastCy);
        const double maxCenterDistance = std::max(
            12.0,
            std::max(currentRect.width, currentRect.height) * 0.55);
        const double areaRatio =
            std::min(currentRect.area(), m_featureRecoveryCandidateRect.area()) /
            std::max(1.0, std::max(currentRect.area(), m_featureRecoveryCandidateRect.area()));
        sameCandidate =
            centerDistance <= maxCenterDistance &&
            areaRatio >= 0.55 &&
            methodName == m_featureRecoveryCandidateMethod;
    }

    if (sameCandidate) {
        m_featureRecoveryCandidateCount++;
    } else {
        m_featureRecoveryCandidateRect = cv::Rect2d(
            boundedTarget.x, boundedTarget.y, boundedTarget.width, boundedTarget.height);
        m_featureRecoveryCandidateMethod = methodName;
        m_featureRecoveryCandidateCount = 1;
        emit logMessage(
            kFeatureRecoveryConfirmFrames <= 1
                ? QStringLiteral("【系统】发现疑似目标，正在重新锁定...")
                : QStringLiteral("【系统】发现疑似目标，正在连续确认..."));
    }

    m_featureRecoveryCandidateRect = cv::Rect2d(
        boundedTarget.x, boundedTarget.y, boundedTarget.width, boundedTarget.height);
    return m_featureRecoveryCandidateCount >= kFeatureRecoveryConfirmFrames;
}

void TrackingEngine::requestFeatureYoloTargetClassification(const cv::Mat &frame)
{
    resetFeatureYoloTargetClassification();
    if (frame.empty()) {
        m_featureUseYoloRecovery = false;
        emit logMessage(
            QStringLiteral("【系统】当前画面不可用，暂时无法判断框选目标是否属于 YOLO 类别；重捕先按传统特征处理。"));
        return;
    }

    if (!m_dnnThread || m_dnnThread->isNetEmpty()) {
        m_featureYoloClassKnown = true;
        m_featureUseYoloRecovery = false;
        emit logMessage(
            QStringLiteral("【系统】YOLO 辅助未加载：框选目标按非 YOLO 目标处理。重捕策略：ORB/模板/颜色传统特征重捕。"));
        return;
    }

    if (!m_dnnThread->isWarmedUp()) {
        m_featureUseYoloRecovery = false;
        emit logMessage(
            QStringLiteral("【系统】YOLO 辅助正在预热，框选目标暂未完成类别判断；开始跟踪或预热完成后会重试，当前先按传统特征重捕。"));
        return;
    }

    m_featureYoloClassifyPending = true;
    m_featureYoloClassifyRequestId = nextFeatureYoloRequestId();
    m_featureYoloClassifyFrame = frame.clone();
    m_dnnThread->requestDetections(m_featureYoloClassifyFrame, m_featureYoloClassifyRequestId);
    emit logMessage(
        QStringLiteral("【系统】正在判断框选目标是否属于 YOLO 模型类别..."));
}

void TrackingEngine::requestFeatureYoloRecovery(const cv::Mat &frame)
{
    if (frame.empty() ||
        !m_dnnThread ||
        m_dnnThread->isNetEmpty() ||
        !m_dnnThread->isWarmedUp() ||
        m_featureYoloRecoveryPending) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastFeatureYoloRecoveryRequestTime < kFeatureYoloRecoveryIntervalMs) {
        return;
    }
    m_lastFeatureYoloRecoveryRequestTime = now;

    m_featureYoloRecoveryPending = true;
    m_featureYoloRecoveryRequestId = nextFeatureYoloRequestId();
    m_featureYoloRecoveryFrame = frame.clone();
    m_featureYoloBestCandidate = cv::Rect();
    m_featureYoloBestScore = 0.0;
    m_featureYoloBestClassId = -1;
    m_featureYoloBestConfidence = 0.0f;
    m_featureYoloCandidatesEvaluated = 0;
    m_dnnThread->requestDetections(m_featureYoloRecoveryFrame, m_featureYoloRecoveryRequestId);
}

quint64 TrackingEngine::nextFeatureYoloRequestId()
{
    ++m_featureYoloNextRequestId;
    if (m_featureYoloNextRequestId == 0) {
        m_featureYoloNextRequestId = 1;
    }
    return m_featureYoloNextRequestId;
}

void TrackingEngine::resetFeatureRecoveryCandidate()
{
    m_featureRecoveryCandidateRect = cv::Rect2d();
    m_featureRecoveryCandidateMethod.clear();
    m_featureRecoveryCandidateCount = 0;
    m_featureYoloRecoveryPending = false;
    m_featureYoloRecoveryFrame.release();
    m_featureYoloBestCandidate = cv::Rect();
    m_featureYoloBestScore = 0.0;
    m_featureYoloBestClassId = -1;
    m_featureYoloBestConfidence = 0.0f;
    m_featureYoloCandidatesEvaluated = 0;
}

void TrackingEngine::resetFeatureYoloTargetClassification()
{
    m_featureYoloClassifyPending = false;
    m_featureYoloClassifyFrame.release();
    m_featureYoloClassKnown = false;
    m_featureUseYoloRecovery = false;
    m_featureTargetYoloClassId = -1;
    m_featureTargetYoloClassName.clear();
    m_featureTargetYoloBox = cv::Rect();
    m_featureYoloRelX = 0.0;
    m_featureYoloRelY = 0.0;
    m_featureYoloRelW = 1.0;
    m_featureYoloRelH = 1.0;
    m_featureYoloClassifyBestClassId = -1;
    m_featureYoloClassifyBestConfidence = 0.0f;
    m_featureYoloClassifyBestScore = 0.0;
    m_featureYoloClassifyBestBox = cv::Rect();
}

bool TrackingEngine::recoverFeatureTrackerByOrb(const cv::Mat &frame, cv::Rect &recoveredRect) const
{
    recoveredRect = cv::Rect();
    if (frame.empty() ||
        m_featureReferenceDescriptors.empty() ||
        m_featureReferenceKeypoints.size() < static_cast<size_t>(kOrbMinGoodMatches) ||
        m_featureReferenceSize.width < 8 ||
        m_featureReferenceSize.height < 8) {
        return false;
    }

    const cv::Rect searchRect(0, 0, frame.cols, frame.rows);
    const cv::Mat searchImage = featureMatchImage(frame(searchRect));

    std::vector<cv::KeyPoint> searchKeypoints;
    cv::Mat searchDescriptors;
    try {
        cv::Ptr<cv::ORB> orb = cv::ORB::create(1800);
        orb->detectAndCompute(
            searchImage,
            cv::noArray(),
            searchKeypoints,
            searchDescriptors);
    } catch (...) {
        return false;
    }

    if (searchDescriptors.empty() ||
        searchKeypoints.size() < static_cast<size_t>(kOrbMinGoodMatches)) {
        return false;
    }

    std::vector<std::vector<cv::DMatch>> knnMatches;
    try {
        cv::BFMatcher matcher(cv::NORM_HAMMING);
        matcher.knnMatch(m_featureReferenceDescriptors, searchDescriptors, knnMatches, 2);
    } catch (...) {
        return false;
    }

    std::vector<cv::DMatch> goodMatches;
    for (const auto &matchPair : knnMatches) {
        if (matchPair.size() < 2) continue;
        if (matchPair[0].distance < 0.76f * matchPair[1].distance &&
            matchPair[0].distance < 70.0f) {
            goodMatches.push_back(matchPair[0]);
        }
    }

    if (goodMatches.size() < static_cast<size_t>(kOrbMinGoodMatches)) {
        return false;
    }

    std::vector<double> dxValues;
    std::vector<double> dyValues;
    dxValues.reserve(goodMatches.size());
    dyValues.reserve(goodMatches.size());

    for (const cv::DMatch &match : goodMatches) {
        const cv::Point2f srcPoint = m_featureReferenceKeypoints[match.queryIdx].pt;
        const cv::Point2f dstPoint = searchKeypoints[match.trainIdx].pt;
        dxValues.push_back(dstPoint.x - srcPoint.x);
        dyValues.push_back(dstPoint.y - srcPoint.y);
    }

    const double medianDx = medianValue(dxValues);
    const double medianDy = medianValue(dyValues);

    int inliers = 0;
    for (const cv::DMatch &match : goodMatches) {
        const cv::Point2f srcPoint = m_featureReferenceKeypoints[match.queryIdx].pt;
        const cv::Point2f dstPoint = searchKeypoints[match.trainIdx].pt;
        const double dx = dstPoint.x - srcPoint.x;
        const double dy = dstPoint.y - srcPoint.y;
        if (std::abs(dx - medianDx) <= std::max(8.0, m_featureReferenceSize.width * 0.25) &&
            std::abs(dy - medianDy) <= std::max(8.0, m_featureReferenceSize.height * 0.25)) {
            inliers++;
        }
    }

    if (inliers < kOrbMinInliers) {
        return false;
    }

    recoveredRect = cv::Rect(
        searchRect.x + cvRound(medianDx),
        searchRect.y + cvRound(medianDy),
        m_featureReferenceSize.width,
        m_featureReferenceSize.height) & cv::Rect(0, 0, frame.cols, frame.rows);

    if (recoveredRect.width < 8 || recoveredRect.height < 8) {
        return false;
    }

    return true;
}

bool TrackingEngine::recoverFeatureTrackerByTemplate(const cv::Mat &frame, cv::Rect &recoveredRect) const
{
    recoveredRect = cv::Rect();
    if (frame.empty() || m_featureTemplate.empty() ||
        m_featureTemplate.cols < 8 || m_featureTemplate.rows < 8 ||
        m_featureTemplate.cols >= frame.cols || m_featureTemplate.rows >= frame.rows) {
        return false;
    }

    const cv::Rect searchRect(0, 0, frame.cols, frame.rows);
    const cv::Mat searchImage = featureMatchImage(frame(searchRect));

    double bestScore = -1.0;
    double bestTemplateScore = 0.0;
    double bestColorScore = 0.0;
    cv::Rect bestRect;
    const double scales[] = {0.55, 0.70, 0.85, 1.00, 1.15, 1.30, 1.50, 1.75};
    constexpr int kCandidatesPerScale = 4;
    for (double scale : scales) {
        const int templateW = cvRound(m_featureTemplate.cols * scale);
        const int templateH = cvRound(m_featureTemplate.rows * scale);
        if (templateW < 8 || templateH < 8 ||
            templateW >= searchImage.cols || templateH >= searchImage.rows) {
            continue;
        }

        cv::Mat scaledTemplate;
        cv::Mat result;
        try {
            cv::resize(m_featureTemplate, scaledTemplate, cv::Size(templateW, templateH), 0, 0, cv::INTER_AREA);
            cv::matchTemplate(searchImage, scaledTemplate, result, cv::TM_CCOEFF_NORMED);
        } catch (...) {
            continue;
        }

        for (int candidate = 0; candidate < kCandidatesPerScale; ++candidate) {
            double maxVal = 0.0;
            cv::Point maxLoc;
            cv::minMaxLoc(result, nullptr, &maxVal, nullptr, &maxLoc);
            if (maxVal < kFeatureRecoverMinTemplateSimilarity) {
                break;
            }

            const cv::Rect candidateRect(
                searchRect.x + maxLoc.x,
                searchRect.y + maxLoc.y,
                templateW,
                templateH);
            const double colorScore = featureColorSimilarity(frame, candidateRect);
            const double combinedScore = maxVal * 0.72 + colorScore * 0.28;
            if (combinedScore > bestScore) {
                bestScore = combinedScore;
                bestTemplateScore = maxVal;
                bestColorScore = colorScore;
                bestRect = candidateRect;
            }

            const int suppressW = std::max(8, templateW / 2);
            const int suppressH = std::max(8, templateH / 2);
            const cv::Rect suppressRect(
                std::max(0, maxLoc.x - suppressW / 2),
                std::max(0, maxLoc.y - suppressH / 2),
                std::min(suppressW, result.cols - std::max(0, maxLoc.x - suppressW / 2)),
                std::min(suppressH, result.rows - std::max(0, maxLoc.y - suppressH / 2)));
            if (suppressRect.width <= 0 || suppressRect.height <= 0) {
                break;
            }
            cv::rectangle(result, suppressRect, cv::Scalar(-1.0), cv::FILLED);
        }
    }

    if (bestTemplateScore < kFeatureRecoverMinTemplateSimilarity ||
        bestColorScore < kFeatureRecoverMinColorSimilarity ||
        bestScore < kFeatureRecoverMinCombinedScore) {
        return false;
    }

    recoveredRect = bestRect & cv::Rect(0, 0, frame.cols, frame.rows);
    if (recoveredRect.width < 8 || recoveredRect.height < 8) {
        return false;
    }

    return true;
}

bool TrackingEngine::recoverFeatureTrackerByColor(const cv::Mat &frame, cv::Rect &recoveredRect) const
{
    recoveredRect = cv::Rect();
    if (frame.empty() || m_featureColorHist.empty() ||
        m_featureReferenceSize.width < 8 || m_featureReferenceSize.height < 8) {
        return false;
    }

    const cv::Rect searchRect(0, 0, frame.cols, frame.rows);
    const cv::Mat searchFrame = frame(searchRect);

    cv::Mat hsv;
    cv::Mat backProject;
    try {
        cv::cvtColor(searchFrame, hsv, cv::COLOR_BGR2HSV);
        const float hRange[] = {0.0f, 180.0f};
        const float sRange[] = {0.0f, 256.0f};
        const float* ranges[] = {hRange, sRange};
        const int channels[] = {0, 1};
        cv::calcBackProject(&hsv, 1, channels, m_featureColorHist, backProject, ranges, 1.0);
        cv::GaussianBlur(backProject, backProject, cv::Size(9, 9), 0);
    } catch (...) {
        return false;
    }

    cv::Size windowSize = m_featureReferenceSize;
    windowSize.width = std::clamp(windowSize.width, 8, searchRect.width);
    windowSize.height = std::clamp(windowSize.height, 8, searchRect.height);
    if (windowSize.width >= backProject.cols || windowSize.height >= backProject.rows) {
        return false;
    }

    cv::Mat kernel = cv::Mat::ones(windowSize, CV_32F) /
                     static_cast<float>(windowSize.area());
    cv::Mat scoreMap;
    try {
        cv::filter2D(backProject, scoreMap, CV_32F, kernel, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
    } catch (...) {
        return false;
    }

    double maxVal = 0.0;
    cv::Point maxLoc;
    cv::minMaxLoc(scoreMap, nullptr, &maxVal, nullptr, &maxLoc);
    if (maxVal < kColorRecoverThreshold) {
        return false;
    }

    recoveredRect = cv::Rect(
        searchRect.x + maxLoc.x - windowSize.width / 2,
        searchRect.y + maxLoc.y - windowSize.height / 2,
        windowSize.width,
        windowSize.height) & cv::Rect(0, 0, frame.cols, frame.rows);
    if (recoveredRect.width < 8 || recoveredRect.height < 8) {
        return false;
    }

    return true;
}

bool TrackingEngine::restartFeatureTrackerFromRect(const cv::Mat &frame, const cv::Rect &target, const QString &methodName)
{
    const cv::Rect boundedTarget = target & cv::Rect(0, 0, frame.cols, frame.rows);
    if (frame.empty() || boundedTarget.width < 8 || boundedTarget.height < 8) {
        return false;
    }
    const bool fromYoloCandidate = (methodName == QStringLiteral("YOLO 候选+外观校验"));
    const bool targetPasses = fromYoloCandidate
                                  ? featureCandidatePassesYoloRecovery(frame, boundedTarget)
                                  : featureCandidatePasses(frame, boundedTarget, true);
    if (!targetPasses) {
        resetFeatureRecoveryCandidate();
        return false;
    }

    try {
        m_featureTracker = cv::TrackerCSRT::create();
        m_featureTracker->init(frame, boundedTarget);
        m_featureTrackerReady = true;
        m_featureLastRect = cv::Rect2d(
            boundedTarget.x, boundedTarget.y, boundedTarget.width, boundedTarget.height);
        m_featureUnreliableCount = 0;
        if (fromYoloCandidate) {
            buildFeatureReference(frame, boundedTarget);
        }
    } catch (...) {
        m_featureTracker.release();
        m_featureTrackerReady = false;
        return false;
    }

    resetFeatureRecoveryCandidate();
    onDnnResultReceived(m_featureLastRect, true, QStringLiteral("FEATURE"));
    emit logMessage(
        QStringLiteral("【系统】特征跟踪已通过 %1 重捕目标。").arg(methodName));
    return true;
}


void TrackingEngine::init(const QSize &frameSize) {
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    m_frameSize = cv::Size(frameSize.width(), frameSize.height());
}

void TrackingEngine::setFrameSize(const QSize &size) {
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    m_frameSize = cv::Size(size.width(), size.height());
}

void TrackingEngine::setCurrentModel(const QString &modelFileName)
{
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    restartDnnThread(modelFileName);
}

bool TrackingEngine::yoloReady() const
{
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    return m_dnnThread && !m_dnnThread->isNetEmpty();
}

bool TrackingEngine::yoloWarmedUp() const
{
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    return m_dnnThread && m_dnnThread->isWarmedUp();
}

void TrackingEngine::setTrackingBackend(bool useFeatureTracking, const QString &modelFileName) {
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    m_useFeatureTracking = useFeatureTracking;
    m_currentModelName = modelFileName;
}

void TrackingEngine::startTracking(const cv::Mat &frame, const cv::Rect2d &targetRect, bool useFeatureTracking, const QString &modelFileName) {
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    m_useFeatureTracking = useFeatureTracking;
    m_currentModelName = modelFileName;
    m_selectedRect = targetRect;
    resetFeatureYoloTargetClassification();
    
    if (!m_useFeatureTracking && (!m_dnnThread || m_dnnThread->isNetEmpty())) {
        emit logMessage("❌【错误】YOLO 模型加载失败或未准备好！");
        return;
    }
    
    m_isCapturing = true;
    m_lostFrameCount = 0;
    m_isTargetTracked = false;
    
    if (m_useFeatureTracking) {
        requestFeatureYoloTargetClassification(frame);
        if (!initFeatureTracker(frame, m_selectedRect)) {
            m_isCapturing = false;
            return;
        }
        m_isTargetTracked = true;
    } else {
        resetFeatureTracker();
        m_dnnThread->initDnn(frame, m_selectedRect);
    }
}

void TrackingEngine::stopTracking() {
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    m_isCapturing = false;
    m_lostFrameCount = 0;
    if (m_dnnThread) m_dnnThread->stopDnn();
    resetFeatureTracker();
    m_trackedRect = cv::Rect2d();
    m_isTargetTracked = false;
    m_offsetX = 0;
    m_offsetY = 0;
    emit targetLost();
}

void TrackingEngine::processFrame(const cv::Mat &frame) {
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    if (!m_isCapturing) return;
    
    if (m_useFeatureTracking) {
        if (!updateFeatureTracker(frame)) {
            // updateFeatureTracker 已经负责发起一次重捕；这里不要再次重捕，
            // 否则同一帧会重复执行整幅搜索。
            m_lostFrameCount++;
            if (m_lostFrameCount > 0) {
                m_isTargetTracked = false;
                m_offsetX = 0;
                m_offsetY = 0;
                if (m_lostFrameCount == 1) {
                    emit logMessage("【警告】目标丢失！已清除追踪框，正在尝试找回...");
                }
                m_trackedRect = cv::Rect2d();
                emit targetLost();
            }
        } else {
            m_isTargetTracked = true;
            m_lostFrameCount = 0;
        }
        
        if (m_isTargetTracked) {
            // Calculate offsets
            int cx = m_frameSize.width / 2;
            int cy = m_frameSize.height / 2;
            int tx = cvRound(m_trackedRect.x + m_trackedRect.width / 2.0);
            int ty = cvRound(m_trackedRect.y + m_trackedRect.height / 2.0);

            int16_t offset_x = tx - cx;
            int16_t offset_y = ty - cy;

            if (abs(offset_x) < DEAD_ZONE) offset_x = 0;
            if (abs(offset_y) < DEAD_ZONE) offset_y = 0;

            m_offsetX = offset_x;
            m_offsetY = offset_y;
            emit targetTracked(m_trackedRect, m_offsetX, m_offsetY);
        }
    } else {
        // YOLO logic is handled by dnnThread async, it calls onDnnResultReceived
        // But we might need to send frames to it
        if (m_dnnThread && m_dnnThread->isWarmedUp()) {
            m_dnnThread->updateDnn(frame);
        }
        if (m_isTargetTracked && m_trackedRect.width > 0) {
             emit targetTracked(m_trackedRect, m_offsetX, m_offsetY);
        } else if (!m_isTargetTracked) {
             emit targetLost();
        }
    }
}

bool TrackingEngine::isTracking() const {
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    return m_isTargetTracked;
}
cv::Rect2d TrackingEngine::currentTrackedRect() const {
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    return m_trackedRect;
}
int TrackingEngine::currentOffsetX() const {
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    return m_offsetX;
}
int TrackingEngine::currentOffsetY() const {
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    return m_offsetY;
}
bool TrackingEngine::isFeatureTrackingSelected() const {
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    return m_useFeatureTracking;
}

