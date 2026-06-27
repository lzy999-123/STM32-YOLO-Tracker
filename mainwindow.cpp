#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QSerialPortInfo>
#include <QMessageBox>
#include <QPainter>
#include <QVBoxLayout>
#include <QElapsedTimer>
#include <utility>
#include <QTimer>
#include <QDateTime>
#include <QPen>
#include <QRect>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QScopeGuard>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace cv;
using namespace cv::dnn;

namespace {
constexpr int kBottleClassId = 39;
constexpr float kDefaultConfidenceThreshold = 0.25f;
constexpr float kBottleConfidenceThreshold = 0.12f;
constexpr float kLockedClassConfidenceThreshold = 0.12f;
constexpr float kNmsThreshold = 0.4f;
constexpr int kYoloHoldLostFrames = 8;
constexpr int kYoloGlobalRecoverFrames = 3;
constexpr int kFeatureUnreliableLimit = 3;
constexpr int kOrbMinGoodMatches = 8;
constexpr int kOrbMinInliers = 6;
constexpr double kFeatureAcceptSimilarity = 0.18;
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
constexpr qint64 kFeatureRecoveryStrategyLogIntervalMs = 1800;
constexpr int kMaxYoloAppearanceCandidatesPerFrame = 4;
constexpr int kFeatureCompareMaxSide = 160;
constexpr double kColorRecoverThreshold = 1.10;
constexpr double kFeatureYoloClassifyMinSelectedCoverage = 0.35;
constexpr double kFeatureYoloClassifyMinCandidateCoverage = 0.22;
constexpr double kFeatureYoloClassifyMinSizeSimilarity = 0.18;

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

cv::Rect boundedIntRect(const cv::Rect2d &rect, const cv::Size &frameSize)
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

// ==========================================
// 大脑线程实现 (YOLOv8)
// ==========================================
/**
 * @brief 深度学习追踪线程构造函数
 * 初始化追踪器所需的标志位，并加载 YOLOv8 模型。
 * 使用 OpenCL 硬件加速（如有集成显卡），有效降低 CPU 占用和发热。
 */
DnnThread::DnnThread(const QString &modelFileName, QObject *parent)
    : QThread(parent)
    , m_modelFileName(modelFileName) {
    m_isTracking = false;
    m_needInit = false;
    m_hasNewFrame = false;
    m_isComputing = false;
    m_isWarmedUp = false;
    m_useYolo = false;
    m_lockedClassId = -1;
    m_dnnMissCount = 0;
    m_detectionRequestId = 0;
    m_needDetection = false;
    m_relX = 0; m_relY = 0; m_relW = 1; m_relH = 1;

    m_classNames = {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
        "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
        "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
        "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle",
        "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
        "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch", "potted plant", "bed",
        "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave", "oven",
        "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"
    };

    try {
        m_ortEnv = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "YOLOv8");
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(4); // 将线程数由 1 改为 4，大幅提升 CPU 模式下的 FPS
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        try {
            OrtCUDAProviderOptions cuda_options;
            cuda_options.device_id = 0;
            sessionOptions.AppendExecutionProvider_CUDA(cuda_options);
            qDebug() << "成功配置 ONNX Runtime CUDA 执行器!";
        } catch (...) {
            qDebug() << "配置 CUDA 失败，将回退到 CPU!";
        }
        const QStringList modelCandidates = m_modelFileName.isEmpty()
                                                ? QStringList{QStringLiteral("yolov8s.onnx"), QStringLiteral("yolov8n.onnx")}
                                                : QStringList{m_modelFileName};
        QString modelPath;
        QStringList checkedModelPaths;
        for (const QString& modelName : modelCandidates) {
            const QString appPath = QDir(QCoreApplication::applicationDirPath()).filePath(modelName);
            checkedModelPaths << appPath;
            if (QFileInfo::exists(appPath)) {
                modelPath = appPath;
                break;
            }

            const QString developmentPath = QDir::current().filePath(modelName);
            checkedModelPaths << developmentPath;
            if (QFileInfo::exists(developmentPath)) {
                modelPath = developmentPath;
                break;
            }
        }
        if (modelPath.isEmpty()) {
            m_modelLoadError = QStringLiteral("YOLO model not found. Checked: %1")
                                   .arg(checkedModelPaths.join(", "));
            throw std::runtime_error(m_modelLoadError.toStdString());
        }
        m_modelFileName = QFileInfo(modelPath).fileName();
        qDebug() << "正在加载 YOLO 模型:" << modelPath;
        m_ortSession = new Ort::Session(
            m_ortEnv, modelPath.toStdWString().c_str(), sessionOptions);

        Ort::AllocatorWithDefaultOptions allocator;
        m_inputNodeNamesStr.clear();
        m_inputNodeNames.clear();
        for (size_t i = 0; i < m_ortSession->GetInputCount(); i++) {
            auto input_name = m_ortSession->GetInputNameAllocated(i, allocator);
            m_inputNodeNamesStr.push_back(input_name.get());
        }
        for (const auto& s : m_inputNodeNamesStr) {
            m_inputNodeNames.push_back(s.c_str());
        }

        m_outputNodeNamesStr.clear();
        m_outputNodeNames.clear();
        for (size_t i = 0; i < m_ortSession->GetOutputCount(); i++) {
            auto output_name = m_ortSession->GetOutputNameAllocated(i, allocator);
            m_outputNodeNamesStr.push_back(output_name.get());
        }
        for (const auto& s : m_outputNodeNamesStr) {
            m_outputNodeNames.push_back(s.c_str());
        }
        
        qDebug() << "ONNX Runtime 模型加载成功!";
    } catch (const Ort::Exception& e) {
        qDebug() << "ONNX Runtime 加载异常: " << e.what();
    } catch (const std::exception& e) {
        qDebug() << "标准异常: " << e.what();
    }
}

DnnThread::~DnnThread() {
    requestInterruption();
    wait();
    if (m_ortSession) {
        delete m_ortSession;
        m_ortSession = nullptr;
    }
}

bool DnnThread::isBusy() {
    QMutexLocker locker(&m_mutex);
    return m_isComputing || m_hasNewFrame || m_needInit;
}

bool DnnThread::isWarmedUp() {
    QMutexLocker locker(&m_mutex);
    return m_isWarmedUp;
}

void DnnThread::initDnn(const cv::Mat &frame, const cv::Rect2d &target) {
    QMutexLocker locker(&m_mutex);
    if(frame.empty()) return;
    m_initFrame = frame;
    m_initRect = target;
    m_needInit = true;
    m_isTracking = true;
}

void DnnThread::updateDnn(const cv::Mat &frame) {
    QMutexLocker locker(&m_mutex);
    if (!m_isTracking || frame.empty()) return;
    m_frame = frame;
    m_hasNewFrame = true;
}

void DnnThread::requestDetections(const cv::Mat &frame, quint64 requestId) {
    QMutexLocker locker(&m_mutex);
    if (frame.empty()) return;
    m_detectFrame = frame;
    m_detectionRequestId = requestId;
    m_needDetection = true;
}

void DnnThread::stopDnn() {
    QMutexLocker locker(&m_mutex);
    m_isTracking = false;
    m_needInit = false;
    m_needDetection = false;
    m_hasNewFrame = false;
    m_initFrame.release();
    m_detectFrame.release();
    m_frame.release();
    m_useYolo = false;
    m_lockedClassId = -1;
    m_dnnMissCount = 0;
    m_relX = 0; m_relY = 0; m_relW = 1; m_relH = 1;
}

/**
 * @brief YOLOv8 深度学习推理主循环
 * 这是一个独立的后台线程，专门用于处理计算机视觉模型的前向推理（Forward Pass）。
 * 它会不断提取主界面的最新画面，调用 OpenCV DNN 模块，并解析 8400 个先验框。
 * 解析后，找到置信度最大且满足 IOU/类别限制的最佳框，通过信号发回主界面。
 */
