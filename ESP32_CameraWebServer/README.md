# ESP32 CameraWebServer

This folder is a local copy of the Arduino ESP32 `CameraWebServer` example.

Original source on this computer:

```text
C:\Users\10548\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.3.8\libraries\ESP32\examples\Camera\CameraWebServer
```

## Files

- `CameraWebServer.ino`: Arduino sketch entry point. It configures the camera, connects to WiFi, and starts the web server.
- `board_config.h`: Selects the ESP32-CAM board model used by the sketch.
- `app_httpd.cpp`: HTTP server implementation. This file defines routes such as `/`, `/capture`, `/stream`, `/status`, and `/control`.
- `camera_index.h`: The web page HTML/JavaScript shown in the browser.
- `camera_pins.h`: Pin definitions for supported ESP32-CAM boards, including `CAMERA_MODEL_AI_THINKER`.

## How Our Project Uses It

The ESP32-CAM runs this web server and exposes a JPEG snapshot endpoint:

```text
http://172.20.10.9/capture
```

The Python OpenCV program reads that endpoint, decodes the JPEG image, detects red/green/blue/unknown, and sends the result to the XIAO BLE board.

## Demo Notes

For AI Thinker ESP32-CAM, `board_config.h` should use:

```cpp
#define CAMERA_MODEL_AI_THINKER
```

The WiFi SSID and password are configured in `CameraWebServer.ino`.
