import os

cpp_file = r"d:\QT\project\untitled7\mainwindow.cpp"
with open(cpp_file, "r", encoding="utf-8") as f:
    content = f.read()

# Replace m_cameraState usages
content = content.replace("m_cameraState == CameraState::Open", "m_cameraManager.state() == CameraManager::CameraState::Open")
content = content.replace("m_cameraState != CameraState::Open", "m_cameraManager.state() != CameraManager::CameraState::Open")

# Replace isActive checks
content = content.replace("!m_camera || !m_camera->isActive()", "m_cameraManager.state() != CameraManager::CameraState::Open")

# Replace the cameraOk assignment logic
content = content.replace("{ QMutexLocker locker(&m_cameraMutex); cameraOk = m_camera && m_camera->isActive() && m_cameraManager.state() == CameraManager::CameraState::Open; }", "cameraOk = (m_cameraManager.state() == CameraManager::CameraState::Open);")

# Replace m_lastFrame lock (since we no longer have m_cameraMutex or m_camera in MainWindow)
content = content.replace("{ QMutexLocker locker(&m_cameraMutex); if (m_cameraManager.state() != CameraManager::CameraState::Open) return; m_lastFrame = cvMat.clone(); }", "if (m_cameraManager.state() != CameraManager::CameraState::Open) return; m_lastFrame = cvMat.clone();")

content = content.replace("{ QMutexLocker locker(&m_cameraMutex); if (!m_lastFrame.empty()) currentFrameClone = m_lastFrame.clone(); }", "if (!m_lastFrame.empty()) currentFrameClone = m_lastFrame.clone();")

# Remove extra mutex lockers
content = content.replace("QMutexLocker locker(&m_cameraMutex);", "")

# Remove stray m_cameraState assignments
content = content.replace("m_cameraState = CameraState::Closing;", "")
content = content.replace("{ if (m_camera) m_camera->stop(); }", "")

with open(cpp_file, "w", encoding="utf-8") as f:
    f.write(content)

print("Remaining camera code fixed.")