void DnnThread::run() {
    // ONNX Runtime（尤其是 CUDA Provider）的第一次 Run 会创建 CUDA 上下文、
    // 加载内核并分配显存。提前在后台执行两次空推理，把这笔开销从首次跟踪移走。
    if (m_ortSession == nullptr) {
        QString warmupMessage = m_modelLoadError.isEmpty()
                                    ? QStringLiteral("YOLO 预热失败：模型未加载")
                                    : QStringLiteral("YOLO 预热失败：%1").arg(m_modelLoadError);
        emit dnnWarmupFinished(false, warmupMessage);
    } else if (!isInterruptionRequested()) {
        {
            QMutexLocker locker(&m_mutex);
            m_isComputing = true;
        }

        bool warmupSuccess = false;
        QString warmupMessage;
        try {
            constexpr size_t inputElementCount = 1ULL * 3 * 640 * 640;
            std::vector<float> warmupInput(inputElementCount, 0.0f);
            const std::vector<int64_t> inputDims = {1, 3, 640, 640};
            auto memoryInfo = Ort::MemoryInfo::CreateCpu(
                OrtDeviceAllocator, OrtMemTypeCPU);

            for (int i = 0; i < 2 && !isInterruptionRequested(); ++i) {
                Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                    memoryInfo,
                    warmupInput.data(),
                    warmupInput.size(),
                    inputDims.data(),
                    inputDims.size());
                auto warmupOutputs = m_ortSession->Run(
                    Ort::RunOptions{nullptr},
                    m_inputNodeNames.data(),
                    &inputTensor,
                    1,
                    m_outputNodeNames.data(),
                    1);
                if (warmupOutputs.empty()) {
                    throw std::runtime_error("YOLO warmup returned no output");
                }
            }

            warmupSuccess = !isInterruptionRequested();
            warmupMessage = warmupSuccess
                                ? QStringLiteral("YOLO 推理引擎预热完成：%1").arg(m_modelFileName)
                                : QStringLiteral("YOLO 推理引擎预热已取消");
        } catch (const Ort::Exception &e) {
            warmupMessage = QStringLiteral("YOLO 预热失败：%1")
                                .arg(QString::fromUtf8(e.what()));
        } catch (const std::exception &e) {
            warmupMessage = QStringLiteral("YOLO 预热失败：%1")
                                .arg(QString::fromUtf8(e.what()));
        }

        {
            QMutexLocker locker(&m_mutex);
            m_isWarmedUp = warmupSuccess;
            m_isComputing = false;
        }
        emit dnnWarmupFinished(warmupSuccess, warmupMessage);
    }

    while (!isInterruptionRequested()) {
        cv::Mat processFrame;
        bool doInit = false;
        bool doUpdate = false;
        bool doDetection = false;
        quint64 detectionRequestId = 0;
        cv::Rect2d initR;

        {
            QMutexLocker locker(&m_mutex);
            if (m_needDetection) {
                doDetection = true;
                detectionRequestId = m_detectionRequestId;
                m_needDetection = false;
                processFrame = m_detectFrame;
                m_detectFrame.release();
                m_isComputing = true;
            } else if (m_needInit) {
                doInit = true;
                initR = m_initRect;
                m_needInit = false;
                processFrame = m_initFrame;
                m_initFrame.release();
                m_isComputing = true;
            } else if (m_hasNewFrame) {
                doUpdate = true;
                m_hasNewFrame = false;
                processFrame = m_frame;
                m_isComputing = true;
            } else {
                locker.unlock();
                msleep(10);
                continue;
            }
        }

        if (processFrame.empty() || m_ortSession == nullptr) {
            QMutexLocker locker(&m_mutex);
            m_isComputing = false;
            continue;
        }

        // ------------------ ONNX Runtime 前向推理 ------------------
        cv::Mat blob;
        cv::dnn::blobFromImage(processFrame, blob, 1.0 / 255.0, cv::Size(640, 640), cv::Scalar(), true, false);
        
        std::vector<int64_t> input_dims = {1, 3, 640, 640};
        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(memory_info, (float*)blob.data, blob.total(), input_dims.data(), input_dims.size());

        std::vector<Ort::Value> output_tensors;
        try {
            output_tensors = m_ortSession->Run(Ort::RunOptions{nullptr}, m_inputNodeNames.data(), &input_tensor, 1, m_outputNodeNames.data(), 1);
        } catch (const Ort::Exception& e) {
            qDebug() << "YOLO 推理发生 Ort::Exception:" << e.what();
            QMutexLocker locker(&m_mutex);
            m_isComputing = false;
            continue;
        } catch (...) {
            qDebug() << "YOLO 推理发生未知异常!";
            QMutexLocker locker(&m_mutex);
            m_isComputing = false;
            continue;
        }

        if (output_tensors.empty() || !output_tensors[0].IsTensor()) {
            qDebug() << "YOLO 输出为空或不是张量";
            QMutexLocker locker(&m_mutex);
            m_isComputing = false;
            m_useYolo = false;
            continue;
        }

        auto output_info = output_tensors[0].GetTensorTypeAndShapeInfo();
        auto output_dims = output_info.GetShape();
        if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            output_dims.size() != 3 ||
            output_dims[0] != 1 ||
            output_dims[1] <= 0 ||
            output_dims[2] <= 0) {
            qDebug() << "不支持的 YOLO 输出结构";
            QMutexLocker locker(&m_mutex);
            m_isComputing = false;
            m_useYolo = false;
            continue;
        }

        float* output_data = output_tensors[0].GetTensorMutableData<float>();
        int r = output_dims[1]; // 84
        int c = output_dims[2]; // 8400
        cv::Mat output2d(r, c, CV_32F, output_data);
        
        // 自动识别正确的行列方向：YOLOv8 框数肯定是几千 (比如 8400)，特征数是几十 (比如 84)
        cv::Mat outputT;
        if (output2d.rows > output2d.cols) {
            outputT = output2d;
        } else {
            outputT = output2d.t(); // 现在 output2d 绝对是 2D 的，转置不会再崩溃
            outputT = outputT.clone(); // 极其关键：转置后必须深拷贝，否则内存不连续会导致下面读取全错！
        }


        // 防崩溃：确保张量特征维度至少包含 cx,cy,w,h 以及至少一个类别的分数
        if (outputT.cols < 5 || outputT.rows < 10) {
            QMutexLocker locker(&m_mutex);
            m_isComputing = false;
            m_useYolo = false;
            continue;
        }

        float x_factor = processFrame.cols / 640.0f;
        float y_factor = processFrame.rows / 640.0f;
        int num_classes = outputT.cols - 4;

        std::vector<int> classIds;
        std::vector<float> confidences;
        std::vector<cv::Rect> boxes;
        int activeLockedClass = -1;
        if (doUpdate) {
            QMutexLocker locker(&m_mutex);
            activeLockedClass = m_lockedClassId;
        }

        for (int i = 0; i < outputT.rows; ++i) {
            float* row = outputT.ptr<float>(i);
            float* classes_scores = row + 4;
            
            // 极速循环：替代原来耗时的 cv::minMaxLoc (8400次调用会拖慢近15毫秒！)
            float max_class_score = classes_scores[0];
            int best_class_id = 0;
            for (int c = 1; c < num_classes; ++c) {
                if (classes_scores[c] > max_class_score) {
                    max_class_score = classes_scores[c];
                    best_class_id = c;
                }
            }

            std::vector<std::pair<int, float>> rowCandidates;
            auto addCandidate = [&](int classId, float score) {
                if (classId < 0 || classId >= num_classes) return;
                if (score <= confidenceThresholdForClass(classId, activeLockedClass)) return;
                for (auto& candidate : rowCandidates) {
                    if (candidate.first == classId) {
                        candidate.second = std::max(candidate.second, score);
                        return;
                    }
                }
                rowCandidates.push_back({classId, score});
            };

            addCandidate(best_class_id, max_class_score);
            if (kBottleClassId < num_classes) {
                addCandidate(kBottleClassId, classes_scores[kBottleClassId]);
            }
            if (activeLockedClass >= 0 && activeLockedClass < num_classes) {
                addCandidate(activeLockedClass, classes_scores[activeLockedClass]);
            }

            if (!rowCandidates.empty()) {
                float cx = row[0];
                float cy = row[1];
                float w = row[2];
                float h = row[3];
                
                // 防御性处理：如果坐标是归一化的 (0~1)，我们需要将其映射回 640x640
                if (cx <= 2.0f && cy <= 2.0f && w <= 2.0f && h <= 2.0f) {
                    cx *= 640.0f;
                    cy *= 640.0f;
                    w *= 640.0f;
                    h *= 640.0f;
                }
                
                if (!std::isfinite(cx) || !std::isfinite(cy) ||
                    !std::isfinite(w) || !std::isfinite(h)) {
                    continue;
                }
                int left = int((cx - 0.5 * w) * x_factor);
                int top = int((cy - 0.5 * h) * y_factor);
                int width = int(w * x_factor);
                int height = int(h * y_factor);
                if (width <= 1 || height <= 1) continue;

                for (const auto& candidate : rowCandidates) {
                    classIds.push_back(candidate.first);
                    confidences.push_back(candidate.second);
                    boxes.push_back(cv::Rect(left, top, width, height));
                }
            }
        }

        std::vector<int> indices;
        for (int classId = 0; classId < num_classes; ++classId) {
            std::vector<cv::Rect> classBoxes;
            std::vector<float> classConfidences;
            std::vector<int> originalIndices;
            for (int i = 0; i < static_cast<int>(classIds.size()); ++i) {
                if (classIds[i] == classId) {
                    classBoxes.push_back(boxes[i]);
                    classConfidences.push_back(confidences[i]);
                    originalIndices.push_back(i);
                }
            }
            if (classBoxes.empty()) continue;

            std::vector<int> classNmsIndices;
            cv::dnn::NMSBoxes(
                classBoxes, classConfidences, 0.0f, kNmsThreshold, classNmsIndices);
            for (int nmsIdx : classNmsIndices) {
                if (nmsIdx >= 0 && nmsIdx < static_cast<int>(originalIndices.size())) {
                    indices.push_back(originalIndices[nmsIdx]);
                }
            }
        }

        if (doDetection) {
            for (int idx : indices) {
                if (idx < 0 ||
                    idx >= static_cast<int>(boxes.size()) ||
                    idx >= static_cast<int>(classIds.size()) ||
                    idx >= static_cast<int>(confidences.size())) {
                    continue;
                }
                const cv::Rect box = boxes[idx] & cv::Rect(0, 0, processFrame.cols, processFrame.rows);
                if (box.width <= 1 || box.height <= 1) {
                    continue;
                }
                emit yoloDetectionResult(
                    cv::Rect2d(box.x, box.y, box.width, box.height),
                    classIds[idx],
                    confidences[idx],
                    detectionRequestId,
                    false);
            }
            emit yoloDetectionResult(cv::Rect2d(), -1, 0.0f, detectionRequestId, true);
            QMutexLocker locker(&m_mutex);
            m_isComputing = false;
            continue;
        }

        if (doInit) {
            // 初始化阶段：优先选择真正贴近用户框选区域的检测框，避免大背景目标抢占小物体。
            double best_score = -1.0;
            int best_idx = -1;
            const double initArea = std::max(1.0, initR.area());
            const double initCx = initR.x + initR.width / 2.0;
            const double initCy = initR.y + initR.height / 2.0;

            for (int idx : indices) {
                cv::Rect det_rect = boxes[idx];
                cv::Rect2d det_rect2d(det_rect.x, det_rect.y, det_rect.width, det_rect.height);
                cv::Rect2d intersection = det_rect2d & initR;
                const double detArea = std::max(1.0, det_rect2d.area());
                const double intersectionArea = std::max(0.0, intersection.area());
                const double unionArea = std::max(1.0, detArea + initArea - intersectionArea);
                const double detCoverage = intersectionArea / detArea;
                const double selectionCoverage = intersectionArea / initArea;
                const double iou = intersectionArea / unionArea;
                const double areaSimilarity = std::min(detArea, initArea) / std::max(detArea, initArea);

                double cx = det_rect.x + det_rect.width / 2.0;
                double cy = det_rect.y + det_rect.height / 2.0;
                double dist = std::sqrt((cx - initCx)*(cx - initCx) + (cy - initCy)*(cy - initCy));
                double max_allowable_dist = std::max(150.0, std::max(initR.width, initR.height) * 1.5);
                if (intersectionArea <= 0.0 && dist > max_allowable_dist) {
                    continue;
                }

                const bool detCenterInsideSelection =
                    initR.contains(cv::Point2d(cx, cy));
                const bool selectionCenterInsideDet =
                    det_rect2d.contains(cv::Point2d(initCx, initCy));
                const double distanceScore = std::max(0.0, 1.0 - dist / max_allowable_dist);
                const double confidence =
                    (idx >= 0 && idx < static_cast<int>(confidences.size())) ? confidences[idx] : 0.0;
                const int classId =
                    (idx >= 0 && idx < static_cast<int>(classIds.size())) ? classIds[idx] : -1;
                const double score =
                    detCoverage * 4.0 +
                    iou * 2.0 +
                    areaSimilarity * 1.5 +
                    (detCenterInsideSelection ? 1.2 : 0.0) +
                    (selectionCenterInsideDet ? 0.8 : 0.0) +
                    (classId == kBottleClassId ? 0.6 : 0.0) +
                    distanceScore * 0.7 +
                    selectionCoverage * 0.4 +
                    confidence * 0.5;

                if (score > best_score) {
                    best_score = score;
                    best_idx = idx;
                }
            }

            QMutexLocker locker(&m_mutex);
            if (best_idx != -1 &&
                best_idx < static_cast<int>(classIds.size()) &&
                classIds[best_idx] >= 0 &&
                classIds[best_idx] < static_cast<int>(m_classNames.size()) &&
                boxes[best_idx].width > 0 &&
                boxes[best_idx].height > 0) {
                m_lockedClassId = classIds[best_idx];
                m_dnnMissCount = 0;
                m_useYolo = true;
                m_lastYoloRect = cv::Rect2d(boxes[best_idx].x, boxes[best_idx].y, boxes[best_idx].width, boxes[best_idx].height);
                
                // 计算用户框相对于 YOLO 识别框的比例和偏移，从而让跟踪框紧紧贴住用户选择的局部区域
                m_relW = initR.width / m_lastYoloRect.width;
                m_relH = initR.height / m_lastYoloRect.height;
                m_relX = (initR.x - m_lastYoloRect.x) / m_lastYoloRect.width;
                m_relY = (initR.y - m_lastYoloRect.y) / m_lastYoloRect.height;

                cv::Rect2d userAlignedRect(
                    m_lastYoloRect.x + m_relX * m_lastYoloRect.width,
                    m_lastYoloRect.y + m_relY * m_lastYoloRect.height,
                    m_lastYoloRect.width * m_relW,
                    m_lastYoloRect.height * m_relH
                );

                QString cName = QString::fromStdString(m_classNames[m_lockedClassId]);
                emit dnnTrackedResult(userAlignedRect, true, "LOCK:" + cName); // 特殊前缀用于触发锁定日志
            } else {
                m_useYolo = false;
                m_lockedClassId = -1;
                // 如果用户框选的地方真的没有任何 YOLO 目标，立刻通知 UI 丢失
                emit dnnTrackedResult(cv::Rect2d(), false, "");
            }
        } else if (doUpdate) {
            bool currentlyTracking = false;
            bool useYolo = false;
            int lockedClass = -1;
            cv::Rect2d lastYoloRect;
            double rx = 0, ry = 0, rw = 1, rh = 1;
            int missCountSnapshot = 0;

            {
                QMutexLocker locker(&m_mutex);
                currentlyTracking = m_isTracking;
                useYolo = m_useYolo;
                lockedClass = m_lockedClassId;
                lastYoloRect = m_lastYoloRect;
                rx = m_relX; ry = m_relY; rw = m_relW; rh = m_relH;
                missCountSnapshot = m_dnnMissCount;
            }

            if (currentlyTracking) {
                if (!useYolo) {
                    emit dnnTrackedResult(cv::Rect2d(), false, "");
                } else {
                    // 正常跟踪时按上一位置找近邻；丢失后改为在画面中心附近重捕同类目标。
                    double min_dist = 1e9;
                    int best_idx = -1;
                    int recover_idx = -1;
                    double best_recover_score = -1.0;
                    double last_cx = lastYoloRect.x + lastYoloRect.width / 2.0;
                    double last_cy = lastYoloRect.y + lastYoloRect.height / 2.0;
                    const double frame_cx = processFrame.cols / 2.0;
                    const double frame_cy = processFrame.rows / 2.0;
                    const double frameDiagonal = std::hypot(
                        static_cast<double>(processFrame.cols),
                        static_cast<double>(processFrame.rows));
                    const double maxCenterDistance = std::max(1.0, frameDiagonal * 0.45);
                    const double lastArea = std::max(1.0, lastYoloRect.area());
                    const bool allowGlobalRecovery =
                        missCountSnapshot >= kYoloGlobalRecoverFrames;

                    for (int idx : indices) {
                        if (classIds[idx] == lockedClass) {
                            cv::Rect r = boxes[idx];
                            double cx = r.x + r.width / 2.0;
                            double cy = r.y + r.height / 2.0;
                            double dist = std::sqrt((cx - last_cx)*(cx - last_cx) + (cy - last_cy)*(cy - last_cy));
                            if (dist < min_dist) {
                                min_dist = dist;
                                best_idx = idx;
                            }

                            if (allowGlobalRecovery) {
                                const double centerDist = std::hypot(cx - frame_cx, cy - frame_cy);
                                const double centerScore =
                                    std::max(0.0, 1.0 - centerDist / maxCenterDistance);
                                const double area = std::max(1.0, static_cast<double>(r.area()));
                                const double areaSimilarity =
                                    std::min(area, lastArea) / std::max(area, lastArea);
                                const double confidence =
                                    (idx >= 0 && idx < static_cast<int>(confidences.size()))
                                        ? confidences[idx]
                                        : 0.0;
                                const double score =
                                    centerScore * 2.2 +
                                    areaSimilarity * 0.9 +
                                    confidence * 1.2;
                                if (score > best_recover_score) {
                                    best_recover_score = score;
                                    recover_idx = idx;
                                }
                            }
                        }
                    }

                    const double maxTrackingDistance = std::max(
                        80.0,
                        std::min(frameDiagonal * 0.18,
                                 std::max(lastYoloRect.width, lastYoloRect.height) * 2.5));
                    bool acceptedByRecovery = false;
                    if ((best_idx == -1 || min_dist > maxTrackingDistance) &&
                        allowGlobalRecovery &&
                        recover_idx != -1 &&
                        best_recover_score >= 1.15) {
                        best_idx = recover_idx;
                        acceptedByRecovery = true;
                    }

                    if (best_idx != -1 && (min_dist <= maxTrackingDistance || acceptedByRecovery) &&
                        lockedClass >= 0 &&
                        lockedClass < static_cast<int>(m_classNames.size())) {
                        cv::Rect best_r = boxes[best_idx];
                        cv::Rect2d resRect(best_r.x, best_r.y, best_r.width, best_r.height);
                        const bool recoveredAfterMiss =
                            acceptedByRecovery || missCountSnapshot >= kYoloGlobalRecoverFrames;
                        {
                            QMutexLocker locker(&m_mutex);
                            m_lastYoloRect = resRect;
                            m_dnnMissCount = 0;
                        }

                        // 将 YOLO 框还原为用户框选的大小和位置
                        cv::Rect2d userAlignedRect(
                            resRect.x + rx * resRect.width,
                            resRect.y + ry * resRect.height,
                            resRect.width * rw,
                            resRect.height * rh
                        );

                        QString cName = QString::fromStdString(m_classNames[lockedClass]);
                        emit dnnTrackedResult(
                            userAlignedRect,
                            true,
                            recoveredAfterMiss ? QStringLiteral("RECOVER:") + cName : cName);
                    } else {
                        int missCount = 0;
                        cv::Rect2d holdRect;
                        bool canHold = false;
                        {
                            QMutexLocker locker(&m_mutex);
                            m_dnnMissCount++;
                            missCount = m_dnnMissCount;
                            if (missCount <= kYoloHoldLostFrames &&
                                m_lastYoloRect.width > 0 && m_lastYoloRect.height > 0) {
                                holdRect = cv::Rect2d(
                                    m_lastYoloRect.x + rx * m_lastYoloRect.width,
                                    m_lastYoloRect.y + ry * m_lastYoloRect.height,
                                    m_lastYoloRect.width * rw,
                                    m_lastYoloRect.height * rh);
                                canHold = true;
                            }
                        }
                        if (canHold) {
                            emit dnnTrackedResult(holdRect, true, "");
                        } else {
                            emit dnnTrackedResult(cv::Rect2d(), false, "");
                        }
                    }
                }
            }
        }

        {
            QMutexLocker locker(&m_mutex);
            m_isComputing = false;
        }
    }
}

