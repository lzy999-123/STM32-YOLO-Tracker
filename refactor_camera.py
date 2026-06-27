import os
import re

h_file = r"d:\QT\project\untitled7\mainwindow.h"
with open(h_file, "r", encoding="utf-8") as f:
    h_content = f.read()

# Add include
h_content = h_content.replace('#include "serialcontroller.h"', '#include "serialcontroller.h"\n#include "cameramanager.h"')

# Remove CameraState enum
h_content = re.sub(r"\s*enum class CameraState \{ Idle, Opening, Open, Closing, Error \};", "", h_content)

# Replace fields
fields_to_remove = [
    r"\s*QCamera \*m_camera;",
    r"\s*QMediaCaptureSession \*m_captureSession;",
    r"\s*QVideoWidget \*m_videoWidget;",
    r"\s*QVideoSink \*m_videoSink;",
    r"\s*QMutex m_cameraMutex;",
    r"\s*QTimer \*m_cameraCheckTimer;",
    r"\s*qint64 m_lastFrameTime;",
    r"\s*QString m_currentCameraId;",
    r"\s*CameraState m_cameraState\s*=\s*CameraState::Idle;",
    r"\s*void emergencyCameraStop\(\);"
]

for field in fields_to_remove:
    h_content = re.sub(field, "", h_content)

# Add m_cameraManager
h_content = h_content.replace("SerialController m_serialController;", "SerialController m_serialController;\n    CameraManager m_cameraManager;")

with open(h_file, "w", encoding="utf-8") as f:
    f.write(h_content)

cpp_file = r"d:\QT\project\untitled7\mainwindow.cpp"
with open(cpp_file, "r", encoding="utf-8") as f:
    cpp_content = f.read()

# Remove constructor initializations
cpp_content = re.sub(r", m_camera\(nullptr\)", "", cpp_content)
cpp_content = re.sub(r", m_captureSession\(new QMediaCaptureSession\(this\)\)", "", cpp_content)
cpp_content = re.sub(r", m_videoWidget\(nullptr\)", "", cpp_content)
cpp_content = re.sub(r", m_videoSink\(new QVideoSink\(this\)\)", "", cpp_content)
cpp_content = re.sub(r", m_cameraState\(CameraState::Idle\)", "", cpp_content)

# Remove constructor body camera setup
cam_setup = r"m_captureSession->setVideoSink\(m_videoSink\);\s*connect\(m_videoSink, &QVideoSink::videoFrameChanged, this, &MainWindow::handleNewVideoFrame\);\s*m_cameraCheckTimer = new QTimer\(this\);[\s\S]*?m_cameraCheckTimer->start\(\);"
cpp_content = re.sub(cam_setup, "connect(&m_cameraManager, &CameraManager::frameReady, this, &MainWindow::handleNewVideoFrame);\n    m_cameraManager.startChecking();\n    connect(&m_cameraManager, &CameraManager::stateChanged, this, [this](CameraManager::CameraState state) {\n        if (state == CameraManager::CameraState::Idle) {\n            ui->pushButton_9->setText(\"打开摄像头\");\n            ui->pushButton_9->setEnabled(true);\n        } else if (state == CameraManager::CameraState::Open) {\n            ui->pushButton_9->setText(\"关闭摄像头\");\n            ui->pushButton_9->setEnabled(true);\n        }\n    });", cpp_content)

# Replace closeEvent cleanup
cam_cleanup = r"m_cameraCheckTimer->stop\(\);\s*\{[\s\S]*?m_camera = nullptr;\s*\}"
cpp_content = re.sub(cam_cleanup, "m_cameraManager.closeCamera();", cpp_content)

# Replace on_pushButton_9_clicked
push9_pattern = r"void MainWindow::on_pushButton_9_clicked\(\)\s*\{[\s\S]*?catch \(\.\.\.\) \{ \}\s*\}"
push9_replace = r"""void MainWindow::on_pushButton_9_clicked()
{
    if (m_cameraManager.state() == CameraManager::CameraState::Idle || m_cameraManager.state() == CameraManager::CameraState::Error) {
        m_cameraManager.openCamera(ui->comboBox_3->currentData().toString());
        ui->pushButton_9->setText("正在打开...");
        ui->pushButton_9->setEnabled(false);
    } else {
        m_cameraManager.closeCamera();
        ui->pushButton_9->setText("正在关闭...");
        ui->pushButton_9->setEnabled(false);
    }
}"""
cpp_content = re.sub(push9_pattern, push9_replace, cpp_content)

# Remove emergencyCameraStop
emerg_pattern = r"void MainWindow::emergencyCameraStop\(\)\s*\{[\s\S]*?m_camera = nullptr;\s*\}"
cpp_content = re.sub(emerg_pattern, "", cpp_content)

# Replace onCameraChanged
camchange_pattern = r"void MainWindow::onCameraChanged\(int index\)\s*\{[\s\S]*?catch \(\.\.\.\) \{ m_cameraState = CameraState::Idle; ui->pushButton_9->setEnabled\(true\); ui->pushButton_9->setText\(\"打开摄像头\"\); return; \}\s*\}"
camchange_replace = r"""void MainWindow::onCameraChanged(int index)
{
    if (index < 0) return;
    if (m_cameraManager.state() == CameraManager::CameraState::Open) {
        m_cameraManager.closeCamera();
        m_cameraManager.openCamera(ui->comboBox_3->itemData(index).toString());
    }
}"""
cpp_content = re.sub(camchange_pattern, camchange_replace, cpp_content)

with open(cpp_file, "w", encoding="utf-8") as f:
    f.write(cpp_content)

print("mainwindow.cpp/h refactored for CameraManager.")
