import shutil
import re
import os

with open("header_fix.txt", "r", encoding="utf-16") as f:
    content = f.read()

# 1. Add include
content = content.replace("#include <QMainWindow>", '#include <QMainWindow>\n#include "serialcontroller.h"')

# 2. Remove QSerialPort include just in case
content = content.replace("#include <QSerialPort>\n", "")

# 3. Remove messlot
content = re.sub(r"\s*void messlot\(\);", "", content)

# 4. Replace m_serial and m_rx_buffer
content = content.replace("QSerialPort m_serial;", "SerialController m_serialController;")
content = re.sub(r"\s*QByteArray m_rx_buffer;", "", content)

with open("mainwindow.h", "w", encoding="utf-8") as f:
    f.write(content)

print("mainwindow.h fixed.")