// ==========================================
// 主窗口类实现
// ==========================================
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_camera(nullptr)
    , m_captureSession(nullptr)
    , m_videoWidget(nullptr)
    , m_videoSink(nullptr)
    , m_dnnThread(nullptr)
    , m_isCapturing(false)
    , m_isSelecting(false)
    , m_hasSelectedTarget(false)
    , m_currentMode(0)
    , m_isTargetTracked(false)
    , m_offsetX(0)
    , m_offsetY(0)
    , m_lastSelectedRect(cv::Rect2d())
    , m_wasTrackingBeforeDisconn(false)
    , m_forceResetTracking(false)
    , m_waitingForRecover(false)
    , m_cameraState(CameraState::Idle)
    , m_lastSerialSendTime(0)
    , m_dnnFrameSkipCounter(0)
    , m_lostFrameCount(0)
{
    ui->setupUi(this);
    cv::setNumThreads(4); // 限制 CPU 线程数，降低负载和风扇噪音
    this->setWindowTitle("STM32云台双核自动跟踪系统");


    // 确保显示直出图像时能够在 UI 中绝对居中
    ui->imageLabel->setAlignment(Qt::AlignCenter);

    QPixmap initPlaceholder(640, 480);
    initPlaceholder.fill(QColor(50, 50, 50));
    QPainter initPainter(&initPlaceholder);
    initPainter.setPen(Qt::white);
    QFont initFont = initPainter.font();
    initFont.setPointSize(12);
    initPainter.setFont(initFont);
    initPainter.drawText(initPlaceholder.rect(), Qt::AlignCenter, "请点击“打开摄像头”");
    initPainter.end();
    ui->imageLabel->setPixmap(initPlaceholder);

    m_rx_buffer.clear();
    on_pushButton_8_clicked();

    testDNN();

    ui->comboBox_4->clear();
    ui->comboBox_4->addItem(QStringLiteral("YOLOv8s（精度优先）"), QStringLiteral("yolov8s.onnx"));
    ui->comboBox_4->addItem(QStringLiteral("YOLOv8n（速度优先）"), QStringLiteral("yolov8n.onnx"));
    ui->comboBox_4->addItem(QStringLiteral("特征跟踪 CSRT+ORB（任意物体）"), QStringLiteral("feature"));
    connect(ui->comboBox_4, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTrackingModelChanged);
    preloadYoloModels();
    restartDnnThread(isFeatureTrackingSelected()
                         ? QStringLiteral("yolov8s.onnx")
                         : currentTrackingModelFileName());
    if (isFeatureTrackingSelected()) {
        ui->btnStartTracking->setEnabled(true);
        ui->btnStartTracking->setToolTip(QString());
    }

    m_captureSession = new QMediaCaptureSession(this);
    m_videoWidget = new QVideoWidget(ui->imageLabel);
    QVBoxLayout *layout = new QVBoxLayout(ui->imageLabel);
    layout->setContentsMargins(0,0,0,0);
    layout->addWidget(m_videoWidget);
    m_captureSession->setVideoOutput(m_videoWidget);
    m_videoWidget->setVisible(false);

    ui->comboBox_3->clear();
    QList<QCameraDevice> cameraDevices;
    try { cameraDevices = QMediaDevices::videoInputs(); } catch (...) {}

    if (cameraDevices.isEmpty()) {
        ui->comboBox_3->addItem("未检测到摄像头");
        ui->comboBox_3->setEnabled(false);
        ui->pushButton_9->setEnabled(false);
    } else {
        for (auto it = cameraDevices.cbegin(); it != cameraDevices.cend(); ++it) {
            ui->comboBox_3->addItem(it->description(), it->id());
        }
        ui->pushButton_9->setText("打开摄像头");
    }

    connect(ui->comboBox_3, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onCameraChanged);

    m_lastFrameTime = 0;
    m_currentCameraId = "";
    m_cameraCheckTimer = new QTimer(this);
    m_cameraCheckTimer->setInterval(1000);
    connect(m_cameraCheckTimer, &QTimer::timeout, this, [this]() {
        if (m_cameraState == CameraState::Opening || m_cameraState == CameraState::Closing) return;
        QList<QCameraDevice> currentCameras;
        try { currentCameras = QMediaDevices::videoInputs(); } catch (...) { return; }

        static int lastKnownCameraCount = -1;
        bool cameraIsOpen = (m_cameraState == CameraState::Open);
        int currentCount = currentCameras.size();

        if (lastKnownCameraCount != currentCount) {
            ui->comboBox_3->blockSignals(true);
            ui->comboBox_3->clear();
            int targetIndex = -1;
            if (currentCameras.isEmpty()) {
                ui->comboBox_3->addItem("未检测到摄像头");
                ui->comboBox_3->setEnabled(false);
            } else {
                ui->comboBox_3->setEnabled(true);
                int loopIndex = 0;
                for (const auto& device : std::as_const(currentCameras)) {
                    ui->comboBox_3->addItem(device.description(), device.id());
                    if (!m_currentCameraId.isEmpty() && device.id() == m_currentCameraId) targetIndex = loopIndex;
                    loopIndex++;
                }
                if (targetIndex >= 0) ui->comboBox_3->setCurrentIndex(targetIndex);
                else ui->comboBox_3->setCurrentIndex(0);
            }
            ui->comboBox_3->blockSignals(false);
            if (m_cameraState == CameraState::Error && currentCount > 0 && ui->pushButton_9->text() == "打开摄像头") {
                QTimer::singleShot(500, this, [this]() { m_cameraState = CameraState::Idle; on_pushButton_9_clicked(); });
            }
            lastKnownCameraCount = currentCount;
        }

        if (cameraIsOpen && !m_currentCameraId.isEmpty()) {
            bool deviceExist = false;
            for (const auto& device : std::as_const(currentCameras)) {
                if (device.id() == m_currentCameraId) { deviceExist = true; break; }
            }
            if (!deviceExist) { emergencyCameraStop(); return; }
        }
    });
    m_cameraCheckTimer->start();
}

