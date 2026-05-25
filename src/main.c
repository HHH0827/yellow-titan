#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>

/*一个日志模块已注册。
该模块名为“color_rx”，日志级别设置为“INFO”。
因此，在程序运行过程中，它可以通过 LOG_INF、LOG_WRN 和 LOG_ERR 输出运行状态、警告信息和错误消息，
这有助于对 BLE 通信和伺服控制进行调试。*/
LOG_MODULE_REGISTER(color_rx, LOG_LEVEL_INF);

#define FINAL_PROJECT_MFG_ID 0x1140U /*Manufacturer ID*/
#define FINAL_PROJECT_ID     0x4011F001U /*Project ID*/
#define COLOR_CMD_TYPE       0xC1U /*Comment type - for defining color*/

/*Obtain the hardware node of the LED on the development board through Zephyr's device tree alias,
and map them respectively as red, green, and blue LEDs.*/
#define LED_R_NODE DT_ALIAS(led0)
#define LED_G_NODE DT_ALIAS(led1)
#define LED_B_NODE DT_ALIAS(led2)

/*The servo angle is controlled approximately by the PWM pulse width. */
#define SERVO_NODE DT_ALIAS(sorter_servo)
#define SERVO_PERIOD PWM_MSEC(20) /*PWM周期=20ms*/
#define SERVO_RED_US 500U /*0*/
#define SERVO_GREEN_US 1000U /*60*/
#define SERVO_BLUE_US 1800U /*120*/
#define SERVO_UNKNOWN_US 2500U /*180*/

/*checks whether the RGB LEDs and servo hardware are available*/
#if DT_NODE_HAS_STATUS(LED_R_NODE, okay) && DT_NODE_HAS_STATUS(LED_G_NODE, okay) && \
	DT_NODE_HAS_STATUS(LED_B_NODE, okay)
