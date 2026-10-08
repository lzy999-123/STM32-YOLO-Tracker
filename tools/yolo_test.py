from pathlib import Path

from ultralytics import YOLO

# 脚本位于 tools/；模型 .pt/.onnx 仍放在仓库根目录，程序运行时从那里查找。
TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent

print("正在加载 YOLO26n 模型...")
# 1. 加载官方的 YOLO26n 预训练模型，体积小，适合先验证接入链路。
model = YOLO(str(REPO_ROOT / "yolo26n.pt"))

print("\n开始进行目标检测...")
# 2. 对仓库自带的测试图片进行预测，并将结果保存到仓库根目录的 runs/（已被 .gitignore 忽略）
results = model.predict(source=str(TOOLS_DIR / "bus.jpg"), save=True,
                        project=str(REPO_ROOT / "runs" / "detect"), name="predict", exist_ok=True)

print("\n开始导出 YOLO26n ONNX...")
model.export(format="onnx")

print("\n开始导出 YOLO26s ONNX...")
YOLO(str(REPO_ROOT / "yolo26s.pt")).export(format="onnx")

print("\n测试完成！")
print("请在仓库根目录确认 yolo26n.onnx / yolo26s.onnx 已生成，runs/detect/predict 中会有检测结果图。")