MainWindow::~MainWindow()
{
    for (DnnThread *thread : std::as_const(m_dnnThreads)) {
        if (!thread) {
            continue;
        }
        thread->requestInterruption();
        thread->stopDnn();
        thread->wait();
        delete thread;
    }
    m_dnnThreads.clear();
    m_dnnThread = nullptr;
    {
        QMutexLocker locker(&m_cameraMutex);
        if (m_camera) {
            if (m_videoSink) {
                disconnect(m_videoSink, &QVideoSink::videoFrameChanged, this, &MainWindow::handleNewVideoFrame);
                m_videoSink = nullptr;
            }
            m_captureSession->setCamera(nullptr);
            m_camera->deleteLater();
            m_camera = nullptr;
        }
    }
    if (m_serial.isOpen()) m_serial.close();
    delete m_videoWidget;
    delete m_captureSession;
    delete ui;
}

QString MainWindow::currentTrackingModelFileName() const
{
    if (!ui || !ui->comboBox_4) {
        return QStringLiteral("yolov8s.onnx");
    }

    const QString modelFileName = ui->comboBox_4->currentData().toString();
    return modelFileName.isEmpty() ? QStringLiteral("yolov8s.onnx") : modelFileName;
}

MainWindow::TrackingBackend MainWindow::currentTrackingBackend() const
{
    if (!ui || !ui->comboBox_4) {
        return TrackingBackend::Yolo;
    }

    return ui->comboBox_4->currentData().toString() == QStringLiteral("feature")
               ? TrackingBackend::Feature
               : TrackingBackend::Yolo;
}

bool MainWindow::isFeatureTrackingSelected() const
{
    return currentTrackingBackend() == TrackingBackend::Feature;
}

void MainWindow::resetFeatureTracker()
{
    m_featureTracker.release();
    m_featureTrackerReady = false;
    m_featureTemplate.release();
    m_featureColorHist.release();
    m_featureReferenceKeypoints.clear();
    m_featureReferenceDescriptors.release();
    m_featureReferenceSize = cv::Size();
    m_featureLastRect = cv::Rect2d();
    resetFeatureRecoveryCandidate();
    m_featureUnreliableCount = 0;
}

void MainWindow::buildFeatureReference(const cv::Mat &frame, const cv::Rect &target)
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

double MainWindow::featurePatchSimilarity(const cv::Mat &frame, const cv::Rect &target) const
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

double MainWindow::featureColorSimilarity(const cv::Mat &frame, const cv::Rect &target) const
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

double MainWindow::featureShapeSimilarity(const cv::Rect &target) const
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

double MainWindow::featureCandidateScore(
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

cv::Rect MainWindow::alignFeatureRectInYoloCandidate(const cv::Rect &yoloRect, const cv::Size &frameSize) const
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

bool MainWindow::evaluateYoloFeatureCandidate(
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

bool MainWindow::featureCandidatePasses(
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

void MainWindow::resetFeatureRecoveryCandidate()
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

quint64 MainWindow::nextFeatureYoloRequestId()
{
    ++m_featureYoloNextRequestId;
    if (m_featureYoloNextRequestId == 0) {
        m_featureYoloNextRequestId = 1;
    }
    return m_featureYoloNextRequestId;
}

void MainWindow::resetFeatureYoloTargetClassification()
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

bool MainWindow::confirmFeatureRecoveryCandidate(
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
        ui->plainTextEdit_2->appendPlainText(
            kFeatureRecoveryConfirmFrames <= 1
                ? QStringLiteral("【系统】发现疑似目标，正在重新锁定...")
                : QStringLiteral("【系统】发现疑似目标，正在连续确认..."));
    }

    m_featureRecoveryCandidateRect = cv::Rect2d(
        boundedTarget.x, boundedTarget.y, boundedTarget.width, boundedTarget.height);
    return m_featureRecoveryCandidateCount >= kFeatureRecoveryConfirmFrames;
}

void MainWindow::requestFeatureYoloTargetClassification(const cv::Mat &frame)
{
    resetFeatureYoloTargetClassification();
    if (frame.empty()) {
        m_featureUseYoloRecovery = false;
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【系统】当前画面不可用，暂时无法判断框选目标是否属于 YOLO 类别；重捕先按传统特征处理。"));
        return;
    }

    if (!m_dnnThread || m_dnnThread->isNetEmpty()) {
        m_featureYoloClassKnown = true;
        m_featureUseYoloRecovery = false;
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【系统】YOLO 辅助未加载：框选目标按非 YOLO 目标处理。重捕策略：ORB/模板/颜色传统特征重捕。"));
        return;
    }

    if (!m_dnnThread->isWarmedUp()) {
        m_featureUseYoloRecovery = false;
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【系统】YOLO 辅助正在预热，框选目标暂未完成类别判断；开始跟踪或预热完成后会重试，当前先按传统特征重捕。"));
        return;
    }

    m_featureYoloClassifyPending = true;
    m_featureYoloClassifyRequestId = nextFeatureYoloRequestId();
    m_featureYoloClassifyFrame = frame.clone();
    m_dnnThread->requestDetections(m_featureYoloClassifyFrame, m_featureYoloClassifyRequestId);
    ui->plainTextEdit_2->appendPlainText(
        QStringLiteral("【系统】正在判断框选目标是否属于 YOLO 模型类别..."));
}

void MainWindow::requestFeatureYoloRecovery(const cv::Mat &frame)
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

bool MainWindow::initFeatureTracker(const cv::Mat &frame, const cv::Rect2d &target)
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
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【系统】特征跟踪算法：CSRT 主跟踪；重捕策略将按 YOLO 类别判定自动分流。"));
        return true;
    } catch (const cv::Exception &e) {
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【警告】特征跟踪初始化失败：%1").arg(QString::fromLocal8Bit(e.what())));
    } catch (...) {
        ui->plainTextEdit_2->appendPlainText(QStringLiteral("【警告】特征跟踪初始化失败。"));
    }

    resetFeatureTracker();
    return false;
}

