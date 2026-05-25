# Project Libraries and Advanced Features

This project uses four main groups of software libraries: ESP32-CAM Arduino libraries, Python computer-vision libraries, a project Kalman filter module, and Zephyr RTOS libraries for the XIAO BLE board. The final implementation uses Wi-Fi/HTTP image capture, OpenCV colour recognition, Kalman filtering, serial command transmission, BLE advertising, and a Zephyr watchdog timer for reliability.

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

No extra Arduino Library Manager package is needed if the ESP32 board package is installed. The project uses the Arduino ESP32 core CameraWebServer example as the camera firmware base.

Important endpoint used by the Python program:

```text
http://172.20.10.9/capture
```

In this project, the ESP32-CAM is used as a Wi-Fi camera node only. It captures JPEG frames and provides them through the HTTP `/capture` endpoint. The colour recognition algorithm is not executed on the ESP32-CAM; it is executed on the PC using Python and OpenCV.

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
| `time` | Python standard library | Measures runtime, FPS, timestamps, and command timing. |

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
| Configurable threshold classifier | Converts the filtered percentages into `red`, `green`, `blue`, or `unknown`. The project can use a 50 percent decision threshold for final classification, while a lower threshold can be selected during testing under difficult lighting. |

## 3. Project Kalman Filter Module

Used in:

```text
kalman_filter.py
```

This is a project-specific algorithm module rather than an external library. It contains the `ScalarKalmanFilter` class used by `esp32_color_detect.py`.

| Module / Class | Purpose |
| --- | --- |
| `ScalarKalmanFilter` | Implements a one-dimensional Kalman filter for smoothing noisy colour percentage measurements. |

The PC program creates one Kalman filter for each colour channel:

```text
red percentage   -> red Kalman filter
green percentage -> green Kalman filter
blue percentage  -> blue Kalman filter
```

The filtered percentages are used for the final colour decision. This improves stability because a single noisy frame is less likely to immediately change the classification result.

## 4. XIAO BLE / Zephyr RTOS Libraries

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
| `zephyr/device.h` | Zephyr device model | Checks whether the watchdog hardware device is ready. |
| `zephyr/drivers/watchdog.h` | Zephyr watchdog driver | Configures and feeds the nRF52840 hardware watchdog timer for reliability. |
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

The transmitter uses BLE advertising manufacturer data, not a GATT connection. The PC sends a command to the XIAO BLE transmitter through USB serial. The transmitter then broadcasts the result wirelessly using BLE advertising data.

The transmitter broadcasts:

```text
R = red
G = green
B = blue
U = unknown
```

Manufacturer data payload:

| Byte(s) | Field | Purpose |
| --- | --- | --- |
| 0-1 | Manufacturer ID | Identifies the project manufacturer field. |
| 2-5 | Project ID | Identifies this final project payload. |
| 6 | Command type | `0xC1` means colour command. |
| 7 | Colour command | `R`, `G`, `B`, or `U`. |
| 8 | Sequence number | Increments when a new colour command is published. |

## 5. Zephyr RTOS Advanced Features

The final project uses the following Zephyr RTOS features on the XIAO BLE nRF52840 transmitter:

