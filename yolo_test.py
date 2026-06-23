from ultralytics import YOLO

print("🔄 正在加载 YOLOv8 模型...")
# 1. 加载官方的 YOLOv8n (nano) 预训练模型，体积最小，速度最快
model = YOLO('yolov8n.pt') 

print("\n🚀 开始进行目标检测...")
# 2. 对一张网络图片进行预测，并将结果保存下来
# (这会自动下载这张测试图片并在图片上画出检测框)
results = model.predict(source='https://ultralytics.com/images/bus.jpg', save=True)

print("\n✅ 测试完成！")
print("👉 请在左侧文件目录中找到 'runs/detect/predict' 文件夹，里面会有一张识别好物体的图片！")
