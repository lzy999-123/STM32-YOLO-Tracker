import re

cpp_file = r"d:\QT\project\untitled7\mainwindow.cpp"
with open(cpp_file, "r", encoding="utf-8") as f:
    cpp_content = f.read()

methods_to_remove = [
    "onDnnResultReceived", "onYoloDetectionResult", "restartDnnThread", "preloadYoloModels", "ensureDnnThread",
    "resetFeatureTracker", "initFeatureTracker", "updateFeatureTracker", "recoverFeatureTracker",
    "buildFeatureReference", "featurePatchSimilarity", "featureColorSimilarity", "featureShapeSimilarity",
    "featureCandidateScore", "alignFeatureRectInYoloCandidate", "evaluateYoloFeatureCandidate",
    "featureCandidatePasses", "confirmFeatureRecoveryCandidate", "requestFeatureYoloTargetClassification",
    "requestFeatureYoloRecovery", "nextFeatureYoloRequestId", "resetFeatureRecoveryCandidate",
    "resetFeatureYoloTargetClassification", "recoverFeatureTrackerByOrb", "recoverFeatureTrackerByTemplate",
    "recoverFeatureTrackerByColor", "restartFeatureTrackerFromRect"
]

def remove_method_body(method_name, cpp_text):
    search_str = "MainWindow::" + method_name + "("
    idx = cpp_text.find(search_str)
    if idx == -1: return cpp_text
    
    start_idx = cpp_text.rfind("\n", 0, idx)
    if start_idx == -1: start_idx = 0
    
    # backtrack to include comments if any! This is hard, let's just delete the function block
    brace_idx = cpp_text.find("{", idx)
    if brace_idx == -1: return cpp_text
        
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
                
    if end_idx == -1: return cpp_text
    
    return cpp_text[:start_idx] + "\n" + cpp_text[end_idx:]

for m in methods_to_remove:
    cpp_content = remove_method_body(m, cpp_content)

for h in ["confidenceThresholdForClass", "boundedIntRect"]:
    search_str = " " + h + "("
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
        cpp_content = cpp_content[:start_idx] + "\n" + cpp_content[end_idx:]

with open(r"d:\QT\project\untitled7\mainwindow.cpp", "w", encoding="utf-8") as f:
    f.write(cpp_content)

print("Removed methods from mainwindow.cpp")
