import re

cpp_file = r"d:\QT\project\untitled7\trackingengine.cpp"
with open(cpp_file, "r", encoding="utf-8") as f:
    content = f.read()

# Replace the dnnWarmupFinished lambda with a simple one
search_str = "connect(thread, &DnnThread::dnnWarmupFinished, this,"
idx1 = content.find(search_str)

if idx1 != -1:
    brace_idx = content.find("{", idx1)
    brace_count = 0
    idx2 = -1
    for i in range(brace_idx, len(content)):
        if content[i] == '{':
            brace_count += 1
        elif content[i] == '}':
            brace_count -= 1
            if brace_count == 0:
                idx2 = content.find(";", i) + 1
                break
                
    if idx2 != -1:
        replacement = """connect(thread, &DnnThread::dnnWarmupFinished, this,
            [this, thread](bool success, const QString &message) {
        emit logMessage(
            QString(success ? "【系统】%1" : "【警告】%1").arg(message));
    });"""
        content = content[:idx1] + replacement + content[idx2:]

with open(cpp_file, "w", encoding="utf-8") as f:
    f.write(content)

print("Fixed dnnWarmupFinished lambda")
