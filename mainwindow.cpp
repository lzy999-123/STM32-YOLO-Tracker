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
#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace cv;
using namespace cv::dnn;

// ==========================================
// 大脑线程实现 (YOLOv8)
// ==========================================
/**
 * @brief 深度学习追踪线程构造函数
 * 初始化追踪器所需的标志位，并加载 YOLOv8 模型。
 * 使用 OpenCL 硬件加速（如有集成显卡），有效降低 CPU 占用和发热。
 */
DnnThread::DnnThread(QObject *parent) : QThread(parent) {
    m_isTracking = false;
    m_needInit = false;
    m_hasNewFrame = false;
    m_isComputing = false;
    m_isWarmedUp = false;
    m_useYolo = false;
    m_lockedClassId = -1;
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
        QString modelPath = QDir(QCoreApplication::applicationDirPath()).filePath("yolov8n.onnx");
        if (!QFileInfo::exists(modelPath)) {
            const QString developmentPath = QDir::current().filePath("yolov8n.onnx");
            if (QFileInfo::exists(developmentPath)) {
                modelPath = developmentPath;
            }
        }
        if (!QFileInfo::exists(modelPath)) {
            throw std::runtime_error(
                QString("YOLO model not found: %1").arg(modelPath).toStdString());
        }
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

void DnnThread::stopDnn() {
    QMutexLocker locker(&m_mutex);
    m_isTracking = false;
    m_needInit = false;
    m_hasNewFrame = false;
    m_initFrame.release();
    m_frame.release();
    m_useYolo = false;
    m_lockedClassId = -1;
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
    if (m_ortSession != nullptr && !isInterruptionRequested()) {
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
                                ? QStringLiteral("YOLO 推理引擎预热完成")
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
        cv::Rect2d initR;

        {
            QMutexLocker locker(&m_mutex);
            if (m_needInit) {
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

            if (max_class_score > 0.25f) {
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

                classIds.push_back(best_class_id);
                confidences.push_back((float)max_class_score);
                boxes.push_back(cv::Rect(left, top, width, height));
            }
        }

        std::vector<int> indices;
        cv::dnn::NMSBoxes(boxes, confidences, 0.25f, 0.4f, indices);

        if (doInit) {
            // 初始化阶段：寻找与用户框选(initR)重合度最高的目标（极大放宽判定条件，适应人类粗糙框选）
            double max_overlap = 0.0;
            int best_idx = -1;
            double min_dist = 1e9;

            for (int idx : indices) {
                cv::Rect det_rect = boxes[idx];
                cv::Rect2d det_rect2d(det_rect.x, det_rect.y, det_rect.width, det_rect.height);
                cv::Rect2d intersection = det_rect2d & initR;
                
                // 核心算法：不论是用户画了一个大框包住了目标，还是画了个小点在目标内部，都能完美匹配
                double overlap_ratio = std::max(
                    intersection.area() / det_rect2d.area(),
                    intersection.area() / initR.area()
                );

                double cx = det_rect.x + det_rect.width / 2.0;
                double cy = det_rect.y + det_rect.height / 2.0;
                double icx = initR.x + initR.width / 2.0;
                double icy = initR.y + initR.height / 2.0;
                double dist = std::sqrt((cx - icx)*(cx - icx) + (cy - icy)*(cy - icy));
                double max_allowable_dist = std::max(150.0, std::max(initR.width, initR.height) * 1.5);

                if (overlap_ratio > 0.05) {
                    if (overlap_ratio > max_overlap) {
                        max_overlap = overlap_ratio;
                        best_idx = idx;
                    }
                } else if (max_overlap <= 0.05 && dist < min_dist && dist < max_allowable_dist) {
                    min_dist = dist;
                    best_idx = idx;
                }
            }

            QMutexLocker locker(&m_mutex);
            // 只要有极其微小的接触(>0.05)，或者距离足够近，就判定为选中该目标
            if (best_idx != -1 &&
                best_idx < static_cast<int>(classIds.size()) &&
                classIds[best_idx] >= 0 &&
                classIds[best_idx] < static_cast<int>(m_classNames.size()) &&
                boxes[best_idx].width > 0 &&
                boxes[best_idx].height > 0) {
                m_lockedClassId = classIds[best_idx];
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

            {
                QMutexLocker locker(&m_mutex);
                currentlyTracking = m_isTracking;
                useYolo = m_useYolo;
                lockedClass = m_lockedClassId;
                lastYoloRect = m_lastYoloRect;
                rx = m_relX; ry = m_relY; rw = m_relW; rh = m_relH;
            }

            if (currentlyTracking) {
                if (!useYolo) {
                    emit dnnTrackedResult(cv::Rect2d(), false, "");
                } else {
                    // 寻找同类目标中距离上一次位置最近的
                    double min_dist = 1e9;
                    int best_idx = -1;
                    double last_cx = lastYoloRect.x + lastYoloRect.width / 2.0;
                    double last_cy = lastYoloRect.y + lastYoloRect.height / 2.0;

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
                        }
                    }

                    const double frameDiagonal = std::hypot(
                        static_cast<double>(processFrame.cols),
                        static_cast<double>(processFrame.rows));
                    const double maxTrackingDistance = std::max(
                        80.0,
                        std::min(frameDiagonal * 0.18,
                                 std::max(lastYoloRect.width, lastYoloRect.height) * 2.5));

                    if (best_idx != -1 && min_dist <= maxTrackingDistance &&
                        lockedClass >= 0 &&
                        lockedClass < static_cast<int>(m_classNames.size())) {
                        cv::Rect best_r = boxes[best_idx];
                        cv::Rect2d resRect(best_r.x, best_r.y, best_r.width, best_r.height);
                        {
                            QMutexLocker locker(&m_mutex);
                            m_lastYoloRect = resRect;
                        }

                        // 将 YOLO 框还原为用户框选的大小和位置
                        cv::Rect2d userAlignedRect(
                            resRect.x + rx * resRect.width,
                            resRect.y + ry * resRect.height,
                            resRect.width * rw,
                            resRect.height * rh
                        );

                        QString cName = QString::fromStdString(m_classNames[lockedClass]);
                        emit dnnTrackedResult(userAlignedRect, true, cName);
                    } else {
                        emit dnnTrackedResult(cv::Rect2d(), false, "");
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
    , m_isCapturing(false)
    , m_isSelecting(false)
    , m_hasSelectedTarget(false)
    , m_currentMode(0)
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

    m_dnnThread = new DnnThread(this);
    ui->btnStartTracking->setEnabled(false);
    ui->btnStartTracking->setToolTip("YOLO 推理引擎正在后台预热");
    connect(m_dnnThread, &DnnThread::dnnTrackedResult, this, &MainWindow::onDnnResultReceived, Qt::QueuedConnection);
    connect(m_dnnThread, &DnnThread::dnnWarmupFinished, this,
            [this](bool success, const QString &message) {
        ui->btnStartTracking->setEnabled(success);
        ui->btnStartTracking->setToolTip(
            success ? QString() : QStringLiteral("YOLO 推理引擎预热失败"));
        ui->plainTextEdit_2->appendPlainText(
            QString(success ? "【系统】%1" : "【警告】%1").arg(message));
    }, Qt::QueuedConnection);
    m_dnnThread->start(QThread::LowPriority);

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
    if (m_dnnThread) {
        delete m_dnnThread;
        m_dnnThread = nullptr;
    }
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
    QVideoFrame frame;
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        frame = m_pendingVideoFrame;
        m_pendingVideoFrame = QVideoFrame();
        m_frameDispatchPending = false;
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
            m_dnnThread && !m_dnnThread->isNetEmpty()) {
            const cv::Rect2d frameBounds(0, 0, cvMat.cols, cvMat.rows);
            m_selectedRect = m_lastSelectedRect & frameBounds;
            if (m_selectedRect.width >= 8 && m_selectedRect.height >= 8) {
                m_trackedRect = cv::Rect2d();
                m_lostFrameCount = 0;
                m_isCapturing = true;
                m_dnnThread->initDnn(cvMat, m_selectedRect);
                ui->plainTextEdit_2->appendPlainText("【系统】摄像头已恢复，正在重新锁定目标");
            }
        }
        m_forceResetTracking = false;
        m_wasTrackingBeforeDisconn = false;
    }
    if (m_isCapturing && m_dnnThread && !m_dnnThread->isInterruptionRequested()) {
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
                        painter.drawText(qtTrackedRect.topLeft() + QPointF(5, -5), "YOLO TRACK");
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
 * @brief 接收深度学习线程(DnnThread)的识别与跟踪结果
 * @param dnnRect  YOLOv8 识别出的目标边界框 (相对于相机原始分辨率)
 * @param success  是否成功跟踪到了目标 (true 表示当前帧有目标, false 表示目标丢失)
 * @param className 目标的分类名称，如 "person", "car"。如果带有 "LOCK:" 前缀表示这是初始锁定的类别
 * 
 * 此槽函数由 DnnThread 发出的 dnnTrackedResult 信号触发。
 * 它负责更新 UI 端的跟踪状态，并将识别框转换为 UI 可以显示的像素坐标。
 */
void MainWindow::onDnnResultReceived(const cv::Rect2d &dnnRect, bool success, const QString &className) {
    static int dnnFrameCount = 0;
    static QElapsedTimer fpsTimer;
    if (!fpsTimer.isValid()) fpsTimer.start();
    dnnFrameCount++;
    if (fpsTimer.elapsed() > 2000) {
        double fps = dnnFrameCount * 1000.0 / fpsTimer.elapsed();
        if (ui->plainTextEdit_2) {
            ui->plainTextEdit_2->appendPlainText(QString("📊 [大脑性能] YOLO 实时推理速度: %1 FPS").arg(QString::number(fps, 'f', 1)));
        }
        dnnFrameCount = 0;
        fpsTimer.restart();
    }

    if (!m_isCapturing) return;

    if (className.startsWith("LOCK:")) {
        QString actualName = className.mid(5);
        if (ui->plainTextEdit_2) {
            ui->plainTextEdit_2->appendPlainText(QString("【系统】YOLO 已识别并锁定目标: %1").arg(actualName));
        }
        // 注意：这里不再 early return，而是继续执行后面的逻辑以初始化 m_trackedRect 和状态
    }

    bool isReasonable = true;
    if (dnnRect.width < 10 || dnnRect.height < 10 ||
        dnnRect.width > m_frameSize.width() * 0.8 ||
        dnnRect.height > m_frameSize.height() * 0.8) {
        isReasonable = false;
    }

    const int DEAD_ZONE = 18;
    bool currentSuccess = (success && isReasonable);

    if (currentSuccess) {
        m_isTargetTracked = true;
        m_lostFrameCount = 0; // 目标找回，清零

        // 自适应滤波：静止时抑制检测抖动，快速移动时立即跟随，
        // 避免固定低 alpha 造成明显拖尾。
        if (m_trackedRect.empty()) {
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
        const int MAX_LOST_TOLERANCE = 15; // 容忍15帧（约0.5秒）的识别丢失

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

void MainWindow::on_btnSelectTarget_clicked() {
    if (!m_camera || !m_camera->isActive()) { QMessageBox::warning(this, "提示", "请先打开摄像头！"); return; }
    if (m_dnnThread) { m_dnnThread->stopDnn(); }
    m_hasSelectedTarget = false; m_trackedRect = cv::Rect2d(); m_isTargetTracked = false; m_isSelecting = true;
    m_lostFrameCount = 0;
    m_selectStart = QPoint(); m_selectEnd = QPoint(); m_selectStartImg = QPoint(); m_selectEndImg = QPoint();
    ui->plainTextEdit_2->appendPlainText("【提示】正在框选，请在画面内拖动鼠标...");
}

void MainWindow::on_btnStartTracking_clicked() {
    if (!m_camera || !m_camera->isActive()) { QMessageBox::warning(this, "提示", "请先打开摄像头！"); return; }
    if (!m_hasSelectedTarget) { QMessageBox::warning(this, "提示", "请先选择目标！"); return; }
    if (!m_dnnThread || m_dnnThread->isNetEmpty()) {
        ui->plainTextEdit_2->appendPlainText(
            "❌【错误】YOLO 模型加载失败，请检查程序目录或当前工作目录中的 yolov8n.onnx！");
        return;
    }
    if (!m_dnnThread->isWarmedUp()) {
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
    m_dnnThread->initDnn(currentFrameClone, m_selectedRect);
    if(ui->label_11->text()=="自动") sendCommand(0x11);
    ui->plainTextEdit_2->appendPlainText("开始纯 YOLOv8 跟踪！");
}

void MainWindow::on_btnStopTracking_clicked() {
    if (!m_camera || !m_camera->isActive()) { return; }
    if (!m_hasSelectedTarget) { return; }
    m_isCapturing = false;
    m_lostFrameCount = 0;
    if (m_dnnThread) m_dnnThread->stopDnn();
    m_trackedRect = cv::Rect2d(); m_wasTrackingBeforeDisconn = false; m_isTargetTracked = false; m_offsetX = 0; m_offsetY = 0;
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

    int maxEdge = std::max(selectRect.width(), selectRect.height());
    if (maxEdge > 180) {
        ui->plainTextEdit_2->appendPlainText("【提示】框选区域较大，可能降低跟踪帧率。建议仅框选核心局部！");
    } else {
        ui->plainTextEdit_2->appendPlainText("【提示】目标模板已锁定。");
    }
}

void MainWindow::paintEvent(QPaintEvent *event) { QMainWindow::paintEvent(event); }

void MainWindow::on_pushButton_2_clicked() {
    QMutexLocker locker(&m_modeMutex);
    ui->pushButton_2->setEnabled(false);
    QTimer::singleShot(100, this, [=]() { ui->pushButton_2->setEnabled(true); });
    m_isCapturing = false;
    if (m_dnnThread) m_dnnThread->stopDnn();
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
                m_trackedRect = cv::Rect2d(); m_isTargetTracked = false; m_offsetX = 0; m_offsetY = 0;
            }
        }
        m_rx_buffer.remove(0, tail + 2);
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_dnnThread) { m_dnnThread->requestInterruption(); m_dnnThread->stopDnn(); m_dnnThread->wait(300); }
    m_cameraState = CameraState::Closing;
    { QMutexLocker locker(&m_cameraMutex); if (m_camera) m_camera->stop(); }
    if (m_serial.isOpen()) { sendCommand(0x12); sendCommand(0x02); m_serial.waitForBytesWritten(200); m_serial.close(); }
    event->accept();
}

