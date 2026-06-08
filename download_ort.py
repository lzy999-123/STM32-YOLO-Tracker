import urllib.request
import zipfile
import os

url = 'https://mirror.ghproxy.com/https://github.com/microsoft/onnxruntime/releases/download/v1.18.0/onnxruntime-win-x64-gpu-1.18.0.zip'
zip_path = 'onnxruntime.zip'
extract_path = 'D:/onnxruntime_gpu'

print('Downloading ONNX Runtime GPU...')
try:
    urllib.request.urlretrieve(url, zip_path)
    print('Extracting...')
    with zipfile.ZipFile(zip_path, 'r') as zip_ref:
        zip_ref.extractall(extract_path)
    print('Done.')
except Exception as e:
    print('Error:', e)
