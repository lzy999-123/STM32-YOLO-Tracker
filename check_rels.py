import urllib.request, re
try:
    res=urllib.request.urlopen('https://mirror.ghproxy.com/https://github.com/microsoft/onnxruntime/releases/tag/v1.18.0')
    html=res.read().decode('utf-8')
    print(set(re.findall(r'onnxruntime-win-x64-gpu[^\"]*\.zip', html)))
except Exception as e:
    print(e)