bool MainWindow::updateFeatureTracker(const cv::Mat &frame)
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
        ui->plainTextEdit_2->appendPlainText(
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

        onDnnResultReceived(cv::Rect2d(), false, QStringLiteral("FEATURE"));
        return false;
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

bool MainWindow::recoverFeatureTracker(const cv::Mat &frame)
{
    if (frame.empty()) {
        return false;
    }

    auto logRecoveryStrategy = [this](const QString &message) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (message != m_lastFeatureRecoveryStrategyLog ||
            now - m_lastFeatureRecoveryStrategyLogTime > kFeatureRecoveryStrategyLogIntervalMs) {
            m_lastFeatureRecoveryStrategyLog = message;
            m_lastFeatureRecoveryStrategyLogTime = now;
            ui->plainTextEdit_2->appendPlainText(message);
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

bool MainWindow::recoverFeatureTrackerByOrb(const cv::Mat &frame, cv::Rect &recoveredRect) const
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

bool MainWindow::recoverFeatureTrackerByTemplate(const cv::Mat &frame, cv::Rect &recoveredRect) const
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

bool MainWindow::recoverFeatureTrackerByColor(const cv::Mat &frame, cv::Rect &recoveredRect) const
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

bool MainWindow::restartFeatureTrackerFromRect(const cv::Mat &frame, const cv::Rect &target, const QString &methodName)
{
    const cv::Rect boundedTarget = target & cv::Rect(0, 0, frame.cols, frame.rows);
    if (frame.empty() || boundedTarget.width < 8 || boundedTarget.height < 8) {
        return false;
    }
    const bool fromYoloCandidate = (methodName == QStringLiteral("YOLO 候选+外观校验"));
    if (!fromYoloCandidate && !featureCandidatePasses(frame, boundedTarget, true)) {
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
    } catch (...) {
        m_featureTracker.release();
        m_featureTrackerReady = false;
        return false;
    }

    resetFeatureRecoveryCandidate();
    onDnnResultReceived(m_featureLastRect, true, QStringLiteral("FEATURE"));
    ui->plainTextEdit_2->appendPlainText(
        QStringLiteral("【系统】特征跟踪已通过 %1 重捕目标。").arg(methodName));
    return true;
}

DnnThread *MainWindow::ensureDnnThread(const QString &modelFileName)
{
    const QString key = modelFileName.isEmpty()
                            ? QStringLiteral("yolov8s.onnx")
                            : modelFileName;
    if (DnnThread *existingThread = m_dnnThreads.value(key, nullptr)) {
        return existingThread;
    }

    ui->plainTextEdit_2->appendPlainText(
        QStringLiteral("【系统】正在后台加载并预热 YOLO 模型：%1").arg(key));

    DnnThread *thread = new DnnThread(key, this);
    m_dnnThreads.insert(key, thread);
    connect(thread, &DnnThread::dnnTrackedResult,
            this, &MainWindow::onDnnResultReceived, Qt::QueuedConnection);
    connect(thread, &DnnThread::yoloDetectionResult,
            this, &MainWindow::onYoloDetectionResult, Qt::QueuedConnection);
    connect(thread, &DnnThread::dnnWarmupFinished, this,
            [this, thread](bool success, const QString &message) {
        ui->plainTextEdit_2->appendPlainText(
            QString(success ? "【系统】%1" : "【警告】%1").arg(message));
        if (m_dnnThread == thread) {
            ui->btnStartTracking->setEnabled(success || isFeatureTrackingSelected());
            ui->btnStartTracking->setToolTip(
                success ? QString() : QStringLiteral("YOLO 推理引擎预热失败"));
        }
        if (success &&
            m_dnnThread == thread &&
            isFeatureTrackingSelected() &&
            m_hasSelectedTarget &&
            !m_featureYoloClassKnown &&
            !m_featureYoloClassifyPending) {
            cv::Mat currentFrameClone;
            {
                QMutexLocker locker(&m_cameraMutex);
                if (!m_lastFrame.empty()) {
                    currentFrameClone = m_lastFrame.clone();
                }
            }
            if (!currentFrameClone.empty()) {
                requestFeatureYoloTargetClassification(currentFrameClone);
            }
        }
    }, Qt::QueuedConnection);
    thread->start(QThread::LowPriority);
    return thread;
}

void MainWindow::preloadYoloModels()
{
    ensureDnnThread(QStringLiteral("yolov8s.onnx"));
    ensureDnnThread(QStringLiteral("yolov8n.onnx"));
    ui->plainTextEdit_2->appendPlainText(
        QStringLiteral("【系统】已启动 YOLOv8s 与 YOLOv8n 后台预热。"));
}

void MainWindow::restartDnnThread(const QString &modelFileName)
{
    const bool wasAutoTracking = m_isCapturing && ui->label_11->text() == "自动";
    m_isCapturing = false;
    m_isTargetTracked = false;
    m_trackedRect = cv::Rect2d();
    m_offsetX = 0;
    m_offsetY = 0;
    resetFeatureTracker();
    if (wasAutoTracking) {
        sendCommand(0x12);
    }

    DnnThread *thread = ensureDnnThread(modelFileName);
    if (m_dnnThread && m_dnnThread != thread) {
        m_dnnThread->stopDnn();
    }
    m_dnnThread = thread;
    if (thread && !thread->isNetEmpty() && thread->isWarmedUp()) {
        ui->btnStartTracking->setEnabled(true);
        ui->btnStartTracking->setToolTip(QString());
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【系统】已切换到已预热模型：%1").arg(thread->modelFileName()));
    } else if (thread && !thread->isNetEmpty()) {
        ui->btnStartTracking->setEnabled(isFeatureTrackingSelected());
        ui->btnStartTracking->setToolTip(QStringLiteral("YOLO 推理引擎正在后台预热"));
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【系统】已切换到模型：%1，正在等待预热完成。").arg(modelFileName));
    } else {
        ui->btnStartTracking->setEnabled(isFeatureTrackingSelected());
        ui->btnStartTracking->setToolTip(QStringLiteral("YOLO 模型加载失败"));
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【警告】YOLO 模型加载失败：%1").arg(modelFileName));
    }
}

void MainWindow::onTrackingModelChanged(int index)
{
    if (index < 0) return;

    const QString modelFileName = ui->comboBox_4->itemData(index).toString();
    if (modelFileName.isEmpty()) return;
    if (modelFileName == QStringLiteral("feature")) {
        const bool wasAutoTracking = m_isCapturing && ui->label_11->text() == "自动";
        m_isCapturing = false;
        m_isTargetTracked = false;
        m_trackedRect = cv::Rect2d();
        m_offsetX = 0;
        m_offsetY = 0;
        resetFeatureTracker();
        if (wasAutoTracking) {
            sendCommand(0x12);
        }
        if (m_dnnThread) {
            m_dnnThread->stopDnn();
        }
        m_dnnThread = ensureDnnThread(QStringLiteral("yolov8s.onnx"));
        ui->btnStartTracking->setEnabled(true);
        ui->btnStartTracking->setToolTip(QString());
        ui->plainTextEdit_2->appendPlainText(
            QStringLiteral("【系统】已切换到特征跟踪：CSRT 主跟踪 + YOLO 候选重捕 + 外观校验。"));
        return;
    }
    if (m_dnnThread && m_dnnThread->modelFileName() == modelFileName) return;

    restartDnnThread(modelFileName);
}

void MainWindow::emergencyCameraStop()
{
    if (m_cameraState == CameraState::Closing || m_cameraState == CameraState::Idle) return;
    if (m_isCapturing && m_hasSelectedTarget) m_wasTrackingBeforeDisconn = true;
    m_cameraState = CameraState::Error;

    {
        QMutexLocker locker(&m_cameraMutex);
        if (m_videoSink) { disconnect(m_videoSink, nullptr, this, nullptr); m_videoSink = nullptr; }
    }

    if (m_dnnThread) { m_dnnThread->stopDnn(); }
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        m_pendingVideoFrame = QVideoFrame();
        m_frameDispatchPending = false;
    }

    {
        QMutexLocker locker(&m_cameraMutex);
        if (m_camera) {
            disconnect(m_camera, &QCamera::activeChanged, this, nullptr);
            m_camera->stop();
            m_captureSession->setCamera(nullptr);
            m_camera->deleteLater();
            m_camera = nullptr;
        }
    }

    ui->imageLabel->clear();
    QPixmap errorPlaceholder(ui->imageLabel->size());
    errorPlaceholder.fill(QColor(50, 50, 50));
    QPainter painter(&errorPlaceholder);
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setPointSize(12);
    painter.setFont(font);
    painter.drawText(errorPlaceholder.rect(), Qt::AlignCenter, "摄像头已断开\n请重新插入");
    painter.end();
    ui->imageLabel->setPixmap(errorPlaceholder);

    m_isCapturing = false;
    m_isSelecting = false;
    resetFeatureTracker();
    m_trackedRect = cv::Rect2d();
    m_forceResetTracking = true;
    m_lastFrameTime = 0;

    ui->pushButton_9->setText("打开摄像头");
    ui->pushButton_9->setEnabled(true);
    ui->plainTextEdit->setPlainText("未追踪到目标\n舵机保持居中");
    ui->plainTextEdit_2->appendPlainText("【提示】摄像头已断开");
}

void MainWindow::testDNN() {
    try {
        cv::dnn::Net net;
        cv::Mat dummyInput = cv::Mat::zeros(224, 224, CV_8UC3);
        cv::Mat blob = cv::dnn::blobFromImage(dummyInput, 1.0, cv::Size(224, 224), cv::Scalar(0, 0, 0), true, false);
    } catch (...) { }
}

void MainWindow::on_pushButton_clicked()
{
    m_serial.setPortName(ui->comboBox->currentText());
    m_serial.setBaudRate(ui->comboBox_2->currentText().toInt());
    m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setStopBits(QSerialPort::OneStop);
    m_serial.setParity(QSerialPort::NoParity);
    m_serial.setFlowControl(QSerialPort::NoFlowControl);

    if(ui->pushButton->text()=="打开串口") {
        if(m_serial.open(QIODevice::ReadWrite)==true) {
            ui->lineEdit->setText("已连接");
            connect(&m_serial, &QSerialPort::readyRead, this, &MainWindow::messlot);
            ui->pushButton->setText("关闭串口");
            ui->LED1->setStyleSheet("background-color:green");
            ui->pushButton_2->setEnabled(true);
            ui->pushButton_3->setEnabled(true);
            m_rx_buffer.clear();
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 500) { m_serial.waitForReadyRead(50); QCoreApplication::processEvents(); }
            ui->plainTextEdit_2->appendPlainText("【串口】已连接，状态同步完成");
        } else {
            ui->lineEdit->setText("串口打开失败！");
            QMessageBox::warning(this, "提示", "串口打开失败！");
        }
    } else {
        ui->lineEdit->setText("未连接");
        ui->pushButton->setText("打开串口");
        ui->LED1->setStyleSheet("background-color:red");
        disconnect(&m_serial, &QSerialPort::readyRead, this, &MainWindow::messlot);
        m_serial.close();
        ui->pushButton_2->setEnabled(false);
        ui->pushButton_3->setEnabled(false);
    }
}

void MainWindow::on_pushButton_8_clicked()
{
    ui->comboBox->clear();
    QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    foreach(const QSerialPortInfo &info, ports) { ui->comboBox->addItem(info.portName()); }
}

void MainWindow::onCameraChanged(int index)
{
    if (m_cameraState == CameraState::Opening || m_cameraState == CameraState::Closing || m_cameraState == CameraState::Error) return;
    QMutexLocker locker(&m_cameraMutex);
    if (m_cameraState == CameraState::Closing) return;

    if (m_videoSink) { disconnect(m_videoSink, &QVideoSink::videoFrameChanged, this, &MainWindow::handleNewVideoFrame); m_videoSink = nullptr; }
    if (m_camera) { m_camera->stop(); m_captureSession->setCamera(nullptr); m_camera->deleteLater(); m_camera = nullptr; }

    if (index < 0 || ui->comboBox_3->count() == 0) return;
    QString cameraId = ui->comboBox_3->itemData(index).toString();
    QCameraDevice selectedCamera;
    bool found = false;
    QList<QCameraDevice> cameraDevices;
    try { cameraDevices = QMediaDevices::videoInputs(); } catch (...) {}
    for (const QCameraDevice &device : std::as_const(cameraDevices)) {
        if (device.id() == cameraId) { selectedCamera = device; found = true; break; }
    }
    if (!found) return;

    try {
        m_camera = new QCamera(selectedCamera);
        m_captureSession->setCamera(m_camera);
        m_videoSink = m_captureSession->videoSink();
        connect(m_videoSink, &QVideoSink::videoFrameChanged, this,
                &MainWindow::handleNewVideoFrame, Qt::DirectConnection);
    } catch (...) { return; }

    if(ui->pushButton_9->text() == "关闭摄像头") {
        m_currentCameraId = cameraId;
        m_camera->start();
    }
}

void MainWindow::on_pushButton_9_clicked()
{
    if (m_cameraState == CameraState::Opening || m_cameraState == CameraState::Closing) return;

    if (ui->pushButton_9->text() == "关闭摄像头") {
        m_cameraState = CameraState::Closing;
        m_wasTrackingBeforeDisconn = false;

        {
            QMutexLocker locker(&m_cameraMutex);
            if (m_videoSink) { disconnect(m_videoSink, nullptr, this, nullptr); m_videoSink = nullptr; }
            if (m_camera) {
                disconnect(m_camera, &QCamera::activeChanged, this, nullptr);
                m_camera->stop();
                m_captureSession->setCamera(nullptr);
                m_camera->deleteLater();
                m_camera = nullptr;
            }
        }

        if (m_dnnThread) { m_dnnThread->stopDnn(); }
        {
            QMutexLocker locker(&m_pendingFrameMutex);
            m_pendingVideoFrame = QVideoFrame();
            m_frameDispatchPending = false;
        }

        m_isCapturing = false;
        m_isSelecting = false;
        m_hasSelectedTarget = false;
        resetFeatureTracker();
        m_selectedRect = cv::Rect2d();
        m_trackedRect = cv::Rect2d();
        m_isTargetTracked = false;
        m_lastFrame.release();

        m_videoWidget->setVisible(false);

        QPixmap placeholder(ui->imageLabel->size());
        placeholder.fill(QColor(50, 50, 50));
        QPainter painter(&placeholder);
        painter.setPen(Qt::white);
        QFont font = painter.font();
        font.setPointSize(12);
        painter.setFont(font);
        painter.drawText(ui->imageLabel->rect(), Qt::AlignCenter, "摄像头已关闭\n请点击打开");
        painter.end();
        ui->imageLabel->setPixmap(placeholder);
        ui->pushButton_9->setText("打开摄像头");
        ui->plainTextEdit_2->appendPlainText("【摄像头】已关闭");
        m_cameraState = CameraState::Idle;
        return;
    }

    if (ui->pushButton_9->text() == "打开摄像头") {
        auto cameraDevices = QMediaDevices::videoInputs();
        if (cameraDevices.isEmpty()) { QMessageBox::warning(this, "错误", "未检测到任何摄像头设备！"); return; }
        if (ui->comboBox_3->currentText() == "未检测到摄像头") { QMessageBox::warning(this, "错误", "请插入摄像头后再打开！"); return; }

        m_cameraState = CameraState::Opening;
        ui->pushButton_9->setEnabled(false);
        ui->pushButton_9->setText("正在打开...");

        QTimer::singleShot(100, this, [this]() {
            QList<QCameraDevice> cameraDevices;
            try { cameraDevices = QMediaDevices::videoInputs(); }
            catch (...) { m_cameraState = CameraState::Idle; ui->pushButton_9->setEnabled(true); ui->pushButton_9->setText("打开摄像头"); return; }

            QString targetCameraId;
            QCameraDevice selectedCamera;
            bool found = false;

            if (!m_currentCameraId.isEmpty()) {
                for (auto it = cameraDevices.cbegin(); it != cameraDevices.cend(); ++it) {
                    if (it->id() == m_currentCameraId) { selectedCamera = *it; found = true; break; }
                }
            }

            if (!found) {
                QString comboId = ui->comboBox_3->itemData(ui->comboBox_3->currentIndex()).toString();
                for (auto it = cameraDevices.cbegin(); it != cameraDevices.cend(); ++it) {
                    if (it->id() == comboId) { selectedCamera = *it; found = true; break; }
                }
            }

            if (!found && !cameraDevices.isEmpty()) { selectedCamera = cameraDevices.first(); found = true; }

            if (!found) {
                QMessageBox::warning(this, "错误", "摄像头已断开/不存在！");
                ui->comboBox_3->setCurrentIndex(0);
                m_cameraState = CameraState::Idle;
                ui->pushButton_9->setEnabled(true);
                ui->pushButton_9->setText("打开摄像头");
                return;
            }

            {
                QMutexLocker locker(&m_cameraMutex);
                try {
                    // 原生格式直出，不强加分辨率限制
                    m_camera = new QCamera(selectedCamera);
                    m_captureSession->setCamera(m_camera);
                    m_videoWidget->setVisible(false);

                    if (m_videoSink) { disconnect(m_videoSink, nullptr, this, nullptr); }
                    m_videoSink = m_captureSession->videoSink();
                    connect(m_videoSink, &QVideoSink::videoFrameChanged, this,
                            &MainWindow::handleNewVideoFrame, Qt::DirectConnection);

                    connect(m_camera, &QCamera::activeChanged, this, [this](bool active) {
                        if (active) {
                            m_cameraState = CameraState::Open;
                            ui->pushButton_9->setText("关闭摄像头");
                            ui->pushButton_9->setEnabled(true);
                            ui->imageLabel->clear();
                            ui->imageLabel->setStyleSheet("");
                            ui->plainTextEdit_2->appendPlainText("【摄像头】已打开");
                            m_currentCameraId = m_camera->cameraDevice().id();
                            int idx = ui->comboBox_3->findData(m_currentCameraId);
                            if(idx >= 0) {
                                const QSignalBlocker blocker(ui->comboBox_3);
                                ui->comboBox_3->setCurrentIndex(idx);
                            }
                            m_lastFrameTime = 0;
                        }
                    });
                    connect(m_camera, &QCamera::errorOccurred, this,
                            [this](QCamera::Error, const QString &errorString) {
                        if (m_cameraState != CameraState::Opening &&
                            m_cameraState != CameraState::Open) {
                            return;
                        }
                        ui->plainTextEdit_2->appendPlainText(
                            QString("【摄像头错误】%1").arg(errorString));
                        emergencyCameraStop();
                    });

                    m_camera->start();
                    QTimer::singleShot(5000, this, [this]() {
                        if (m_cameraState != CameraState::Opening) return;
                        ui->plainTextEdit_2->appendPlainText("【摄像头错误】启动超时");
                        emergencyCameraStop();
                    });

                } catch (...) {
                    QMessageBox::warning(this, "错误", "摄像头打开失败！");
                    if (m_camera) { m_captureSession->setCamera(nullptr); m_camera->deleteLater(); m_camera = nullptr; }
                    m_videoSink = nullptr;
                    m_cameraState = CameraState::Idle;
                    ui->pushButton_9->setEnabled(true);
                    ui->pushButton_9->setText("打开摄像头");
                    return;
                }
            }
        });
    }
}

Mat MainWindow::QImageToCvMat(const QImage& qImage) {
    if (qImage.isNull() || qImage.format() != QImage::Format_RGB888) return Mat();
    int width = qImage.width(), height = qImage.height(), bytes_per_line = qImage.bytesPerLine();
    Mat mat(height, width, CV_8UC3, const_cast<uchar*>(qImage.bits()), bytes_per_line);
    Mat bgr_mat; cv::cvtColor(mat, bgr_mat, cv::COLOR_RGB2BGR);
    return bgr_mat.clone();
}

cv::Mat MainWindow::QVideoFrameToCvMat(const QVideoFrame &frame) {
    if (!frame.isValid()) return cv::Mat();
    QImage qImage = frame.toImage();
    if (qImage.isNull()) return cv::Mat();
    cv::Mat cvMat;
    try {
        switch (qImage.format()) {
        case QImage::Format_RGB32:
        case QImage::Format_ARGB32:
        case QImage::Format_ARGB32_Premultiplied: {
            cv::Mat src(qImage.height(), qImage.width(), CV_8UC4, const_cast<uchar*>(qImage.bits()), qImage.bytesPerLine());
            cv::cvtColor(src, cvMat, cv::COLOR_BGRA2BGR); break;
        }
        case QImage::Format_RGB888: {
            cv::Mat src(qImage.height(), qImage.width(), CV_8UC3, const_cast<uchar*>(qImage.bits()), qImage.bytesPerLine());
            cv::cvtColor(src, cvMat, cv::COLOR_RGB2BGR); break;
        }
        default: {
            QImage converted = qImage.convertToFormat(QImage::Format_RGB888);
            cv::Mat src(converted.height(), converted.width(), CV_8UC3, const_cast<uchar*>(converted.bits()), converted.bytesPerLine());
            cv::cvtColor(src, cvMat, cv::COLOR_RGB2BGR); break;
        }
        }
    } catch (...) { return cv::Mat(); }
    return cvMat.clone();
}

QImage MainWindow::CvMatToQImage(const cv::Mat& mat) {
    if (mat.empty()) return QImage();
    if (mat.type() == CV_8UC3) {
        cv::Mat rgbMat; cv::cvtColor(mat, rgbMat, cv::COLOR_BGR2RGB);
        QImage img = QImage((const uchar*)rgbMat.data, rgbMat.cols, rgbMat.rows, rgbMat.step, QImage::Format_RGB888).copy();
        rgbMat.release(); return img;
    } else if (mat.type() == CV_8UC1) {
        return QImage((const uchar*)mat.data, mat.cols, mat.rows, mat.step, QImage::Format_Grayscale8).copy();
    }
    return QImage();
}

void MainWindow::handleNewVideoFrame(const QVideoFrame &frame) {
    if (!frame.isValid()) return;

    bool shouldDispatch = false;
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        m_pendingVideoFrame = frame;
        if (!m_frameDispatchPending) {
            m_frameDispatchPending = true;
            shouldDispatch = true;
        }
    }

    if (shouldDispatch) {
        QMetaObject::invokeMethod(
            this, &MainWindow::processLatestVideoFrame, Qt::QueuedConnection);
    }
}

