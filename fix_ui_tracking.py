import re

cpp_file = r"d:\QT\project\untitled7\trackingengine.cpp"

with open(cpp_file, "r", encoding="utf-8") as f:
    content = f.read()

# Replace ui->btnStartTracking usages with signals or remove them
# In MainWindow, trackingengine runs them, but trackingengine shouldn't know about UI.
# In TrackingEngine::preloadYoloModels():
# "ui->btnStartTracking->setEnabled(true);" -> remove
# "ui->btnStartTracking->setToolTip(QString());" -> remove
# "ui->btnStartTracking->setEnabled(isFeatureTrackingSelected());" -> remove
# "ui->btnStartTracking->setToolTip(QStringLiteral("YOLO 推理引擎正在后台预热"));" -> remove
content = re.sub(r"\s*ui->btnStartTracking->.*?;", "", content)

# In TrackingEngine::restartDnnThread():
# const bool wasAutoTracking = m_isCapturing && ui->label_11->text() == "自动";
# We can just ignore wasAutoTracking sending commands here since TrackingEngine doesn't handle UI auto tracking logic,
# actually `wasAutoTracking` was used to `sendCommand(0x11)` inside `restartDnnThread`.
# Let's remove the `wasAutoTracking` logic from restartDnnThread, we can emit a signal or just skip it.
content = re.sub(r"const bool wasAutoTracking = m_isCapturing && ui->label_11->text\(\) == \"自动\";\s*if\s*\(wasAutoTracking\)\s*\{\s*sendCommand\(0x11\);\s*\}", "", content)
content = re.sub(r"const bool wasAutoTracking = m_isCapturing && ui->label_11->text\(\) == \"自动\";", "", content)

# Also ensure "sendCommand" is removed from trackingengine.cpp
content = re.sub(r"sendCommand\(.*?\);", "", content)

# We should also emit yoloWarmupFinished() which is defined in trackingengine.h
# ui->btnStartTracking->setEnabled(true) was at the end of preloadYoloModels
content = content.replace('emit logMessage("【系统】YOLO 模型已准备完毕");', 'emit logMessage("【系统】YOLO 模型已准备完毕");\n    emit yoloWarmupFinished(m_currentModelName);')

with open(cpp_file, "w", encoding="utf-8") as f:
    f.write(content)

print("Fixed ui in trackingengine.cpp")
