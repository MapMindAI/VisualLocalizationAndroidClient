# VisualLocalizationAndroidClient


An Android app for visual localization in a prebuilt map, using ARCore and a remote localization service.


## 🚀 What it does

This app allows a device to localize itself inside a prebuilt 3D map.

It works by:
1. Using ARCore as a VIO (Visual-Inertial Odometry) tracker
2. Capturing a camera image
3. Sending the image to a localization server
4. Receiving:
  * 📌 6DoF pose (position + orientation)
  * 🧩 Map mesh

With this, the app can align itself inside a known environment and run reliably without GPS.


## ⚙️ Build

```
docker build -f artifacts/docker/dev.dockerfile -t android_ndk25 artifacts/docker/
```

```
cd app_vlp
./build.sh
```

APK output: "app_vlp/build/outputs/apk/debug/"


## ▶️ Run

![vlp run gif](/home/yeliu/Downloads/ezgif-6d49af0c8a5bb15a.gif)