void MainWindow::processLatestVideoFrame() {
    const auto resetFrameDispatch = qScopeGuard([this]() {
        QMutexLocker locker(&m_pendingFrameMutex);
        m_frameDispatchPending = false;
    });

    QVideoFrame frame;
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        frame = m_pendingVideoFrame;
        m_pendingVideoFrame = QVideoFrame();
        // Keep dispatch marked busy until this function exits, even on early returns.
    }

    if (m_cameraState != CameraState::Open) return;
    bool cameraOk = false;
    { QMutexLocker locker(&m_cameraMutex); cameraOk = m_camera && m_camera->isActive() && m_cameraState == CameraState::Open; }
    if (!cameraOk || !frame.isValid()) return;
    cv::Mat cvMat;
    try { cvMat = QVideoFrameToCvMat(frame); } catch (...) { return; }
    if (cvMat.empty()) return;
    cv::flip(cvMat, cvMat, -1);
    { QMutexLocker locker(&m_frameSizeMutex); m_frameSize = QSize(cvMat.cols, cvMat.rows); }
    if (m_forceResetTracking) {
        if (m_wasTrackingBeforeDisconn && m_hasSelectedTarget &&
            (isFeatureTrackingSelected() || (m_dnnThread && !m_dnnThread->isNetEmpty()))) {
            const cv::Rect2d frameBounds(0, 0, cvMat.cols, cvMat.rows);
            m_selectedRect = m_lastSelectedRect & frameBounds;
            if (m_selectedRect.width >= 8 && m_selectedRect.height >= 8) {
                m_trackedRect = cv::Rect2d();
                m_lostFrameCount = 0;
                m_isCapturing = true;
                if (isFeatureTrackingSelected()) {
                    resetFeatureYoloTargetClassification();
                    requestFeatureYoloTargetClassification(cvMat);
                    initFeatureTracker(cvMat, m_selectedRect);
                } else {
                    m_dnnThread->initDnn(cvMat, m_selectedRect);
                }
                ui->plainTextEdit_2->appendPlainText("【系统】摄像头已恢复，正在重新锁定目标");
            }
        }
        m_forceResetTracking = false;
        m_wasTrackingBeforeDisconn = false;
    }
    if (m_isCapturing && isFeatureTrackingSelected()) {
        updateFeatureTracker(cvMat);
    } else if (m_isCapturing && m_dnnThread && !m_dnnThread->isInterruptionRequested()) {
        // 推理忙碌时不排队旧帧，而是持续覆盖为摄像头的最新源帧。
        // 当前推理结束后会直接处理此刻最新的画面。
        m_dnnThread->updateDnn(cvMat);
    }
    { QMutexLocker locker(&m_cameraMutex); if (m_cameraState != CameraState::Open) return; m_lastFrame = cvMat.clone(); }
    // ==== 纯 YOLOv8 UI 渲染与控制逻辑 ====
    // 此段代码负责将 OpenCV 处理出的画面转换为 Qt 的 QImage，
    // 并将底层计算出的 YOLOv8 跟踪框(m_trackedRect)映射到 UI 界面的正确比例和位置进行绘制。
    QImage img = CvMatToQImage(cvMat);
    if (!img.isNull()) {
        QSize labelSize = ui->imageLabel->size();
        if (labelSize.width() >= 50 && labelSize.height() >= 50) {
            QSize imageSize = img.size();
            if (m_renderCanvas.size() != labelSize) { m_renderCanvas = QPixmap(labelSize); }

            QPainter painter(&m_renderCanvas);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            m_renderCanvas.fill(Qt::black);
            
            QSize scaledImageSize = imageSize.scaled(labelSize, Qt::KeepAspectRatio);
            int xOffset = (labelSize.width() - scaledImageSize.width()) / 2;
            int yOffset = (labelSize.height() - scaledImageSize.height()) / 2;
            painter.drawImage(QRect(xOffset, yOffset, scaledImageSize.width(), scaledImageSize.height()), img);

            double scaleX = (double)scaledImageSize.width() / imageSize.width();
            double scaleY = (double)scaledImageSize.height() / imageSize.height();
            int centerX = labelSize.width() / 2;
            int centerY = labelSize.height() / 2;
            int crossLength = 30;
            
            painter.setPen(QPen(Qt::red, 3));
            painter.drawLine(centerX, centerY - crossLength, centerX, centerY + crossLength);
            painter.drawLine(centerX - crossLength, centerY, centerX + crossLength, centerY);

            if (!m_waitingForRecover && ((m_trackedRect.width > 0 && m_trackedRect.height > 0) || m_isCapturing)) {
                if (m_trackedRect.width > 5 && m_trackedRect.height > 5) {
                    QRectF qtTrackedRect(
                        xOffset + m_trackedRect.x * scaleX,
                        yOffset + m_trackedRect.y * scaleY,
                        m_trackedRect.width * scaleX,
                        m_trackedRect.height * scaleY
                    );
                    if (m_isTargetTracked) {
                        painter.setPen(QPen(Qt::green, 4));
                        painter.drawRect(qtTrackedRect);
                        QFont trackFont = painter.font();
                        trackFont.setPointSize(10);
                        trackFont.setBold(true);
                        painter.setFont(trackFont);
                        painter.drawText(
                            qtTrackedRect.topLeft() + QPointF(5, -5),
                            isFeatureTrackingSelected() ? QStringLiteral("CSRT+ORB TRACK")
                                                        : QStringLiteral("YOLO TRACK"));
                    }
                }
            }

            if (m_isSelecting && m_selectStartImg != m_selectEndImg) {
                QRect originalSelectRect = QRect(m_selectStartImg, m_selectEndImg).normalized();
                QRectF scaledSelectRect(xOffset + originalSelectRect.x() * scaleX, yOffset + originalSelectRect.y() * scaleY, originalSelectRect.width() * scaleX, originalSelectRect.height() * scaleY);
                painter.setPen(QPen(Qt::blue, 3));
                painter.drawRect(scaledSelectRect);
                QFont selFont = painter.font();
                selFont.setPointSize(10);
                painter.setFont(selFont);
                painter.drawText(scaledSelectRect.topLeft() + QPointF(5, -5), "Select");
            }
            
            static qint64 lastFpsCalcTime = QDateTime::currentMSecsSinceEpoch();
            static int frameCount = 0;
            static int displayedFps = 0;

            frameCount++;
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - lastFpsCalcTime >= 1000) {
                displayedFps = frameCount;
                frameCount = 0;
                lastFpsCalcTime = now;
            }

            painter.setPen(QPen(Qt::yellow, 2));
            QFont fpsFont = painter.font();
            fpsFont.setPointSize(10);
            fpsFont.setBold(true);
            painter.setFont(fpsFont);
            painter.setPen(Qt::black);
            painter.drawText(32, 72, QString("FPS: %1").arg(displayedFps));
            painter.setPen(Qt::yellow);
            painter.drawText(30, 70, QString("FPS: %1").arg(displayedFps));

            painter.end();
            ui->imageLabel->setPixmap(m_renderCanvas);
        }
    }

    QMutexLocker modeLocker(&m_modeMutex);
    if (m_serial.isOpen() && m_currentMode == 1 && m_isTargetTracked) {
        qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
        if (currentTime - m_lastSerialSendTime >= 20) {
            uint8_t txBuf[6] = { 0xFF, (uint8_t)(m_offsetX >> 8), (uint8_t)(m_offsetX & 0xFF), (uint8_t)(m_offsetY >> 8), (uint8_t)(m_offsetY & 0xFF), 0xFE };
            m_serial.write((char*)txBuf, 6);
            m_lastSerialSendTime = currentTime;
        }
    }

    static qint64 lastInfoUpdateTime = 0;
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - lastInfoUpdateTime >= 100) {
        lastInfoUpdateTime = nowMs;
        if (m_isCapturing && !m_trackedRect.empty()) {
            QString info;
            info += QString("X 偏移：%1\n").arg(m_offsetX);
            info += QString("Y 偏移：%2\n").arg(m_offsetY);
            info += (m_offsetX > 0) ? "舵机X → 向右转\n" : (m_offsetX < 0) ? "舵机X → 向左转\n" : "舵机X → 居中\n";
            info += (m_offsetY > 0) ? "舵机Y → 向下转\n" : (m_offsetY < 0) ? "舵机Y → 向上转\n" : "舵机Y → 居中\n";
            ui->plainTextEdit->setPlainText(info);
        } else {
            if (!m_waitingForRecover) ui->plainTextEdit->setPlainText("未追踪到目标\n舵机保持居中");
        }
    }
}


