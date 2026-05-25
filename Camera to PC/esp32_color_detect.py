import argparse
import time
from dataclasses import dataclass
from urllib.request import urlopen

import cv2
#分析数组
import numpy as np

from kalman_filter import ScalarKalmanFilter

#设置三种颜色的HSV范围
@dataclass(frozen=True)
class ColorRange:
    name: str
    bgr: tuple[int, int, int]
    ranges: tuple[tuple[tuple[int, int, int], tuple[int, int, int]], ...]


COLOR_RANGES = (
    ColorRange(
        "red",
        (0, 0, 255),
        (
            ((0, 90, 70), (10, 255, 255)),
            ((170, 90, 70), (179, 255, 255)),
        ),
    ),
    ColorRange("green", (0, 255, 0), (((36, 60, 60), (85, 255, 255)),)),
    ColorRange("blue", (255, 0, 0), (((90, 60, 60), (130, 255, 255)),)),
)

#从http://172.20.10.9/capture网页中读取图片
def read_jpeg(url: str, timeout: float):
    with urlopen(url, timeout=timeout) as response:
        data = response.read()
    #将图片原本二进制转化为数组形式，用uint8的形式
    image_array = np.frombuffer(data, dtype=np.uint8)
    #用OpenCV解码成bgr的形式
    return cv2.imdecode(image_array, cv2.IMREAD_COLOR)

#给每种颜色生成掩图
def build_mask(hsv, color_range: ColorRange):
    mask = np.zeros(hsv.shape[:2], dtype=np.uint8)
    #生成三种颜色的掩图
    for lower, upper in color_range.ranges:
        mask |= cv2.inRange(hsv, np.array(lower), np.array(upper))
    #生成kernel，kernel越大去噪声和平滑效果越强
    kernel = np.ones((5, 5), np.uint8)
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, kernel)
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel)
    return mask

#检测颜色
def detect_colors(frame):
    #将bgr形式的图转化为hsv格式
    hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
    frame_area = frame.shape[0] * frame.shape[1]
    results = []
    #循环计算每种颜色的占比
    for color_range in COLOR_RANGES:
        mask = build_mask(hsv, color_range)
        area = int(cv2.countNonZero(mask))
        raw_percent = area / frame_area * 100
        ##找到目标颜色区域的轮廓
        contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        if contours:
            #找到最大的颜色轮廓
            contour = max(contours, key=cv2.contourArea)
            #生成对应的最小的外接矩形
            x, y, w, h = cv2.boundingRect(contour)
        else:
            x, y, w, h = 0, 0, 0, 0

        #对这个矩形加入文字说明
        results.append(
            {
                "name": color_range.name,
                "bgr": color_range.bgr,
                "area": area,
                "raw_percent": raw_percent,
                "percent": raw_percent,
                "box": (x, y, w, h),
            }
        )

    return sorted(results, key=lambda item: item["percent"], reverse=True)

#加入kalman滤波
def create_kalman_filters(process_noise: float, measurement_noise: float):
    return {
        color_range.name: ScalarKalmanFilter(
            process_noise=process_noise,
            measurement_noise=measurement_noise,
        )
        for color_range in COLOR_RANGES
    }

#应用kalman滤波
def apply_kalman_filters(results, filters, enabled: bool):
    filtered_results = []
    for result in results:
        filtered_result = result.copy()
        raw_percent = result["raw_percent"]
        if enabled:
            filtered_result["percent"] = filters[result["name"]].update(raw_percent)
        else:
            filtered_result["percent"] = raw_percent
        filtered_results.append(filtered_result)
    return sorted(filtered_results, key=lambda item: item["percent"], reverse=True)

#通过阈值判断颜色种类
def decide_color(results, threshold_percent: float):
    best = results[0] if results else None
    if best and best["percent"] > threshold_percent:
        return best["name"], best
    return "unknown", None

#画出可视化结果
def draw_results(frame, decision: str, selected, fps: float, send_state: str):
    annotated = frame.copy()
    cv2.putText(
        annotated,
        f"{frame.shape[1]}x{frame.shape[0]}  {fps:.1f} FPS",
        (8, 24),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.65,
        (0, 255, 0),
        2,
        cv2.LINE_AA,
    )

    decision_colors = {
        "red": (0, 0, 255),
        "green": (0, 255, 0),
        "blue": (255, 0, 0),
        "unknown": (0, 255, 255),
    }
    label_color = decision_colors[decision]
    cv2.putText(
        annotated,
        decision.upper(),
        (8, 64),
        cv2.FONT_HERSHEY_SIMPLEX,
        1.2,
        label_color,
        3,
        cv2.LINE_AA,
    )
    cv2.putText(
        annotated,
        send_state,
        (8, 92),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.55,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )

    if selected:
        x, y, w, h = selected["box"]
        color = selected["bgr"]
        cv2.rectangle(annotated, (x, y), (x + w, y + h), color, 2)
        cv2.putText(
            annotated,
            f"K {selected['percent']:.1f}% / raw {selected['raw_percent']:.1f}%",
            (x, max(18, y - 6)),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.55,
            color,
            2,
            cv2.LINE_AA,
        )
    return annotated

#把颜色判断转换成 Shell 命令
def decision_to_shell_command(decision: str) -> str:
    color_args = {
        "red": "r",
        "green": "g",
        "blue": "b",
        "unknown": "u",
    }
    return f"color {color_args[decision]}\r\n"

