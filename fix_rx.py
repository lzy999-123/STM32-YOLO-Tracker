import os
file_path = r"d:\QT\project\untitled7\mainwindow.cpp"
with open(file_path, "r", encoding="utf-8") as f:
    content = f.read()

content = content.replace("m_rx_buffer.clear();", "")

with open(file_path, "w", encoding="utf-8") as f:
    f.write(content)

print("m_rx_buffer removed.")