/**
 * @brief 接收当前跟踪算法的目标框结果
 * @param dnnRect  跟踪算法输出的目标边界框 (相对于相机原始分辨率)
 * @param success  是否成功跟踪到了目标 (true 表示当前帧有目标, false 表示目标丢失)
 * @param className 目标的分类名称，如 "person", "car"。如果带有 "LOCK:" 前缀表示这是初始锁定的类别
 * 
 * 此槽函数由 YOLO 线程或特征跟踪流程触发。
 * 它负责更新 UI 端的跟踪状态，并将识别框转换为 UI 可以显示的像素坐标。
 */
void MainWindow::onDnnResultReceived(const cv::Rect2d &dnnRect, bool success, const QString &className) {
    if (!m_isCapturing) return;

    QString resultName = className;
    const bool isRecoverResult = resultName.startsWith(QStringLiteral("RECOVER:"));
    if (resultName.startsWith(QStringLiteral("LOCK:"))) {
        QString actualName = resultName.mid(5);
        if (ui->plainTextEdit_2) {
            ui->plainTextEdit_2->appendPlainText(QString("【系统】YOLO 已识别并锁定目标: %1").arg(actualName));
        }
        resultName = actualName;
        // 注意：这里不再 early return，而是继续执行后面的逻辑以初始化 m_trackedRect 和状态
    } else if (isRecoverResult) {
        QString actualName = resultName.mid(8);
        if (ui->plainTextEdit_2) {
            ui->plainTextEdit_2->appendPlainText(QString("【系统】YOLO 重捕完成，已重新锁定目标: %1").arg(actualName));
        }
        resultName = actualName;
    }

    bool isReasonable = true;
    if (dnnRect.width < 10 || dnnRect.height < 10 ||
        dnnRect.width > m_frameSize.width() * 0.8 ||
        dnnRect.height > m_frameSize.height() * 0.8) {
        isReasonable = false;
    }

    const int DEAD_ZONE = 18;
    const bool isFeatureResult = (resultName == QStringLiteral("FEATURE"));
    bool currentSuccess = (success && isReasonable);

    if (currentSuccess) {
        m_isTargetTracked = true;
        m_lostFrameCount = 0; // 目标找回，清零

        // 自适应滤波：静止时抑制检测抖动，快速移动时立即跟随，
        // 避免固定低 alpha 造成明显拖尾。
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

            double positionAlpha = 0.45;
            if (movementRatio > 0.18) {
                positionAlpha = 1.0;
            } else if (movementRatio > 0.08) {
                positionAlpha = 0.82;
            } else if (movementRatio > 0.03) {
                positionAlpha = 0.65;
            }
            const double sizeAlpha = std::min(0.75, positionAlpha);

            m_trackedRect.x += (dnnRect.x - m_trackedRect.x) * positionAlpha;
            m_trackedRect.y += (dnnRect.y - m_trackedRect.y) * positionAlpha;
            m_trackedRect.width += (dnnRect.width - m_trackedRect.width) * sizeAlpha;
            m_trackedRect.height += (dnnRect.height - m_trackedRect.height) * sizeAlpha;
        }

        int cx = m_frameSize.width() / 2;
        int cy = m_frameSize.height() / 2;
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
                if (ui->plainTextEdit_2) {
                    ui->plainTextEdit_2->appendPlainText("【警告】目标丢失！已清除追踪框，正在尝试找回...");
                }
                m_trackedRect = cv::Rect2d(); // 立刻将宽和高变成0，界面不再画框
            }
        }
        // 如果 lostFrameCount <= MAX_LOST_TOLERANCE，则保持 m_isTargetTracked 为 true，
        // 且保留 m_trackedRect、m_offsetX、m_offsetY 的上一次值，起到防抖和惯性预测的作用。
    }


}

