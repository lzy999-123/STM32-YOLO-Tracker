import re
import os

cpp_file = r"d:\QT\project\untitled7\mainwindow.cpp"
h_file = r"d:\QT\project\untitled7\mainwindow.h"

with open(cpp_file, "r", encoding="utf-8") as f:
    cpp_content = f.read()

with open(h_file, "r", encoding="utf-8") as f:
    h_content = f.read()

print(f"Read files. cpp len={len(cpp_content)}, h len={len(h_content)}")