| Zephyr Feature | Configuration / API | Use in This Project |
| --- | --- | --- |
| Bluetooth LE broadcaster | `CONFIG_BT`, `CONFIG_BT_BROADCASTER`, `bt_enable()`, `bt_le_adv_start()` | Broadcasts the detected colour command to the receiver board using BLE advertising manufacturer data. |
| Serial shell subsystem | `CONFIG_SHELL`, `CONFIG_SHELL_BACKEND_SERIAL`, `SHELL_CMD_REGISTER()` | Allows the PC Python program to issue commands such as `color r`, `color g`, `color b`, or `color u` over COM5. |
| Logging subsystem | `CONFIG_LOG`, `LOG_INF()`, `LOG_WRN()`, `LOG_ERR()` | Prints firmware status, BLE advertising status, command updates, and watchdog status during debugging. |
| Kernel thread | `K_THREAD_DEFINE()` | Runs a dedicated watchdog feed thread in parallel with the BLE and shell logic. |
| Atomic variables | `atomic_t`, `atomic_get()`, `atomic_set()` | Tracks Bluetooth readiness, advertising state, and watchdog readiness safely across firmware contexts. |
| Hardware watchdog driver | `CONFIG_WATCHDOG`, `wdt_install_timeout()`, `wdt_setup()`, `wdt_feed()` | Improves reliability by resetting the XIAO BLE board if the firmware hangs and the watchdog is no longer fed. |
| DeviceTree device access | `DT_NODELABEL(wdt0)`, `DEVICE_DT_GET()` | Gets the nRF52840 watchdog hardware device from the Zephyr DeviceTree. |

Watchdog reliability design:

```text
Firmware starts -> watchdog configured with 4 s timeout -> watchdog thread feeds every 1 s
If firmware hangs -> feed stops -> watchdog resets the XIAO BLE board
```

This is used as a reliability and robustness feature for the final demonstration.
## 6. Receiver / Servo Node Libraries and Features

The receiver / servo node is implemented using Zephyr RTOS on the XIAO BLE nRF52840 board. This part of the system receives BLE colour commands, controls the servo motor, updates object counts, stores sorting statistics and improves firmware reliability.

### 6.1 Zephyr Libraries Used in the Receiver Node

| Library / Header | Type | Use in This Project |
|---|---|---|
| `zephyr/bluetooth/bluetooth.h` | Bluetooth library | Enables Bluetooth on the receiver node and supports BLE scanning. |
| `zephyr/bluetooth/hci.h` | Bluetooth HCI library | Provides BLE scanning constants and Bluetooth controller definitions. |
| `zephyr/devicetree.h` | DeviceTree library | Reads board-specific hardware configuration such as LED, PWM and watchdog nodes. |
| `zephyr/drivers/gpio.h` | GPIO driver | Controls RGB debug LEDs to indicate the received colour command. |
| `zephyr/drivers/pwm.h` | PWM driver | Generates PWM signals for controlling the sorting servo motor. |
| `zephyr/kernel.h` | Kernel library | Provides message queues, kernel threads, sleep timing and uptime functions. |
| `zephyr/logging/log.h` | Logging library | Prints debug, warning and error messages for BLE reception, servo control, storage and watchdog status. |
| `zephyr/sys/byteorder.h` | System utility library | Reads little-endian manufacturer ID and project ID values from BLE advertising data. |
| `zephyr/drivers/watchdog.h` | Watchdog driver | Enables the hardware watchdog so the receiver can recover if the firmware becomes unresponsive. |
| `zephyr/fs/nvs.h` | NVS flash storage | Stores red, green, blue, unknown and total sorting counts in non-volatile flash memory. |
| `zephyr/storage/flash_map.h` | Flash storage library | Provides access to the flash device and flash area used by NVS. |
| `stdbool.h` | C standard library | Provides the `bool` data type. |
| `stdint.h` | C standard library | Provides fixed-width integer types such as `uint8_t`, `uint16_t` and `uint32_t`. |
| `string.h` | C standard library | Provides basic string and memory utility functions. |

### 6.2 Receiver Node Advanced Features