void MainWindow::onYoloDetectionResult(
    const cv::Rect2d &rect,
    int classId,
    float confidence,
    quint64 requestId,
    bool finished)
{
    static qint64 lastYoloRecoveryLogTime = 0;
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

            ui->plainTextEdit_2->appendPlainText(
                QStringLiteral("【系统】框选目标属于 YOLO 类别：%1。重捕策略：YOLO 检测该类别 + 初始外观校验。")
                    .arg(m_featureTargetYoloClassName));
        } else {
            m_featureUseYoloRecovery = false;
            m_featureTargetYoloClassId = -1;
            m_featureTargetYoloClassName.clear();
            ui->plainTextEdit_2->appendPlainText(
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
                    m_featureYoloBestCandidate =
                        (relativeCandidate.width >= 8 && relativeCandidate.height >= 8)
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
        if (now - lastYoloRecoveryLogTime > 1200) {
            lastYoloRecoveryLogTime = now;
            ui->plainTextEdit_2->appendPlainText(
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
        if (now - lastYoloRecoveryLogTime > 1200) {
            lastYoloRecoveryLogTime = now;
            ui->plainTextEdit_2->appendPlainText(
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

void MainWindow::on_btnSelectTarget_clicked() {
    if (!m_camera || !m_camera->isActive()) { QMessageBox::warning(this, "提示", "请先打开摄像头！"); return; }
    if (m_dnnThread) { m_dnnThread->stopDnn(); }
    resetFeatureTracker();
    m_hasSelectedTarget = false; m_trackedRect = cv::Rect2d(); m_isTargetTracked = false; m_isSelecting = true;
    m_lostFrameCount = 0;
    m_selectStart = QPoint(); m_selectEnd = QPoint(); m_selectStartImg = QPoint(); m_selectEndImg = QPoint();
    ui->plainTextEdit_2->appendPlainText("【提示】正在框选，请在画面内拖动鼠标...");
}

void MainWindow::on_btnStartTracking_clicked() {
    if (!m_camera || !m_camera->isActive()) { QMessageBox::warning(this, "提示", "请先打开摄像头！"); return; }
    if (!m_hasSelectedTarget) { QMessageBox::warning(this, "提示", "请先选择目标！"); return; }
    const bool useFeatureTracking = isFeatureTrackingSelected();
    if (!useFeatureTracking && (!m_dnnThread || m_dnnThread->isNetEmpty())) {
        ui->plainTextEdit_2->appendPlainText(
            "❌【错误】YOLO 模型加载失败，请检查程序目录或当前工作目录中的 yolov8s.onnx / yolov8n.onnx！");
        return;
    }
    if (!useFeatureTracking && !m_dnnThread->isWarmedUp()) {
        ui->plainTextEdit_2->appendPlainText("【提示】YOLO 引擎正在后台预热，请稍候再开始跟踪。");
        return;
    }

    cv::Mat currentFrameClone;
    { QMutexLocker locker(&m_cameraMutex); if (!m_lastFrame.empty()) currentFrameClone = m_lastFrame.clone(); }
    if (currentFrameClone.empty()) {
        ui->plainTextEdit_2->appendPlainText("【提示】尚未收到摄像头画面，请稍后重试。");
        return;
    }

    m_isCapturing = true;
    m_wasTrackingBeforeDisconn = true;
    m_lostFrameCount = 0;
    if (useFeatureTracking) {
        if (!m_featureYoloClassKnown && !m_featureYoloClassifyPending) {
            requestFeatureYoloTargetClassification(currentFrameClone);
        }
        if (!initFeatureTracker(currentFrameClone, m_selectedRect)) {
            m_isCapturing = false;
            return;
        }
    } else {
        resetFeatureTracker();
        m_dnnThread->initDnn(currentFrameClone, m_selectedRect);
    }
    if(ui->label_11->text()=="自动") sendCommand(0x11);
    ui->plainTextEdit_2->appendPlainText(
        useFeatureTracking ? QStringLiteral("开始特征跟踪（CSRT + ORB）！")
                           : QStringLiteral("开始纯 YOLOv8 跟踪！"));
}

void MainWindow::on_btnStopTracking_clicked() {
    if (!m_camera || !m_camera->isActive()) { return; }
    if (!m_hasSelectedTarget) { return; }
    cv::Mat currentFrameClone;
    { QMutexLocker locker(&m_cameraMutex); if (!m_lastFrame.empty()) currentFrameClone = m_lastFrame.clone(); }
    m_isCapturing = false;
    m_lostFrameCount = 0;
    if (m_dnnThread) m_dnnThread->stopDnn();
    resetFeatureTracker();
    m_trackedRect = cv::Rect2d(); m_wasTrackingBeforeDisconn = false; m_isTargetTracked = false; m_offsetX = 0; m_offsetY = 0;
    if (!currentFrameClone.empty()) {
        const QImage img = CvMatToQImage(currentFrameClone);
        if (!img.isNull()) {
            ui->imageLabel->setPixmap(QPixmap::fromImage(img).scaled(
                ui->imageLabel->size(),
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation));
        }
    }
    if(ui->label_11->text()=="自动") sendCommand(0x12);
    ui->plainTextEdit_2->appendPlainText("停止跟踪！");
}

void MainWindow::mousePressEvent(QMouseEvent *event) {
    if (!m_isSelecting) { QMainWindow::mousePressEvent(event); return; }
    QPoint localPos = ui->imageLabel->mapFromGlobal(event->globalPosition().toPoint());
    if (!ui->imageLabel->rect().contains(localPos) || event->button() != Qt::LeftButton) { QMainWindow::mousePressEvent(event); return; }
    m_selectStart = localPos;
    m_selectEnd = localPos;
    QSize frameSize; { QMutexLocker locker(&m_frameSizeMutex); frameSize = m_frameSize; }
    if (frameSize.isEmpty()) return;
    QSize imageSize = frameSize;
    QSize labelSize = ui->imageLabel->size();
    QSize scaledImageSize = imageSize.scaled(labelSize, Qt::KeepAspectRatio);
    int xOffset = (labelSize.width() - scaledImageSize.width()) / 2;
    int yOffset = (labelSize.height() - scaledImageSize.height()) / 2;
    QRect displayedImageRect(QPoint(xOffset, yOffset), scaledImageSize);
    if (!displayedImageRect.contains(localPos)) return;
    double scaleX = (double)imageSize.width() / scaledImageSize.width();
    double scaleY = (double)imageSize.height() / scaledImageSize.height();
    m_selectStartImg = QPoint((int)((localPos.x() - xOffset) * scaleX), (int)((localPos.y() - yOffset) * scaleY));
    m_selectEndImg = m_selectStartImg;
}

void MainWindow::mouseMoveEvent(QMouseEvent *event) {
    if (!m_isSelecting) { QMainWindow::mouseMoveEvent(event); return; }
    QPoint localPos = ui->imageLabel->mapFromGlobal(event->globalPosition().toPoint());
    if (!ui->imageLabel->rect().contains(localPos)) return;
    m_selectEnd = localPos;
    QSize frameSize; { QMutexLocker locker(&m_frameSizeMutex); frameSize = m_frameSize; }
    if (frameSize.isEmpty()) return;
    QSize imageSize = frameSize;
    QSize labelSize = ui->imageLabel->size();
    QSize scaledImageSize = imageSize.scaled(labelSize, Qt::KeepAspectRatio);
    int xOffset = (labelSize.width() - scaledImageSize.width()) / 2;
    int yOffset = (labelSize.height() - scaledImageSize.height()) / 2;
    QRect displayedImageRect(QPoint(xOffset, yOffset), scaledImageSize);
    localPos.setX(std::clamp(localPos.x(), displayedImageRect.left(), displayedImageRect.right()));
    localPos.setY(std::clamp(localPos.y(), displayedImageRect.top(), displayedImageRect.bottom()));
    double scaleX = (double)imageSize.width() / scaledImageSize.width();
    double scaleY = (double)imageSize.height() / scaledImageSize.height();
    m_selectEndImg = QPoint((int)((localPos.x() - xOffset) * scaleX), (int)((localPos.y() - yOffset) * scaleY));
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event) {
    if (!m_isSelecting) { QMainWindow::mouseReleaseEvent(event); return; }
    QPoint localPos = ui->imageLabel->mapFromGlobal(event->globalPosition().toPoint());
    if (!ui->imageLabel->rect().contains(localPos) || event->button() != Qt::LeftButton) { QMainWindow::mouseReleaseEvent(event); return; }
    m_selectEnd = localPos;
    QSize frameSize; { QMutexLocker locker(&m_frameSizeMutex); frameSize = m_frameSize; }
    if (frameSize.isEmpty()) return;
    QSize imageSize = frameSize;
    QSize labelSize = ui->imageLabel->size();
    QSize scaledImageSize = imageSize.scaled(labelSize, Qt::KeepAspectRatio);
    int xOffset = (labelSize.width() - scaledImageSize.width()) / 2;
    int yOffset = (labelSize.height() - scaledImageSize.height()) / 2;
    QRect displayedImageRect(QPoint(xOffset, yOffset), scaledImageSize);
    localPos.setX(std::clamp(localPos.x(), displayedImageRect.left(), displayedImageRect.right()));
    localPos.setY(std::clamp(localPos.y(), displayedImageRect.top(), displayedImageRect.bottom()));
    double scaleX = (double)imageSize.width() / scaledImageSize.width();
    double scaleY = (double)imageSize.height() / scaledImageSize.height();
    m_selectEndImg = QPoint((int)((localPos.x() - xOffset) * scaleX), (int)((localPos.y() - yOffset) * scaleY));
    m_isSelecting = false;

    QRect selectRect = QRect(m_selectStartImg, m_selectEndImg).normalized()
                           .intersected(QRect(0, 0, frameSize.width(), frameSize.height()));
    if (selectRect.width() < 8 || selectRect.height() < 8) {
        QMessageBox::warning(this, "提示", "框选区域太小！");
        return;
    }

    m_selectedRect = cv::Rect2d(selectRect.x(), selectRect.y(), selectRect.width(), selectRect.height());
    m_hasSelectedTarget = true;
    m_lastSelectedRect = m_selectedRect;
    resetFeatureYoloTargetClassification();

    int maxEdge = std::max(selectRect.width(), selectRect.height());
    if (maxEdge > 180) {
        ui->plainTextEdit_2->appendPlainText("【提示】框选区域较大，可能降低跟踪帧率。建议仅框选核心局部！");
    } else {
        ui->plainTextEdit_2->appendPlainText("【提示】目标模板已锁定。");
    }

    if (isFeatureTrackingSelected()) {
        cv::Mat currentFrameClone;
        {
            QMutexLocker locker(&m_cameraMutex);
            if (!m_lastFrame.empty()) currentFrameClone = m_lastFrame.clone();
        }
        requestFeatureYoloTargetClassification(currentFrameClone);
    }
}

void MainWindow::paintEvent(QPaintEvent *event) { QMainWindow::paintEvent(event); }

void MainWindow::on_pushButton_2_clicked() {
    QMutexLocker locker(&m_modeMutex);
    ui->pushButton_2->setEnabled(false);
    QTimer::singleShot(100, this, [=]() { ui->pushButton_2->setEnabled(true); });
    m_isCapturing = false;
    if (m_dnnThread) m_dnnThread->stopDnn();
    resetFeatureTracker();
    m_trackedRect = cv::Rect2d();
    m_isTargetTracked = false;
    m_hasSelectedTarget = false;
    m_isSelecting = false;
    m_offsetX = 0;
    m_offsetY = 0;
    sendCommand(0x02);
}

void MainWindow::on_pushButton_3_clicked() {
    if (!m_serial.isOpen()) { QMessageBox::warning(this, "提示", "请先连接串口！"); return; }
    QMutexLocker locker(&m_modeMutex);
    m_currentMode = !m_currentMode;
    if (m_currentMode == 0) {
        ui->label_11->setText("手动");
        ui->label_11->setStyleSheet("color: black; font-size: 14px; font-weight: bold;");
    } else {
        ui->label_11->setText("自动");
        ui->label_11->setStyleSheet("color: red; font-size: 14px; font-weight: bold;");
    }
    m_isCapturing = false;
    if (m_dnnThread) m_dnnThread->stopDnn();
    resetFeatureTracker();
    m_trackedRect = cv::Rect2d();
    m_isTargetTracked = false;
    m_hasSelectedTarget = false;
    m_isSelecting = false;
    m_offsetX = 0;
    m_offsetY = 0;
    sendCommand(0x01);
    ui->pushButton_3->setEnabled(false);
    QTimer::singleShot(100, this, [=]() { ui->pushButton_3->setEnabled(true); });
}

void MainWindow::sendCommand(uint8_t cmd) {
    if (!m_serial.isOpen()) return;
    QByteArray cmdData; cmdData.append((char)0xCC); cmdData.append((char)cmd); cmdData.append((char)0xDD);
    m_serial.write(cmdData);
}

void MainWindow::messlot() {
    QByteArray new_data = m_serial.readAll();
    if (new_data.isEmpty()) return;
    m_rx_buffer.append(new_data);

    while (true) {
        int head = m_rx_buffer.indexOf("AA");
        if (head == -1) {
            if (m_rx_buffer.size() > 1) m_rx_buffer = m_rx_buffer.right(1);
            break;
        }
        if (head > 0) {
            m_rx_buffer.remove(0, head);
            head = 0;
        }
        int tail = m_rx_buffer.indexOf("BB", head + 2);
        if (tail == -1) {
            if (m_rx_buffer.size() > 4096) m_rx_buffer.truncate(4096);
            break;
        }
        QByteArray frame = m_rx_buffer.mid(head, tail - head + 2);
        QString s = QString::fromUtf8(frame); QStringList list = s.mid(2, s.length() - 4).split(",");
        if (list.count() == 3) {
            int remoteMode = list[0].toInt();
            ui->plainTextEdit_3->setPlainText(list[1]);
            ui->plainTextEdit_4->setPlainText(list[2]);
            static int lastRemoteMode = -1;
            if (remoteMode != lastRemoteMode) {
                lastRemoteMode = remoteMode; QMutexLocker locker(&m_modeMutex); m_currentMode = remoteMode;
                if (remoteMode == 0) { ui->label_11->setText("手动"); ui->label_11->setStyleSheet("color: black; font-size: 14px; font-weight: bold;"); }
                else { ui->label_11->setText("自动"); ui->label_11->setStyleSheet("color: red; font-size: 14px; font-weight: bold;"); }
                m_isCapturing = false;
                if (m_dnnThread) m_dnnThread->stopDnn();
                resetFeatureTracker();
                m_trackedRect = cv::Rect2d(); m_isTargetTracked = false; m_offsetX = 0; m_offsetY = 0;
            }
        }
        m_rx_buffer.remove(0, tail + 2);
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    for (DnnThread *thread : std::as_const(m_dnnThreads)) {
        if (!thread) {
            continue;
        }
        thread->requestInterruption();
        thread->stopDnn();
        thread->wait(300);
    }
    m_cameraState = CameraState::Closing;
    { QMutexLocker locker(&m_cameraMutex); if (m_camera) m_camera->stop(); }
    if (m_serial.isOpen()) { sendCommand(0x12); sendCommand(0x02); m_serial.waitForBytesWritten(200); m_serial.close(); }
    event->accept();
}

