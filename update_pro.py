import re

pro_file = r"d:\QT\project\untitled7\untitled7.pro"
with open(pro_file, "r", encoding="utf-8") as f:
    content = f.read()

# Add to SOURCES
if "dnnthread.cpp" not in content:
    content = content.replace("cameramanager.cpp", "cameramanager.cpp \\\n    trackingengine.cpp \\\n    dnnthread.cpp")

# Add to HEADERS
if "dnnthread.h" not in content:
    content = content.replace("cameramanager.h", "cameramanager.h \\\n    trackingengine.h \\\n    dnnthread.h")

with open(pro_file, "w", encoding="utf-8") as f:
    f.write(content)

print("Updated untitled7.pro")