#define HAS_RGB_LEDS 1
static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(LED_R_NODE, gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(LED_G_NODE, gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(LED_B_NODE, gpios);
#else
#define HAS_RGB_LEDS 0
#endif

#if DT_NODE_HAS_STATUS(SERVO_NODE, okay)
#define HAS_SERVO 1
static const struct pwm_dt_spec servo = PWM_DT_SPEC_GET(SERVO_NODE);
#else
#define HAS_SERVO 0
#endif

/*Define Bluetooth packets format*/
struct color_packet {
	bool valid;
	uint16_t mfg_id;
	uint32_t project_id;
	uint8_t cmd_type;
	char color;
	uint8_t sequence;
};

static char last_color = '\0';
static uint8_t last_sequence;
static int64_t hold_until_ms = 0;
#define MOTOR_HOLD_MS 3000
#define WDT_TIMEOUT_MS 5000
#define STORAGE_NODE DT_CHOSEN(zephyr_flash_controller)
#define NVS_SECTOR_SIZE 4096
#define NVS_SECTOR_COUNT 3

#define NVS_ID_RED_COUNT     1
#define NVS_ID_GREEN_COUNT   2
#define NVS_ID_BLUE_COUNT    3
#define NVS_ID_UNKNOWN_COUNT 4
#define NVS_ID_TOTAL_COUNT   5

static struct nvs_fs fs;

static uint32_t red_count;
static uint32_t green_count;
static uint32_t blue_count;
static uint32_t unknown_count;
static uint32_t total_count;

static int storage_init(void)
{
	int ret;

	fs.flash_device = DEVICE_DT_GET(STORAGE_NODE);
	if (!device_is_ready(fs.flash_device)) {
		LOG_ERR("Flash device not ready");
		return -ENODEV;
	}

	fs.offset = 0xFA000;
	fs.sector_size = NVS_SECTOR_SIZE;
	fs.sector_count = NVS_SECTOR_COUNT;

	ret = nvs_mount(&fs);
	if (ret < 0) {
		LOG_ERR("NVS mount failed: %d", ret);
		return ret;
	}

	(void)nvs_read(&fs, NVS_ID_RED_COUNT, &red_count, sizeof(red_count));
	(void)nvs_read(&fs, NVS_ID_GREEN_COUNT, &green_count, sizeof(green_count));
	(void)nvs_read(&fs, NVS_ID_BLUE_COUNT, &blue_count, sizeof(blue_count));
	(void)nvs_read(&fs, NVS_ID_UNKNOWN_COUNT, &unknown_count, sizeof(unknown_count));
	(void)nvs_read(&fs, NVS_ID_TOTAL_COUNT, &total_count, sizeof(total_count));

	LOG_INF("Stored counts: R=%u G=%u B=%u U=%u Total=%u",
		red_count, green_count, blue_count, unknown_count, total_count);

	return 0;
}

static void storage_save_counts(void)
{
	(void)nvs_write(&fs, NVS_ID_RED_COUNT, &red_count, sizeof(red_count));
	(void)nvs_write(&fs, NVS_ID_GREEN_COUNT, &green_count, sizeof(green_count));
	(void)nvs_write(&fs, NVS_ID_BLUE_COUNT, &blue_count, sizeof(blue_count));
	(void)nvs_write(&fs, NVS_ID_UNKNOWN_COUNT, &unknown_count, sizeof(unknown_count));
	(void)nvs_write(&fs, NVS_ID_TOTAL_COUNT, &total_count, sizeof(total_count));
}

static void update_sorting_counts(char color)
{
	switch (color) {
	case 'R':
		red_count++;
		break;
	case 'G':
		green_count++;
		break;
	case 'B':
		blue_count++;
		break;
	case 'U':
	default:
		unknown_count++;
		break;
	}

	total_count++;
	storage_save_counts();

	LOG_INF("Counts updated: R=%u G=%u B=%u U=%u Total=%u",
		red_count, green_count, blue_count, unknown_count, total_count);
}

#define COLOR_MSGQ_MAX_MSGS 8
K_MSGQ_DEFINE(color_msgq, sizeof(char), COLOR_MSGQ_MAX_MSGS, 4);

#if DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
#define WDT_NODE DT_ALIAS(watchdog0)
#elif DT_NODE_HAS_STATUS(DT_NODELABEL(wdt0), okay)
#define WDT_NODE DT_NODELABEL(wdt0)
#else
#define WDT_NODE DT_INVALID_NODE
#endif

#if DT_NODE_HAS_STATUS(WDT_NODE, okay)
#define HAS_WATCHDOG 1
static const struct device *const wdt = DEVICE_DT_GET(WDT_NODE);
static int wdt_channel_id;
#else
#define HAS_WATCHDOG 0
#endif

/*控制LED*/
static int set_debug_leds(char color)
{
#if HAS_RGB_LEDS
	(void)gpio_pin_set_dt(&led_r, color == 'R');
	(void)gpio_pin_set_dt(&led_g, color == 'G');
	(void)gpio_pin_set_dt(&led_b, color == 'B');
#else
	ARG_UNUSED(color);
#endif
	return 0;
}

static int debug_leds_init(void)
{
#if HAS_RGB_LEDS
	if (!gpio_is_ready_dt(&led_r) || !gpio_is_ready_dt(&led_g) || !gpio_is_ready_dt(&led_b)) {
		return -ENODEV;
	}

	(void)gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
	(void)gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
	(void)gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);
#endif
	return 0;
}

/*颜色转换成舵机脉冲，这时会向上查表，负责告诉后面该用多少us的PWM脉冲*/
static uint32_t color_pulse_us(char color)
{
	switch (color) {
	case 'R':
		return SERVO_RED_US;
	case 'G':
		return SERVO_GREEN_US;
	case 'B':
		return SERVO_BLUE_US;
	case 'U':
	default:
		return SERVO_UNKNOWN_US;
	}
}

static int servo_init(void)
{
#if HAS_SERVO
	if (!pwm_is_ready_dt(&servo)) {
		return -ENODEV;
	}

	return pwm_set_dt(&servo, SERVO_PERIOD, PWM_USEC(SERVO_UNKNOWN_US));
#else
	LOG_WRN("No sorter-servo alias found; motor commands will only be logged");
	return 0;
#endif
}

static int watchdog_init(void)
{
#if HAS_WATCHDOG
	int ret;

	if (!device_is_ready(wdt)) {
		LOG_ERR("Watchdog device not ready");
		return -ENODEV;
	}

	struct wdt_timeout_cfg wdt_config = {
		.flags = WDT_FLAG_RESET_SOC,
		.window = {
			.min = 0,
			.max = WDT_TIMEOUT_MS,
		},
	};

	wdt_channel_id = wdt_install_timeout(wdt, &wdt_config);
	if (wdt_channel_id < 0) {
		LOG_ERR("Watchdog install timeout failed: %d", wdt_channel_id);
		return wdt_channel_id;
	}

	ret = wdt_setup(wdt, 0);
	if (ret < 0) {
		LOG_ERR("Watchdog setup failed: %d", ret);
		return ret;
	}

	LOG_INF("Watchdog enabled, timeout=%d ms", WDT_TIMEOUT_MS);
	return 0;
#else
	LOG_WRN("No watchdog device found");
	return 0;
#endif
}

/*真正的控制舵机*/
static int servo_set_color(char color)
{
#if HAS_SERVO
	return pwm_set_dt(&servo, SERVO_PERIOD, PWM_USEC(color_pulse_us(color))); /*PWM输出=20ms周期+color对应的脉冲宽度*/
#else
	ARG_UNUSED(color);
	return 0;
#endif
}

/*执行颜色命令*/
static void motor_apply_command(char color)
{
	int ret;
	/*判断颜色*/
	switch (color) {
	case 'R':
		LOG_INF("MOTOR COMMAND: RED angle=0 pulse=%uus", SERVO_RED_US);
		break;
	case 'G':
		LOG_INF("MOTOR COMMAND: GREEN angle=60 pulse=%uus", SERVO_GREEN_US);
		break;
	case 'B':
		LOG_INF("MOTOR COMMAND: BLUE angle=120 pulse=%uus", SERVO_BLUE_US);
		break;
	case 'U':
		LOG_INF("MOTOR COMMAND: UNKNOWN/IDLE angle=180 pulse=%uus", SERVO_UNKNOWN_US);
		break;
	default:
		LOG_WRN("Ignored invalid color command: 0x%02x", color);
		return;
	}
	/*让舵机转动*/
	ret = servo_set_color(color);
	if (ret < 0) {
	LOG_ERR("Servo update failed: %d", ret);
	}

	/* Update persistent sorting statistics */
	update_sorting_counts(color);

	/*让LED显示与监测到的相同的颜色*/
	(void)set_debug_leds(color);
}

#define MOTOR_THREAD_STACK_SIZE 1024
#define MOTOR_THREAD_PRIORITY 5

static void motor_thread(void *p1, void *p2, void *p3)
{
	char color;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		if (k_msgq_get(&color_msgq, &color, K_FOREVER) == 0) {
			motor_apply_command(color);
		}
	}
}

