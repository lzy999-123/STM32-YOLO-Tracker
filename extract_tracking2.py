import re

cpp_file = r"d:\QT\project\untitled7\mainwindow.cpp"
h_file = r"d:\QT\project\untitled7\mainwindow.h"

with open(cpp_file, "r", encoding="utf-8") as f:
    cpp_content = f.read()
with open(h_file, "r", encoding="utf-8") as f:
    h_content = f.read()

# Define the methods to extract
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
    # Needs to handle nested braces
    pattern = r"((?:[\w\:\<\>\,]+\s+)+)MainWindow::" + method_name + r"\s*\((.*?)\)(?:\s*const)?\s*\{"
    match = re.search(pattern, cpp_text, re.MULTILINE | re.DOTALL)
    if not match:
        print(f"Failed to find {method_name}")
        return None, None, None, None
    
    start_idx = match.start()
    ret_type = match.group(1).strip()
    args = match.group(2).strip()
    
    # find the matching closing brace
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
    
    # Determine if it was const
    is_const = "const" in cpp_text[match.end(2):match.end(2)+15] # heuristic
    
    return full_body, ret_type, args, is_const

extracted_bodies = {}
for m in methods_to_extract:
    body, ret_type, args, is_const = extract_method_body(m, cpp_content)
    if body:
        extracted_bodies[m] = {
            "body": body,
            "ret_type": ret_type,
            "args": args,
            "is_const": is_const
        }

print(f"Extracted {len(extracted_bodies)} methods.")

# Transform the bodies to TrackingEngine
cpp_out = '#include "trackingengine.h"\n#include <QDateTime>\n#include <QDebug>\n\n'
cpp_out += 'TrackingEngine::TrackingEngine(QObject *parent) : QObject(parent), m_dnnThread(nullptr) {}\n\n'
cpp_out += 'TrackingEngine::~TrackingEngine() { stopTracking(); }\n\n'

for m in methods_to_extract:
    if m not in extracted_bodies: continue
    
    body = extracted_bodies[m]["body"]
    # Replace MainWindow:: with TrackingEngine::
    body = body.replace(f"MainWindow::{m}", f"TrackingEngine::{m}")
    
    # Replace ui->plainTextEdit_2->appendPlainText with emit logMessage
    # we need to handle multi-line strings carefully, but a regex can do most of it
    body = re.sub(r"if\s*\(\s*ui->plainTextEdit_2\s*\)\s*\{[\s\n]*ui->plainTextEdit_2->appendPlainText\((.*?)\);[\s\n]*\}", r"emit logMessage(\1);", body, flags=re.DOTALL)
    body = re.sub(r"ui->plainTextEdit_2->appendPlainText\((.*?)\);", r"emit logMessage(\1);", body, flags=re.DOTALL)
    
    # Replace ui->plainTextEdit->setPlainText with logMessage
    body = re.sub(r"ui->plainTextEdit->setPlainText\((.*?)\);", r"emit logMessage(\1);", body, flags=re.DOTALL)

    # Some methods use m_frameSize, let's assume TrackingEngine keeps track of it
    # We will just pass frameSize in processFrame, or TrackingEngine keeps m_frameSize
    
    cpp_out += body + "\n\n"

with open(r"d:\QT\project\untitled7\trackingengine_draft.cpp", "w", encoding="utf-8") as f:
    f.write(cpp_out)

print("Wrote trackingengine_draft.cpp")
