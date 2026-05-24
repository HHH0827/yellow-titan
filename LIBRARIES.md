# Project Libraries

This project uses three groups of libraries: ESP32-CAM Arduino libraries, Python computer-vision libraries, and Zephyr Bluetooth libraries for the XIAO BLE board.

## 1. ESP32-CAM / Arduino Libraries

Used in:

```text
ESP32_CameraWebServer/
```

Main files:

- `CameraWebServer.ino`
- `app_httpd.cpp`
- `camera_pins.h`
- `camera_index.h`
- `board_config.h`

Libraries:

| Library | Source | Purpose |
| --- | --- | --- |
| `Arduino.h` | Arduino core | Basic Arduino functions, setup/loop structure, serial output. |
| `WiFi.h` | ESP32 Arduino core | Connects the ESP32-CAM to WiFi so the computer can access the camera over HTTP. |
| `esp_camera.h` | ESP32 Arduino core | Controls the OV2640 camera module and captures JPEG frames. |
| `esp_http_server.h` | ESP-IDF through ESP32 Arduino core | Runs the HTTP server that provides `/capture`, `/stream`, `/status`, and `/control`. |
| `esp_timer.h` | ESP-IDF through ESP32 Arduino core | Timing support used by the camera web server. |
| `img_converters.h` | ESP32 camera support | Image conversion utilities used by the web server. |
| `fb_gfx.h` | ESP32 camera support | Framebuffer graphics helper used by the camera example. |
| `esp32-hal-ledc.h` | ESP32 Arduino core | LED PWM/flash control support. |

No extra Arduino Library Manager package is needed if the ESP32 board package is installed. The project uses the Arduino ESP32 core CameraWebServer example.

Important endpoint used by the Python program:

```text
http://172.20.10.9/capture
```

## 2. Python / OpenCV Libraries

Used in:

```text
esp32_color_detect.py
kalman_filter.py
```

Install command:

```powershell
py -3.12 -m pip install -r "\\wsl.localhost\Ubuntu\home\dayang\csse4011\firmware\FinalProject\requirements.txt"
```

Libraries:

| Library | Package | Purpose |
| --- | --- | --- |
| `cv2` | `opencv-python` | Decodes JPEG images, converts BGR to HSV, thresholds red/green/blue, draws the result on the preview frame. |
| `numpy` | `numpy` | Stores image bytes and pixel arrays for OpenCV processing. |
| `serial` | `pyserial` | Sends `color r`, `color g`, `color b`, or `color u` to the XIAO BLE board over `COM5`. |
| `urllib.request` | Python standard library | Downloads the JPEG image from the ESP32-CAM `/capture` endpoint. |
| `argparse` | Python standard library | Handles command-line options such as `--serial-port COM5`. |
| `dataclasses` | Python standard library | Stores color range definitions cleanly. |

Project module:

| Module | Purpose |
| --- | --- |
| `kalman_filter.py` | Provides the standalone `ScalarKalmanFilter` algorithm used to smooth noisy color percentage measurements. |

Python role in the project:

```text
ESP32-CAM image -> OpenCV HSV color detection -> Kalman Filter smoothing -> threshold decision -> serial command to XIAO BLE
```

Algorithms used in `esp32_color_detect.py`:

| Algorithm | Purpose |
| --- | --- |
| HSV thresholding | Separates red, green, and blue pixels from the camera image. |
| Scalar Kalman Filter | Smooths noisy per-frame red/green/blue percentage measurements. |
| 50 percent threshold classifier | Converts the filtered percentages into `red`, `green`, `blue`, or `unknown`. |

## 3. XIAO BLE / Zephyr Libraries

Used in:

```text
xiao_ble_tx/
```

Main files:

- `xiao_ble_tx/src/main.c`
- `xiao_ble_tx/prj.conf`
- `xiao_ble_tx/CMakeLists.txt`

Libraries:

| Library | Source | Purpose |
| --- | --- | --- |
| `zephyr/bluetooth/bluetooth.h` | Zephyr Bluetooth stack | Starts Bluetooth and BLE advertising. |
| `zephyr/bluetooth/hci.h` | Zephyr Bluetooth stack | Provides BLE advertising constants and host-controller interface definitions. |
| `zephyr/kernel.h` | Zephyr kernel | Kernel utilities used by the embedded firmware. |
| `zephyr/logging/log.h` | Zephyr logging | Prints debug/status logs from the XIAO BLE board. |
| `zephyr/shell/shell.h` | Zephyr shell | Receives serial shell commands such as `color r`. |
| `zephyr/sys/atomic.h` | Zephyr system utilities | Stores safe shared state for BLE readiness and advertising state. |
| `zephyr/sys/byteorder.h` | Zephyr system utilities | Packs manufacturer data in little-endian format. |
| `zephyr/sys/util.h` | Zephyr system utilities | Helper macros such as `ARRAY_SIZE`. |

Zephyr role in the project:

```text
Serial command from PC -> XIAO BLE -> BLE manufacturer-data broadcast
```

The transmitter broadcasts:

```text
R = red
G = green
B = blue
U = unknown
```

## 4. Final Runtime Command

After the ESP32-CAM and XIAO BLE firmware are running:

```powershell
py -3.12 "\\wsl.localhost\Ubuntu\home\dayang\csse4011\firmware\FinalProject\esp32_color_detect.py" --serial-port COM5
```

## 5. Short Explanation for Demonstration

The ESP32-CAM uses the Arduino ESP32 camera and HTTP server libraries to provide a JPEG image endpoint. The computer uses OpenCV and NumPy to measure red, green, and blue percentages. A scalar Kalman Filter smooths these noisy measurements, then a 50 percent threshold classifier decides whether the frame is red, green, blue, or unknown. PySerial sends the detected result to the XIAO BLE board. The XIAO BLE firmware uses Zephyr Bluetooth libraries to broadcast that command to the receiver board.
