import re

cpp_file = r"d:\QT\project\untitled7\mainwindow.cpp"
with open(cpp_file, "r", encoding="utf-8") as f:
    cpp_content = f.read()

# 1. Constructor connections
# Find MainWindow constructor
ctor_idx = cpp_content.find("MainWindow::MainWindow")
brace_idx = cpp_content.find("{", ctor_idx)

# Append signal connections at the end of constructor
end_brace_idx = -1
brace_count = 0
for i in range(brace_idx, len(cpp_content)):
    if cpp_content[i] == '{':
        brace_count += 1
    elif cpp_content[i] == '}':
        brace_count -= 1
        if brace_count == 0:
            end_brace_idx = i
            break

connections = """
    connect(&m_trackingEngine, &TrackingEngine::targetTracked, this, [this](const cv::Rect2d &rect, int offsetX, int offsetY) {
        m_trackedRect = rect;
        m_isTargetTracked = true;
        m_offsetX = offsetX;
        m_offsetY = offsetY;
    });
    connect(&m_trackingEngine, &TrackingEngine::targetLost, this, [this]() {
        m_isTargetTracked = false;
        m_trackedRect = cv::Rect2d();
        m_offsetX = 0;
        m_offsetY = 0;
    });
    connect(&m_trackingEngine, &TrackingEngine::logMessage, this, [this](const QString &msg) {
        if (ui->plainTextEdit_2) {
            ui->plainTextEdit_2->appendPlainText(msg);
        }
    });
"""
cpp_content = cpp_content[:end_brace_idx] + connections + cpp_content[end_brace_idx:]


# 2. Rewrite processLatestVideoFrame tracking part
# Let's replace the whole block from `if (m_forceResetTracking)` to `m_lastFrame = cvMat.clone();`
search_str = "if (m_forceResetTracking) {"
idx1 = cpp_content.find(search_str)
idx2 = cpp_content.find("if (m_cameraManager.state() != CameraManager::CameraState::Open) return; m_lastFrame = cvMat.clone();", idx1)
if idx2 != -1:
    idx2 += len("if (m_cameraManager.state() != CameraManager::CameraState::Open) return; m_lastFrame = cvMat.clone();")

replacement = """
    if (m_forceResetTracking) {
        if (m_wasTrackingBeforeDisconn && m_hasSelectedTarget) {
            const cv::Rect2d frameBounds(0, 0, cvMat.cols, cvMat.rows);
            m_selectedRect = m_lastSelectedRect & frameBounds;
            if (m_selectedRect.width >= 8 && m_selectedRect.height >= 8) {
                m_trackingEngine.startTracking(cvMat, m_selectedRect, ui->comboBox->currentText() == "特征跟踪(CSRT)", ui->comboBox->currentText());
                ui->plainTextEdit_2->appendPlainText("【系统】摄像头已恢复，正在重新锁定目标");
            }
        }
        m_forceResetTracking = false;
        m_wasTrackingBeforeDisconn = false;
    }
    
    m_trackingEngine.setFrameSize(m_frameSize);
    m_trackingEngine.processFrame(cvMat);
    
    if (m_cameraManager.state() != CameraManager::CameraState::Open) return;
    m_lastFrame = cvMat.clone();
"""
cpp_content = cpp_content[:idx1] + replacement + cpp_content[idx2:]

# 3. Rewrite on_btnStartTracking_clicked
search_start = "void MainWindow::on_btnStartTracking_clicked() {"
idx_start = cpp_content.find(search_start)
idx_end_start = cpp_content.find("}", idx_start) + 1

new_start_btn = """void MainWindow::on_btnStartTracking_clicked() {
    if (m_cameraManager.state() != CameraManager::CameraState::Open) { QMessageBox::warning(this, "提示", "请先打开摄像头！"); return; }
    if (!m_hasSelectedTarget) { QMessageBox::warning(this, "提示", "请先选择目标！"); return; }

    cv::Mat currentFrameClone;
    if (!m_lastFrame.empty()) currentFrameClone = m_lastFrame.clone();
    if (currentFrameClone.empty()) {
        ui->plainTextEdit_2->appendPlainText("【提示】尚未收到摄像头画面，请稍后重试。");
        return;
    }

    m_isCapturing = true;
    m_wasTrackingBeforeDisconn = true;
    
    bool useFeatureTracking = (ui->comboBox->currentText() == "特征跟踪(CSRT)");
    m_trackingEngine.startTracking(currentFrameClone, m_selectedRect, useFeatureTracking, ui->comboBox->currentText());
    
    if(ui->label_11->text()=="自动") sendCommand(0x11);
    ui->plainTextEdit_2->appendPlainText(
        useFeatureTracking ? QStringLiteral("开始特征跟踪（CSRT + ORB）！")
                           : QStringLiteral("开始纯 YOLOv8 跟踪！"));
}"""
cpp_content = cpp_content[:idx_start] + new_start_btn + cpp_content[idx_end_start:]

# 4. Rewrite on_btnStopTracking_clicked
search_stop = "void MainWindow::on_btnStopTracking_clicked() {"
idx_stop = cpp_content.find(search_stop)
idx_end_stop = cpp_content.find("}", idx_stop) + 1

new_stop_btn = """void MainWindow::on_btnStopTracking_clicked() {
    if (m_cameraManager.state() != CameraManager::CameraState::Open) { return; }
    if (!m_hasSelectedTarget) { return; }
    cv::Mat currentFrameClone;
    if (!m_lastFrame.empty()) currentFrameClone = m_lastFrame.clone();
    
    m_isCapturing = false;
    m_wasTrackingBeforeDisconn = false;
    m_trackingEngine.stopTracking();
    
    m_trackedRect = cv::Rect2d();
    m_isTargetTracked = false;
    m_offsetX = 0; m_offsetY = 0;
    
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
}"""
cpp_content = cpp_content[:idx_stop] + new_stop_btn + cpp_content[idx_end_stop:]

# 5. Fix UI drawing bug: isFeatureTrackingSelected() uses tracking engine now
cpp_content = cpp_content.replace("isFeatureTrackingSelected()", "m_trackingEngine.isFeatureTrackingSelected()")

with open(r"d:\QT\project\untitled7\mainwindow.cpp", "w", encoding="utf-8") as f:
    f.write(cpp_content)

print("Updated mainwindow.cpp with TrackingEngine logic")
