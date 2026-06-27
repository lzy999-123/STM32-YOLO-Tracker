import os

cpp_file = r"d:\QT\project\untitled7\mainwindow.cpp"
with open(cpp_file, "r", encoding="utf-8") as f:
    cpp_content = f.read()

methods_to_extract = [
    "onDnnResultReceived", "onYoloDetectionResult", "restartDnnThread", "preloadYoloModels", "ensureDnnThread",
    "resetFeatureTracker", "initFeatureTracker", "updateFeatureTracker", "recoverFeatureTracker",
    "buildFeatureReference", "featurePatchSimilarity", "featureColorSimilarity", "featureShapeSimilarity",
    "featureCandidateScore", "alignFeatureRectInYoloCandidate", "evaluateYoloFeatureCandidate",
    "featureCandidatePasses", "confirmFeatureRecoveryCandidate", "requestFeatureYoloTargetClassification",
    "requestFeatureYoloRecovery", "nextFeatureYoloRequestId", "resetFeatureRecoveryCandidate",
    "resetFeatureYoloTargetClassification", "recoverFeatureTrackerByOrb", "recoverFeatureTrackerByTemplate",
    "recoverFeatureTrackerByColor", "restartFeatureTrackerFromRect"
]

def extract_method_body(method_name, cpp_text):
    # Find the string "MainWindow::method_name("
    search_str = "MainWindow::" + method_name + "("
    idx = cpp_text.find(search_str)
    if idx == -1:
        # try without parenthesis if there's space
        print(f"Failed to find {method_name}")
        return None
    
    # backtrack to the beginning of the return type. Just find the previous newline
    start_idx = cpp_text.rfind("\n", 0, idx)
    if start_idx == -1:
        start_idx = 0
    else:
        start_idx += 1
        
    # find the opening brace '{'
    brace_idx = cpp_text.find("{", idx)
    if brace_idx == -1:
        return None
        
    brace_count = 0
    end_idx = -1
    for i in range(brace_idx, len(cpp_text)):
        if cpp_text[i] == '{':
            brace_count += 1
        elif cpp_text[i] == '}':
            brace_count -= 1
            if brace_count == 0:
                end_idx = i + 1
                break
                
    if end_idx == -1:
        return None
        
    return cpp_text[start_idx:end_idx]

extracted_bodies = {}
for m in methods_to_extract:
    body = extract_method_body(m, cpp_content)
    if body:
        extracted_bodies[m] = {"body": body}

print(f"Extracted {len(extracted_bodies)} methods.")

cpp_out = '#include "trackingengine.h"\n#include <QDateTime>\n#include <QDebug>\n\n'
cpp_out += 'TrackingEngine::TrackingEngine(QObject *parent) : QObject(parent), m_dnnThread(nullptr) {\n'
cpp_out += '    m_dnnThreads.clear();\n'
cpp_out += '}\n\n'
cpp_out += 'TrackingEngine::~TrackingEngine() { stopTracking(); }\n\n'

# Find helpers boundedIntRect and confidenceThresholdForClass
for h in ["confidenceThresholdForClass", "boundedIntRect"]:
    search_str = " " + h + "("
    idx = cpp_content.find(search_str)
    if idx == -1: continue
    start_idx = cpp_content.rfind("\n", 0, idx)
    if start_idx == -1: start_idx = 0
    else: start_idx += 1
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
        body = cpp_content[start_idx:end_idx]
        cpp_out += body + "\n\n"

for m in methods_to_extract:
    if m not in extracted_bodies: continue
    body = extracted_bodies[m]["body"]
    body = body.replace(f"MainWindow::{m}", f"TrackingEngine::{m}")
    import re
    body = re.sub(r"if\s*\(\s*ui->plainTextEdit_2\s*\)\s*\{[\s\n]*ui->plainTextEdit_2->appendPlainText\((.*?)\);[\s\n]*\}", r"emit logMessage(\1);", body, flags=re.DOTALL)
    body = re.sub(r"ui->plainTextEdit_2->appendPlainText\((.*?)\);", r"emit logMessage(\1);", body, flags=re.DOTALL)
    body = re.sub(r"ui->plainTextEdit->setPlainText\((.*?)\);", r"emit logMessage(\1);", body, flags=re.DOTALL)
    cpp_out += body + "\n\n"

with open(r"d:\QT\project\untitled7\trackingengine.cpp", "w", encoding="utf-8") as f:
    f.write(cpp_out)

print("Wrote trackingengine.cpp")