| Feature | Related API / Code | Use in This Project |
|---|---|---|
| BLE passive scanning | `bt_enable()`, `bt_le_scan_start()`, `BT_LE_SCAN_TYPE_PASSIVE` | The receiver listens for BLE advertising packets from the transmitter without maintaining a permanent BLE connection. |
| BLE manufacturer data parsing | `bt_data_parse()` | Extracts manufacturer ID, project ID, command type, colour command and sequence number from BLE advertising data. |
| Packet filtering | Manufacturer ID, project ID and command type checks | Ensures the receiver only responds to valid project packets and ignores unrelated BLE advertisements. |
| Duplicate command protection | `last_sequence`, `last_color` | Prevents the servo from reacting repeatedly to the same BLE advertisement. |
| Servo PWM control | `pwm_set_dt()`, `PWM_USEC()`, `PWM_MSEC()` | Converts colour commands into servo angles for physical sorting. |
| Message queue | `K_MSGQ_DEFINE()`, `k_msgq_put()`, `k_msgq_get()` | Passes colour commands from the BLE scan callback to the motor control thread. |
| Motor control thread | `K_THREAD_DEFINE()` | Runs servo movement and counting logic separately from BLE scanning. |
| NVS flash storage | `nvs_mount()`, `nvs_read()`, `nvs_write()` | Saves sorting statistics so object counts can survive reset or power loss. |
| Watchdog protection | `wdt_install_timeout()`, `wdt_setup()`, `wdt_feed()` | Improves reliability by allowing the board to reset if the firmware hangs. |
| Low-power timing | `k_msleep(1000)` | Avoids busy-waiting in the main loop while still feeding the watchdog periodically. |
| Scan interval / scan window | `.interval = 0x00A0`, `.window = 0x0030` | Reduces continuous radio activity compared with scanning at full duty cycle. |

### 6.3 Receiver Node Data Flow

```text
BLE advertising packet received
        ↓
Manufacturer data parsed
        ↓
Project ID, command type and colour checked
        ↓
Duplicate command filtered using sequence number
        ↓
Colour command sent to message queue
        ↓
Motor thread updates servo angle
        ↓
Object counter is updated
        ↓
Sorting statistics are saved to NVS flash
        ↓
Watchdog is fed periodically in the main loop
```
### 6.4 Servo PWM Mapping

| Colour Command | PWM Pulse Width | Approximate Servo Angle | Purpose |
|---|---:|---:|---|
| `R` | `500 us` | `0°` | Red |
| `G` | `1000 us` | `60°` | Green |
| `B` | `1800 us` | `120°` | Blue |
| `U` | `2500 us` | `180°` | Unknown / reject position |

### 6.5 NVS Stored Values

| NVS ID | Stored Value | Meaning |
|---:|---|---|
| `1` | `red_count` | Number of red objects sorted |
| `2` | `green_count` | Number of green objects sorted |
| `3` | `blue_count` | Number of blue objects sorted |
| `4` | `unknown_count` | Number of unknown / rejected objects |
| `5` | `total_object` | Total number of processed objects |

The receiver uses NVS because the object counts should not be lost immediately after reset or power loss. This helps with testing, demonstration and KPI tracking.

## 7. Final Runtime Command

After the ESP32-CAM and XIAO BLE firmware are running:

```powershell
py -3.12 "\\wsl.localhost\Ubuntu\home\dayang\csse4011\firmware\FinalProject\esp32_color_detect.py" --serial-port COM5
```

For the current test setup, a lower threshold can also be used:

```powershell
py -3.12 "\\wsl.localhost\Ubuntu\home\dayang\csse4011\firmware\FinalProject\esp32_color_detect.py" --threshold 15 --kalman-process-noise 1.0 --kalman-measurement-noise 3.0 --serial-port COM5
```

## 8. Short Explanation for Demonstration

The ESP32-CAM uses the Arduino ESP32 camera and HTTP server libraries to provide a JPEG image endpoint. The computer uses OpenCV and NumPy to measure red, green, and blue percentages. A scalar Kalman Filter smooths these noisy measurements, then a threshold classifier decides whether the frame is red, green, blue, or unknown. PySerial sends the detected result to the XIAO BLE board. The XIAO BLE firmware uses Zephyr Bluetooth libraries to broadcast that command to the receiver board through BLE advertising. The XIAO BLE firmware also uses the Zephyr watchdog driver as a reliability feature, so the board can automatically reset if the firmware becomes stuck.
