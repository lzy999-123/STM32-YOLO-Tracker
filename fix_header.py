import os

file_path = r"d:\QT\project\untitled7\mainwindow.h"
with open(file_path, "r", encoding="utf-8") as f:
    lines = f.readlines()

new_lines = []
skip = False
for i, line in enumerate(lines):
    # DnnThread ends with double m_relH; };
    # The corrupted part is having multiple MainWindow constructors and a fake MainWindow class.
    pass

# Actually, let's just use regex to clean up the file
with open(file_path, "r", encoding="utf-8") as f:
    content = f.read()

import re

# The end of DnnThread should be exactly:
#     double m_relX;
#     double m_relY;
#     double m_relW;
#     double m_relH;
# };

# Let's fix the corrupted DnnThread end:
broken_dnn = r"    double m_relX;\s*double m_relX;\s*double m_relY;\s*double m_relW;\s*double m_relH;\s*\};"
content = re.sub(broken_dnn, "    double m_relX;\n    double m_relY;\n    double m_relW;\n    double m_relH;\n};", content)

# Now remove the duplicated MainWindow constructor if it exists:
# The user error showed: MainWindow(QWidget *parent = nullptr); ~MainWindow(); being duplicated.
# Let's look for exactly that.
dup_construct = r"(    MainWindow\(QWidget \*parent = nullptr\);\s*~MainWindow\(\);\s*static cv::Mat QImageToCvMat\(const QImage& qImage\);\s*)"
# If it appears twice in a row (with optional whitespace)
content = re.sub(dup_construct + r"(?:cv::Mat QVideoFrameToCvMat\(const QVideoFrame &frame\);\s*static QImage CvMatToQImage\(const cv::Mat& mat\);\s*)?" + dup_construct, r"\1", content)

with open(file_path, "w", encoding="utf-8") as f:
    f.write(content)

print("Attempted cleanup.")