K_THREAD_DEFINE(motor_thread_id,
		MOTOR_THREAD_STACK_SIZE,
		motor_thread,
		NULL,
		NULL,
		NULL,
		MOTOR_THREAD_PRIORITY,
		0,
		0);

/*拆蓝牙广告包*/
static bool ad_parse_cb(struct bt_data *data, void *user_data)
{
	struct color_packet *packet = user_data;

	if (data->type != BT_DATA_MANUFACTURER_DATA || data->data_len < 9U) {
		return true;
	}
	/*[厂家ID 2字节] [项目ID 4字节] [命令类型 1字节] [颜色 1字节] [序号 1字节]*/
	packet->mfg_id = sys_get_le16(&data->data[0]);
	packet->project_id = sys_get_le32(&data->data[2]);
	packet->cmd_type = data->data[6];
	packet->color = (char)data->data[7];
	packet->sequence = data->data[8];
	packet->valid = true;

	return false;
}

/*重要：蓝牙接收的逻辑*/
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad)
{
	struct color_packet packet = { 0 };
	struct net_buf_simple ad_copy = *ad;

	ARG_UNUSED(addr);
	ARG_UNUSED(type);

	bt_data_parse(&ad_copy, ad_parse_cb, &packet); /*解析蓝牙包*/
	/*检查是不是我们需要的包*/
	if (!packet.valid || packet.mfg_id != FINAL_PROJECT_MFG_ID ||
	    packet.project_id != FINAL_PROJECT_ID || packet.cmd_type != COLOR_CMD_TYPE) {
		return;
	}
	/*防止重复：意思是当这次接收到的数据和上次一样，那就不会重复转动舵机*/
	if (packet.sequence == last_sequence && packet.color == last_color) {
		return;
	}
	/*持续3s的保护时间*/
	last_sequence = packet.sequence;
	last_color = packet.color;
	int64_t now_ms = k_uptime_get();
	if (now_ms < hold_until_ms) {
    return;
	}

	LOG_INF("RX color=%c seq=%u rssi=%d", packet.color, packet.sequence, rssi);

	/*根据颜色来执行动作*/
	char motor_cmd;
	if (packet.color == 'R' || packet.color == 'G' || packet.color == 'B') {
	motor_cmd = packet.color;
	hold_until_ms = now_ms + MOTOR_HOLD_MS; /*当前时间+3000 ms*/
	} 
	else {
		motor_cmd = 'U';
	}

	if (k_msgq_put(&color_msgq, &motor_cmd, K_NO_WAIT) != 0) {
	LOG_WRN("Colour message queue full, command dropped");
	}
}


int main(void)
{
	int ret;
	static const struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.options = 0,
		.interval = 0x00A0,
		.window = 0x0030,
	};
	
	/*初始化LED，舵机和启动蓝牙*/
	ret = debug_leds_init();
	if (ret < 0) {
		LOG_WRN("Debug LED init failed: %d", ret);
	}

	ret = storage_init();
	if (ret < 0) {
	LOG_WRN("Storage init failed: %d", ret);
	}

	ret = servo_init();
	if (ret < 0) {
		LOG_ERR("Servo init failed: %d", ret);
		return 0;
	}

	ret = watchdog_init();
	if (ret < 0) {
	LOG_WRN("Watchdog init failed: %d", ret);
	}

	ret = bt_enable(NULL);
	if (ret < 0) {
		LOG_ERR("Bluetooth init failed: %d", ret);
		return 0;
	}
	/*开始扫描蓝牙广播*/
	ret = bt_le_scan_start(&scan_param, scan_cb);
	if (ret < 0) {
		LOG_ERR("Scan start failed: %d", ret);
		return 0;
	}

	
	LOG_INF("FinalProject color RX ready");
	motor_apply_command('U');
	k_msleep(3000);
	LOG_INF("Servo initialised to UNKNOWN position");
	LOG_INF("Waiting for payload id=0x%08x type=0x%02x",
		FINAL_PROJECT_ID,
        COLOR_CMD_TYPE);
	while (1) {
	#if HAS_WATCHDOG
		ret = wdt_feed(wdt, wdt_channel_id);
		if (ret < 0) {
			LOG_ERR("Watchdog feed failed: %d", ret);
		}
	#endif
		k_msleep(1000);
}
return 0;	
}