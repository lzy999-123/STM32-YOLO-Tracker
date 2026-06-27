import re

h_file = r"d:\QT\project\untitled7\mainwindow.h"
cpp_file = r"d:\QT\project\untitled7\mainwindow.cpp"

with open(h_file, "r", encoding="utf-8") as f:
    h_content = f.read()

with open(cpp_file, "r", encoding="utf-8") as f:
    cpp_content = f.read()

# Extract DnnThread from mainwindow.h
dnn_h_start = h_content.find("class DnnThread : public QThread")
dnn_h_end = -1
if dnn_h_start != -1:
    brace_idx = h_content.find("{", dnn_h_start)
    brace_count = 0
    for i in range(brace_idx, len(h_content)):
        if h_content[i] == '{':
            brace_count += 1
        elif h_content[i] == '}':
            brace_count -= 1
            if brace_count == 0:
                dnn_h_end = h_content.find(";", i) + 1
                break

if dnn_h_end != -1:
    dnn_h_code = h_content[dnn_h_start:dnn_h_end]
    # Remove from mainwindow.h
    h_content = h_content[:dnn_h_start] + h_content[dnn_h_end:]
    
    # Save to dnnthread.h
    dnn_h_full = f"""#ifndef DNNTHREAD_H
#define DNNTHREAD_H

#include <QThread>
#include <QMutex>
#include <QImage>
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>

{dnn_h_code}

#endif // DNNTHREAD_H
"""
    with open(r"d:\QT\project\untitled7\dnnthread.h", "w", encoding="utf-8") as f:
        f.write(dnn_h_full)
    with open(h_file, "w", encoding="utf-8") as f:
        f.write(h_content)
    print("Extracted dnnthread.h")

# Extract DnnThread implementation from mainwindow.cpp
# Since DnnThread methods start with DnnThread::, we can just extract everything that matches DnnThread::
# It's better to just extract the block if it's contiguous, but it might be scattered.
# Actually, the DnnThread constructor and methods are likely contiguous.
dnn_cpp_code = ""
methods = ["DnnThread", "~DnnThread", "updateDnn", "initDnn", "stopDnn", "run", "warmupModel", "receiveFrame", "isWarmedUp", "isNetEmpty"]

for m in methods:
    search_str = "DnnThread::" + m + "("
    idx = cpp_content.find(search_str)
    if idx == -1: continue
    start_idx = cpp_content.rfind("\n", 0, idx)
    if start_idx == -1: start_idx = 0
    brace_idx = cpp_content.find("{", idx)
    if brace_idx == -1: continue
    brace_count = 0
    end_idx = -1
    for i in range(brace_idx, len(cpp_content)):
        if cpp_content[i] == '{':
            brace_count += 1
        elif cpp_content[i] == '}':
            brace_count -= 1
            if brace_count == 0:
                end_idx = i + 1
                break
    if end_idx != -1:
        dnn_cpp_code += cpp_content[start_idx:end_idx] + "\n\n"
        # Remove from cpp_content
        cpp_content = cpp_content[:start_idx] + "\n" + cpp_content[end_idx:]

if dnn_cpp_code:
    dnn_cpp_full = f"""#include "dnnthread.h"
#include <QDebug>

{dnn_cpp_code}
"""
    with open(r"d:\QT\project\untitled7\dnnthread.cpp", "w", encoding="utf-8") as f:
        f.write(dnn_cpp_full)
    with open(cpp_file, "w", encoding="utf-8") as f:
        f.write(cpp_content)
    print("Extracted dnnthread.cpp")