#打开 XIAO BLE 对应的串口
def open_serial(port: str | None, baudrate: int):
    if not port:
        return None

    try:
        import serial
    except ImportError as exc:
        raise RuntimeError("pyserial is required for --serial-port. Run: py -3.12 -m pip install pyserial") from exc

    ser = serial.Serial(port, baudrate=baudrate, timeout=0.1, write_timeout=1.0)
    time.sleep(2.0)
    ser.reset_input_buffer()
    return ser

#发送指令
def send_serial_command(serial_port, decision: str):
    if not serial_port:
        return False, "serial disabled"

    command = decision_to_shell_command(decision)
    try:
        serial_port.write(command.encode("ascii"))
        serial_port.flush()
        return True, command.strip()
    except Exception as exc:
        return False, f"serial error: {exc}"


def main() -> int:
    parser = argparse.ArgumentParser(description="Detect red, green, blue, or unknown from ESP32-CAM frames.")
    parser.add_argument("--url", default="http://172.20.10.9/capture", help="ESP32-CAM capture URL")
    parser.add_argument("--frames", type=int, default=0, help="Number of frames to read; 0 means run until q is pressed")
    parser.add_argument("--duration", type=float, default=0.0, help="Seconds to run; 0 means no time limit")
    parser.add_argument("--timeout", type=float, default=3.0, help="HTTP timeout in seconds")
    parser.add_argument("--threshold", type=float, default=50.0, help="Filtered percent needed to decide a color")
    parser.add_argument("--no-kalman", action="store_true", help="Disable Kalman filtering and use raw percentages")
    parser.add_argument("--kalman-process-noise", type=float, default=0.08)
    parser.add_argument("--kalman-measurement-noise", type=float, default=6.0)
    parser.add_argument("--save", default="esp32_color_detect.jpg", help="Path for the latest annotated frame")
    parser.add_argument("--no-window", action="store_true", help="Do not open an OpenCV preview window")
    parser.add_argument("--serial-port", default = "COM5",help="Serial port for the XIAO BLE transmitter, for example COM5")
    parser.add_argument("--baudrate", type=int, default=115200, help="Serial baudrate for the XIAO BLE transmitter")
    parser.add_argument("--send-interval", type=float, default=2.0, help="Minimum seconds between repeated serial sends")
    args = parser.parse_args()

    #初始化信息
    count = 0
    send_count = 0
    start = time.time()
    last_sent_decision = None
    last_sent_time = 0.0
    send_state = "TX idle"
    serial_port = open_serial(args.serial_port, args.baudrate)

    #初始化kalman滤波
    kalman_enabled = not args.no_kalman
    kalman_filters = create_kalman_filters(args.kalman_process_noise, args.kalman_measurement_noise)

    #打印最开始的信息
    print(f"Reading frames from {args.url}")
    print(
        "Kalman filter: "
        f"{'enabled' if kalman_enabled else 'disabled'} "
        f"(Q={args.kalman_process_noise}, R={args.kalman_measurement_noise})"
    )
    if serial_port:
        print(f"Sending color commands to {args.serial_port} at {args.baudrate} baud")
    if not args.no_window:
        print("Press q while the OpenCV window is focused, or press Ctrl+C in PowerShell.")

    try:
        #判断是否检测发送
        while args.frames <= 0 or count < args.frames:
            if args.duration > 0 and time.time() - start >= args.duration:
                print(f"Duration limit reached: {args.duration:.1f}s")
                break
            #从网页读取一帧图片
            frame = read_jpeg(args.url, args.timeout)
            if frame is None:
                print("Failed to decode JPEG frame.")
                return 1
            #计算fps的值是多少
            count += 1
            fps = count / max(time.time() - start, 1e-6)
            #对图片进行颜色判断
            raw_results = detect_colors(frame)
            results = apply_kalman_filters(raw_results, kalman_filters, kalman_enabled)
            decision, selected = decide_color(results, args.threshold)

            now = time.time()
            #判断是否发送，如果有新的颜色判决或者两次识别的时间超过interval就发送指令
            should_send = serial_port and (
                decision != last_sent_decision or now - last_sent_time >= args.send_interval
            )
            if should_send:
                ok, detail = send_serial_command(serial_port, decision)
                if ok:
                    send_count += 1
                    last_sent_decision = decision
                    last_sent_time = now
                    send_state = f"TX #{send_count}: {detail}"
                else:
                    send_state = f"TX failed: {detail}"
                print(send_state)

            annotated = draw_results(frame, decision, selected, fps, send_state)
            if args.save:
                cv2.imwrite(args.save, annotated)

            percentages = {item["name"]: item["percent"] for item in results}
            raw_percentages = {item["name"]: item["raw_percent"] for item in results}
            #打印对应的信息
            print(
                f"frame {count}: {decision} "
                f"(kalman r/g/b={percentages.get('red', 0):.1f}/"
                f"{percentages.get('green', 0):.1f}/"
                f"{percentages.get('blue', 0):.1f}%; "
                f"raw r/g/b={raw_percentages.get('red', 0):.1f}/"
                f"{raw_percentages.get('green', 0):.1f}/"
                f"{raw_percentages.get('blue', 0):.1f}%; "
                f"tx={send_count})"
            )

            if not args.no_window:
                cv2.imshow("ESP32-CAM color detection", annotated)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break
    #退出程序
    except KeyboardInterrupt:
        print("Stopped by Ctrl+C.")
    finally:
        if serial_port:
            serial_port.close()
        if not args.no_window:
            cv2.destroyAllWindows()

    elapsed = max(time.time() - start, 1e-6)
    print(f"Read {count} frame(s). Average FPS: {count / elapsed:.2f}. Serial sends: {send_count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
