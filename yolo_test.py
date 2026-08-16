from ultralytics import YOLO

print("正在加载 YOLO26n 模型...")
# 1. 加载官方的 YOLO26n 预训练模型，体积小，适合先验证接入链路。
model = YOLO("yolo26n.pt")

print("\n开始进行目标检测...")
# 2. 对一张网络图片进行预测，并将结果保存下来
# (这会自动下载这张测试图片并在图片上画出检测框)
results = model.predict(source='https://ultralytics.com/images/bus.jpg', save=True)

print("\n开始导出 YOLO26n ONNX...")
model.export(format="onnx")

print("\n开始导出 YOLO26s ONNX...")
YOLO("yolo26s.pt").export(format="onnx")

print("\n测试完成！")
print("请在项目目录中确认 yolo26n.onnx / yolo26s.onnx 已生成，runs/detect/predict 中会有检测结果图。")
