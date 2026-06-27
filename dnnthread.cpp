#include "dnnthread.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
constexpr int kBottleClassId = 39;
constexpr float kDefaultConfidenceThreshold = 0.25f;
constexpr float kBottleConfidenceThreshold = 0.12f;
constexpr float kLockedClassConfidenceThreshold = 0.12f;
constexpr float kNmsThreshold = 0.4f;
constexpr int kYoloHoldLostFrames = 8;
constexpr int kYoloGlobalRecoverFrames = 3;
constexpr double kYoloSizeDeadZoneRatio = 0.045;
constexpr double kYoloSizeDeadZonePixels = 4.0;

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

cv::Rect2d boundedRect(const cv::Rect2d &rect, const cv::Size &frameSize)
{
    if (frameSize.width <= 0 || frameSize.height <= 0) {
        return cv::Rect2d();
    }

    const double left = std::clamp(rect.x, 0.0, static_cast<double>(frameSize.width));
    const double top = std::clamp(rect.y, 0.0, static_cast<double>(frameSize.height));
    const double right = std::clamp(rect.x + rect.width, 0.0, static_cast<double>(frameSize.width));
    const double bottom = std::clamp(rect.y + rect.height, 0.0, static_cast<double>(frameSize.height));
    if (right <= left || bottom <= top) {
        return cv::Rect2d();
    }
    return cv::Rect2d(left, top, right - left, bottom - top);
}

double smoothedYoloSize(double previous, double detected, double scale, bool snap)
{
    if (snap || previous <= 1.0) {
        return detected;
    }

    const double diff = detected - previous;
    const double deadZone = std::max(kYoloSizeDeadZonePixels, scale * kYoloSizeDeadZoneRatio);
    if (std::abs(diff) <= deadZone) {
        return previous;
    }

    const bool expanding = diff > 0.0;
    const double changeRatio = std::abs(diff) / std::max(1.0, previous);
    double alpha = expanding ? 0.42 : 0.16;
    if (changeRatio > 0.25) {
        alpha = expanding ? 0.62 : 0.30;
    }
    return previous + diff * alpha;
}

cv::Rect2d smoothYoloTargetRect(
    const cv::Rect2d &previous,
    const cv::Rect2d &detected,
    const cv::Size &frameSize,
    bool snap)
{
    const cv::Rect2d boundedDetected = boundedRect(detected, frameSize);
    if (boundedDetected.width <= 1.0 || boundedDetected.height <= 1.0) {
        return cv::Rect2d();
    }
    if (snap || previous.width <= 1.0 || previous.height <= 1.0) {
        return boundedDetected;
    }

    const double prevCx = previous.x + previous.width * 0.5;
    const double prevCy = previous.y + previous.height * 0.5;
    const double detCx = boundedDetected.x + boundedDetected.width * 0.5;
    const double detCy = boundedDetected.y + boundedDetected.height * 0.5;
    const double movement = std::hypot(detCx - prevCx, detCy - prevCy);
    const double targetScale = std::max(20.0, std::max(previous.width, previous.height));
    const double movementRatio = movement / targetScale;

    double centerAlpha = 0.72;
    if (movementRatio > 0.45) {
        centerAlpha = 1.0;
    } else if (movementRatio > 0.18) {
        centerAlpha = 0.92;
    } else if (movementRatio > 0.06) {
        centerAlpha = 0.82;
    }

    const double width = smoothedYoloSize(previous.width, boundedDetected.width, targetScale, false);
    const double height = smoothedYoloSize(previous.height, boundedDetected.height, targetScale, false);
    const double centerX = prevCx + (detCx - prevCx) * centerAlpha;
    const double centerY = prevCy + (detCy - prevCy) * centerAlpha;
    return boundedRect(
        cv::Rect2d(centerX - width * 0.5, centerY - height * 0.5, width, height),
        frameSize);
}
}

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
    return m_isComputing || m_hasNewFrame || m_needInit || m_needDetection;
}

void DnnThread::requestDetections(const cv::Mat &frame, quint64 requestId) {
    QMutexLocker locker(&m_mutex);
    if (frame.empty()) return;
    m_detectFrame = frame.clone();
    m_detectionRequestId = requestId;
    m_needDetection = true;
}

void DnnThread::updateDnn(const cv::Mat &frame) {
    QMutexLocker locker(&m_mutex);
    if (!m_isTracking || frame.empty()) return;
    m_frame = frame;
    m_hasNewFrame = true;
}


void DnnThread::initDnn(const cv::Mat &frame, const cv::Rect2d &target) {
    QMutexLocker locker(&m_mutex);
    if(frame.empty()) return;
    m_initFrame = frame;
    m_initRect = target;
    m_needInit = true;
    m_isTracking = true;
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
}


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
                const cv::Rect bestBox = boxes[best_idx] & cv::Rect(0, 0, processFrame.cols, processFrame.rows);
                m_lastYoloRect = boundedRect(
                    cv::Rect2d(bestBox.x, bestBox.y, bestBox.width, bestBox.height),
                    processFrame.size());

                QString cName = QString::fromStdString(m_classNames[m_lockedClassId]);
                emit dnnTrackedResult(m_lastYoloRect, true, "LOCK:" + cName); // 特殊前缀用于触发锁定日志
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
            int missCountSnapshot = 0;

            {
                QMutexLocker locker(&m_mutex);
                currentlyTracking = m_isTracking;
                useYolo = m_useYolo;
                lockedClass = m_lockedClassId;
                lastYoloRect = m_lastYoloRect;
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
                        cv::Rect2d detectedRect(best_r.x, best_r.y, best_r.width, best_r.height);
                        const bool recoveredAfterMiss =
                            acceptedByRecovery || missCountSnapshot >= kYoloGlobalRecoverFrames;
                        cv::Rect2d outputRect = smoothYoloTargetRect(
                            lastYoloRect,
                            detectedRect,
                            processFrame.size(),
                            recoveredAfterMiss);
                        {
                            QMutexLocker locker(&m_mutex);
                            m_lastYoloRect = outputRect;
                            m_dnnMissCount = 0;
                        }

                        QString cName = QString::fromStdString(m_classNames[lockedClass]);
                        emit dnnTrackedResult(
                            outputRect,
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
                                holdRect = m_lastYoloRect;
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


bool DnnThread::isWarmedUp() {
    QMutexLocker locker(&m_mutex);
    return m_isWarmedUp;
}


