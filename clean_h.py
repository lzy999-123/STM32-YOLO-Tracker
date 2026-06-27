import re

h_file = r"d:\QT\project\untitled7\mainwindow.h"
with open(h_file, "r", encoding="utf-8") as f:
    h_content = f.read()

# Add TrackingEngine include
if '#include "trackingengine.h"' not in h_content:
    h_content = h_content.replace('#include "cameramanager.h"', '#include "cameramanager.h"\n#include "trackingengine.h"')

# Remove slots
slots_to_remove = [
    "void onDnnResultReceived", "void onYoloDetectionResult", "void restartDnnThread", "void preloadYoloModels", "DnnThread *ensureDnnThread"
]
for slot in slots_to_remove:
    h_content = re.sub(r"\s*" + re.escape(slot) + r"\(.*?\);", "", h_content)

# Remove all methods starting with "feature" or "recoverFeatureTracker" etc.
methods_to_remove = [
    "currentTrackingModelFileName", "currentTrackingBackend", "isFeatureTrackingSelected", "resetFeatureTracker",
    "initFeatureTracker", "updateFeatureTracker", "recoverFeatureTracker", "buildFeatureReference",
    "featurePatchSimilarity", "featureColorSimilarity", "featureShapeSimilarity", "featureCandidateScore",
    "alignFeatureRectInYoloCandidate", "evaluateYoloFeatureCandidate", "featureCandidatePasses",
    "confirmFeatureRecoveryCandidate", "requestFeatureYoloTargetClassification", "requestFeatureYoloRecovery",
    "nextFeatureYoloRequestId", "resetFeatureRecoveryCandidate", "resetFeatureYoloTargetClassification",
    "restartFeatureTrackerFromRect"
]
for m in methods_to_remove:
    h_content = re.sub(r"\s*[\w\:\<\>\,\*\s]+ " + re.escape(m) + r"\(.*?\)(?:\s*const)?;", "", h_content)

# Remove member variables related to tracking
vars_to_remove = [
    "m_dnnThread", "m_dnnThreads", "m_featureTracker", "m_featureTrackerReady", "m_featureTemplate",
    "m_featureColorHist", "m_featureReferenceKeypoints", "m_featureReferenceDescriptors", "m_featureReferenceSize",
    "m_featureLastRect", "m_featureRecoveryCandidateRect", "m_featureRecoveryCandidateMethod",
    "m_featureUnreliableCount", "m_featureRecoveryCandidateCount", "m_featureYoloRecoveryPending",
    "m_featureYoloRecoveryRequestId", "m_featureYoloRecoveryFrame", "m_featureYoloBestCandidate",
    "m_featureYoloBestScore", "m_featureYoloBestClassId", "m_featureYoloBestConfidence",
    "m_lastFeatureYoloRecoveryRequestTime", "m_featureYoloCandidatesEvaluated", "m_featureYoloNextRequestId",
    "m_featureYoloClassifyPending", "m_featureYoloClassifyRequestId", "m_featureYoloClassifyFrame",
    "m_featureYoloClassifyBestScore", "m_featureYoloClassifyBestClassId", "m_featureYoloClassifyBestConfidence",
    "m_featureYoloClassifyBestBox", "m_featureYoloClassKnown", "m_featureUseYoloRecovery",
    "m_featureTargetYoloClassId", "m_featureTargetYoloClassName", "m_featureTargetYoloBox",
    "m_featureYoloRelX", "m_featureYoloRelY", "m_featureYoloRelW", "m_featureYoloRelH"
]
for v in vars_to_remove:
    h_content = re.sub(r"\s*[\w\:\<\>\,\*\s]+ " + re.escape(v) + r"(?:\s*=\s*.*?)?;", "", h_content)

# Add TrackingEngine
if "TrackingEngine m_trackingEngine;" not in h_content:
    h_content = h_content.replace("CameraManager m_cameraManager;", "CameraManager m_cameraManager;\n    TrackingEngine m_trackingEngine;")

# Remove OpenCV includes if they are no longer needed? 
# We'll keep them for now to avoid breaking other things, 
# wait, actually let's clean them if possible. No, MainWindow still uses cv::Mat, cv::Rect2d.

with open(h_file, "w", encoding="utf-8") as f:
    f.write(h_content)

print("Cleaned mainwindow.h")
