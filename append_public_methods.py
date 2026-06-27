import os

cpp_file = r"d:\QT\project\untitled7\trackingengine.cpp"

with open(cpp_file, "r", encoding="utf-8") as f:
    content = f.read()

append_code = """
void TrackingEngine::init(const QSize &frameSize) {
    m_frameSize = frameSize;
}

void TrackingEngine::setFrameSize(const QSize &size) {
    m_frameSize = size;
}

void TrackingEngine::setTrackingBackend(bool useFeatureTracking, const QString &modelFileName) {
    m_useFeatureTracking = useFeatureTracking;
    m_currentModelName = modelFileName;
}

void TrackingEngine::startTracking(const cv::Mat &frame, const cv::Rect2d &targetRect, bool useFeatureTracking, const QString &modelFileName) {
    m_useFeatureTracking = useFeatureTracking;
    m_currentModelName = modelFileName;
    m_selectedRect = targetRect;
    
    if (!m_useFeatureTracking && (!m_dnnThread || m_dnnThread->isNetEmpty())) {
        emit logMessage("❌【错误】YOLO 模型加载失败或未准备好！");
        return;
    }
    
    m_isCapturing = true;
    m_lostFrameCount = 0;
    m_isTargetTracked = false;
    
    if (m_useFeatureTracking) {
        if (!m_featureYoloClassKnown && !m_featureYoloClassifyPending) {
            requestFeatureYoloTargetClassification(frame);
        }
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
    if (!m_isCapturing) return;
    
    if (m_useFeatureTracking) {
        if (!updateFeatureTracker(frame)) {
            if (!recoverFeatureTracker(frame)) {
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
        } else {
            m_isTargetTracked = true;
            m_lostFrameCount = 0;
        }
        
        if (m_isTargetTracked) {
            // Calculate offsets
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
            emit targetTracked(m_trackedRect, m_offsetX, m_offsetY);
        }
    } else {
        // YOLO logic is handled by dnnThread async, it calls onDnnResultReceived
        // But we might need to send frames to it
        if (m_dnnThread && m_dnnThread->isWarmedUp()) {
            m_dnnThread->receiveFrame(frame);
        }
        if (m_isTargetTracked && m_trackedRect.width > 0) {
             emit targetTracked(m_trackedRect, m_offsetX, m_offsetY);
        } else if (!m_isTargetTracked) {
             emit targetLost();
        }
    }
}

bool TrackingEngine::isTracking() const { return m_isTargetTracked; }
cv::Rect2d TrackingEngine::currentTrackedRect() const { return m_trackedRect; }
int TrackingEngine::currentOffsetX() const { return m_offsetX; }
int TrackingEngine::currentOffsetY() const { return m_offsetY; }
bool TrackingEngine::isFeatureTrackingSelected() const { return m_useFeatureTracking; }

"""

content += append_code

with open(cpp_file, "w", encoding="utf-8") as f:
    f.write(content)

print("Appended TrackingEngine methods")
