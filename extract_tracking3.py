import re

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
    # Matches return_type MainWindow::methodName(args) { ... }
    # Added \* and \& to the return type regex
    pattern = r"((?:[\w\:\<\>\,]+\s*\*?\s*)+)MainWindow::" + method_name + r"\s*\((.*?)\)(?:\s*const)?\s*\{"
    match = re.search(pattern, cpp_text, re.MULTILINE | re.DOTALL)
    if not match:
        print(f"Failed to find {method_name}")
        return None, None, None, None
    
    start_idx = match.start()
    ret_type = match.group(1).strip()
    args = match.group(2).strip()
    
    idx = match.end() - 1
    brace_count = 0
    end_idx = -1
    for i in range(idx, len(cpp_text)):
        if cpp_text[i] == '{':
            brace_count += 1
        elif cpp_text[i] == '}':
            brace_count -= 1
            if brace_count == 0:
                end_idx = i + 1
                break
                
    if end_idx == -1:
        return None, None, None, None
        
    full_body = cpp_text[start_idx:end_idx]
    is_const = "const" in cpp_text[match.end(2):match.end(2)+15] 
    return full_body, ret_type, args, is_const

extracted_bodies = {}
for m in methods_to_extract:
    body, ret_type, args, is_const = extract_method_body(m, cpp_content)
    if body:
        extracted_bodies[m] = {"body": body}

print(f"Extracted {len(extracted_bodies)} methods.")

cpp_out = '#include "trackingengine.h"\n#include <QDateTime>\n#include <QDebug>\n\n'
cpp_out += 'TrackingEngine::TrackingEngine(QObject *parent) : QObject(parent), m_dnnThread(nullptr) {\n'
cpp_out += '    m_dnnThreads.clear();\n'
cpp_out += '}\n\n'
cpp_out += 'TrackingEngine::~TrackingEngine() { stopTracking(); }\n\n'

# Put static helper functions here!
helpers = []
for h in ["confidenceThresholdForClass", "boundedIntRect"]:
    # These are not MainWindow:: methods, they are static/free functions
    pattern = r"(float|cv::Rect)\s+" + h + r"\s*\(.*?\)\s*\{"
    match = re.search(pattern, cpp_content, re.MULTILINE | re.DOTALL)
    if match:
        start_idx = match.start()
        idx = match.end() - 1
        brace_count = 0
        end_idx = -1
        for i in range(idx, len(cpp_content)):
            if cpp_content[i] == '{':
                brace_count += 1
            elif cpp_content[i] == '}':
                brace_count -= 1
                if brace_count == 0:
                    end_idx = i + 1
                    break
        if end_idx != -1:
            body = cpp_content[start_idx:end_idx]
            # Since we will just keep them as static functions in trackingengine.cpp
            cpp_out += body + "\n\n"

for m in methods_to_extract:
    if m not in extracted_bodies: continue
    body = extracted_bodies[m]["body"]
    body = body.replace(f"MainWindow::{m}", f"TrackingEngine::{m}")
    body = re.sub(r"if\s*\(\s*ui->plainTextEdit_2\s*\)\s*\{[\s\n]*ui->plainTextEdit_2->appendPlainText\((.*?)\);[\s\n]*\}", r"emit logMessage(\1);", body, flags=re.DOTALL)
    body = re.sub(r"ui->plainTextEdit_2->appendPlainText\((.*?)\);", r"emit logMessage(\1);", body, flags=re.DOTALL)
    body = re.sub(r"ui->plainTextEdit->setPlainText\((.*?)\);", r"emit logMessage(\1);", body, flags=re.DOTALL)
    cpp_out += body + "\n\n"

with open(r"d:\QT\project\untitled7\trackingengine.cpp", "w", encoding="utf-8") as f:
    f.write(cpp_out)

print("Wrote trackingengine.cpp")
