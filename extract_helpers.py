import re
import os

cpp_file = r"d:\QT\project\untitled7\mainwindow.cpp"

with open(cpp_file, "r", encoding="utf-8") as f:
    cpp_content = f.read()

helpers = []

for func in ["confidenceThresholdForClass", "boundedIntRect", "ensureDnnThread"]:
    pattern = r"((?:[\w\:\<\>\,]+\s+)+)MainWindow::" + func + r"\s*\((.*?)\)(?:\s*const)?\s*\{"
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
            body = body.replace("MainWindow::", "TrackingEngine::")
            helpers.append(body)

with open(r"d:\QT\project\untitled7\trackingengine_helpers.cpp", "w", encoding="utf-8") as f:
    f.write("\n\n".join(helpers))

print(f"Extracted {len(helpers)} helpers.")
