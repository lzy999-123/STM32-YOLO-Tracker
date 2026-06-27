import os
import re

file_path = r"d:\QT\project\untitled7\mainwindow.cpp"
with open(file_path, "r", encoding="utf-8") as f:
    content = f.read()

# Replace sendCommand implementation
content = re.sub(
    r"void MainWindow::sendCommand\(uint8_t cmd\)\s*\{\s*if\s*\(!m_serial\.isOpen\(\)\)\s*return;\s*QByteArray cmdData;.*?\s*m_serial\.write\(cmdData\);\s*\}",
    "void MainWindow::sendCommand(uint8_t cmd) {\n    m_serialController.sendCommand(cmd);\n}",
    content,
    flags=re.DOTALL
)

# Replace m_serial.isOpen() -> m_serialController.isOpen()
content = content.replace("m_serial.isOpen()", "m_serialController.isOpen()")

# Replace sendTrackData
track_data_pattern = r"uint8_t txBuf\[6\] = \{ 0xFF, \(uint8_t\)\(m_offsetX >> 8\), \(uint8_t\)\(m_offsetX & 0xFF\), \(uint8_t\)\(m_offsetY >> 8\), \(uint8_t\)\(m_offsetY & 0xFF\), 0xFE \};\s*m_serial\.write\(\(char\*\)txBuf, 6\);"
track_data_replace = r"m_serialController.sendTrackData(m_offsetX, m_offsetY);"
content = re.sub(track_data_pattern, track_data_replace, content)

# Replace serial setup in pushButton_clicked
setup_pattern = r"m_serial\.setPortName\(ui->comboBox->currentText\(\)\);\s*m_serial\.setBaudRate\(ui->comboBox_2->currentText\(\)\.toInt\(\)\);\s*m_serial\.setDataBits\(QSerialPort::Data8\);\s*m_serial\.setStopBits\(QSerialPort::OneStop\);\s*m_serial\.setParity\(QSerialPort::NoParity\);\s*m_serial\.setFlowControl\(QSerialPort::NoFlowControl\);\s*if\(m_serial\.open\(QIODevice::ReadWrite\)==true\)\s*\{"
setup_replace = r"if(m_serialController.openSerial(ui->comboBox->currentText(), ui->comboBox_2->currentText().toInt())) {"
content = re.sub(setup_pattern, setup_replace, content)

# Remove messlot implementation and replace it with lambda or update
# We will just remove messlot implementation completely
messlot_pattern = r"void MainWindow::messlot\(\)\s*\{[\s\S]*?m_rx_buffer\.remove\(0, tail \+ 2\);\s*\}\s*\}"
content = re.sub(messlot_pattern, "", content)

# Replace connect(&m_serial, &QSerialPort::readyRead, this, &MainWindow::messlot);
connect_pattern = r"connect\(&m_serial, &QSerialPort::readyRead, this, &MainWindow::messlot\);"
connect_replace = r"""connect(&m_serialController, &SerialController::telemetryReceived, this, [this](int remoteMode, const QString& param1, const QString& param2) {
                ui->plainTextEdit_3->setPlainText(param1);
                ui->plainTextEdit_4->setPlainText(param2);
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
            });"""
content = re.sub(connect_pattern, connect_replace, content)

# Replace disconnect(&m_serial, &QSerialPort::readyRead, this, &MainWindow::messlot);
disconnect_pattern = r"disconnect\(&m_serial, &QSerialPort::readyRead, this, &MainWindow::messlot\);"
disconnect_replace = r"disconnect(&m_serialController, &SerialController::telemetryReceived, this, nullptr);"
content = re.sub(disconnect_pattern, disconnect_replace, content)

# Replace m_serial.close() -> m_serialController.closeSerial()
content = content.replace("m_serial.close()", "m_serialController.closeSerial()")

# Replace m_serial.waitForReadyRead(50) -> ...
# We'll just remove or skip it, as QSerialPort is now internal. We can replace it with QThread::msleep(50) if needed, but QCoreApplication::processEvents() does the job.
content = content.replace("m_serial.waitForReadyRead(50);", "")
content = content.replace("m_serial.waitForBytesWritten(200);", "")

with open(file_path, "w", encoding="utf-8") as f:
    f.write(content)

print("Done refactoring mainwindow.cpp")
