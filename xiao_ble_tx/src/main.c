#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(color_tx, LOG_LEVEL_INF);
//BLE的广播标识
#define FINAL_PROJECT_MFG_ID 0x1140U
#define FINAL_PROJECT_ID     0x4011F001U
#define COLOR_CMD_TYPE       0xC1U
#define WDT_TIMEOUT_MS       4000U
#define WDT_FEED_INTERVAL_MS 1000U

//watchdog设备检查
#if DT_NODE_HAS_STATUS(DT_NODELABEL(wdt0), okay)
#define HAS_WATCHDOG 1
static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));
#else
#define HAS_WATCHDOG 0
#endif

static atomic_t bt_ready = ATOMIC_INIT(0);     //Bluetooth是否初始化完成
static atomic_t adv_running = ATOMIC_INIT(0);   //BLE advertising是否在正常运行
static atomic_t watchdog_ready = ATOMIC_INIT(0);
static uint8_t sequence;   //颜色命令序列号
static int wdt_channel_id = -1;

//使用了Zephyr的功能: Bluetooth advertising, serial shell, logging, hardware watchdog

/*
 * Manufacturer data layout:
 * [0..1]  manufacturer id, little endian
 * [2..5]  final project id, little endian
 * [6]     command type, 0xC1 means color command
 * [7]     command argument: 'R', 'G', 'B', or 'U'
 * [8]     sequence number, increments when command changes
 */

static uint8_t mfg_data[9] = {
	0x40, 0x11,
	0x01, 0xF0, 0x11, 0x40,
	COLOR_CMD_TYPE,
	'U',
	0x00,
};

//BLE设备名
static const char local_name[] = "FP_COLOR_TX";

//BLE广播数据
static const struct bt_data adv_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, local_name, sizeof(local_name) - 1U),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),  //广播自定义的 9 字节颜色命令数据
};

//启动Hardware看门狗
static int watchdog_start(void)
{
#if HAS_WATCHDOG
	int ret;
	//watchdog的超时设置，超过WDT_TIMEOUT_MS没有喂狗，超时
	struct wdt_timeout_cfg wdt_config = {
		.window = {
			.min = 0U,
			.max = WDT_TIMEOUT_MS,
		},
		.callback = NULL,
		//超时之后直接reset SoC，而不是先进入回调函数处理
		.flags = WDT_FLAG_RESET_SOC,
	};
	//检查watchdog是否ready
	if (!device_is_ready(wdt)) {
		LOG_WRN("Watchdog device is not ready");
		return -ENODEV;
	}

	//安装watchdog timeout
	wdt_channel_id = wdt_install_timeout(wdt, &wdt_config);
	if (wdt_channel_id < 0) {
		LOG_WRN("Watchdog timeout install failed: %d", wdt_channel_id);
		return wdt_channel_id;
	}

	//启动watchdog
	ret = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (ret == -ENOTSUP) {
		ret = wdt_setup(wdt, 0);
	}
	if (ret < 0) {
		LOG_WRN("Watchdog setup failed: %d", ret);
		return ret;
	}

	//设置watchdog ready
	atomic_set(&watchdog_ready, 1);
	LOG_INF("Watchdog started: timeout=%ums feed_interval=%ums",
		WDT_TIMEOUT_MS, WDT_FEED_INTERVAL_MS);
	return 0;
#else
	LOG_WRN("No watchdog device found in devicetree");
	return -ENODEV;
#endif
}

//喂狗线程
static void watchdog_feed_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	//无限循环feed watchdog
	while (true) {
#if HAS_WATCHDOG
		if (atomic_get(&watchdog_ready) && wdt_channel_id >= 0) {
			int ret = wdt_feed(wdt, wdt_channel_id);

			if (ret < 0) {
				LOG_WRN("Watchdog feed failed: %d", ret);
			}
		}
#endif
		k_msleep(WDT_FEED_INTERVAL_MS);
	}
}

//创建watchdog 线程
K_THREAD_DEFINE(watchdog_thread_id, 1024, watchdog_feed_thread,
	NULL, NULL, NULL, 7, 0, 0);

//将 Shell 输入参数转换成标准颜色字符
static char normalize_color_arg(const char *arg)
{
	if (arg == NULL || arg[0] == '\0') {
		return '\0';
	}

	switch (arg[0]) {
	case 'r':
	case 'R':
		return 'R';
	case 'g':
	case 'G':
		return 'G';
	case 'b':
	case 'B':
		return 'B';
	case 'u':
	case 'U':
	case 'i':
	case 'I':
	case '0':
		return 'U';
	default:
		return '\0';
	}
}

//启动BLE advertising
static int adv_start(void)
{
	int err;

	if (!atomic_get(&bt_ready)) {
		return -EAGAIN;
	}

	if (atomic_get(&adv_running)) {
		return 0;
	}

	//是 Zephyr 的 BLE advertising 启动函数
	err = bt_le_adv_start(BT_LE_ADV_NCONN_IDENTITY, adv_data, ARRAY_SIZE(adv_data), NULL, 0);
	//更新状态     如果启动成功，就把adv_running 设置为 1
	if (err == 0) {
		atomic_set(&adv_running, 1);
	}

	return err;
}

//停止BLE advertising
static int adv_stop(void)
{
	int err;

	if (!atomic_get(&adv_running)) {
		return 0;
	}

	err = bt_le_adv_stop();
	if (err == 0) {
		atomic_set(&adv_running, 0);
	}

	return err;
}

//核心函数    检测颜色是否合法
static int publish_color(char color)
{
	int err;

	if (color != 'R' && color != 'G' && color != 'B' && color != 'U') {
		return -EINVAL;
	}
	
	//改变BLE广播内容
	mfg_data[7] = (uint8_t)color;
	mfg_data[8] = ++sequence;

	//如果广播正在运行，就直接更新广播数据
	if (atomic_get(&adv_running)) {
		err = bt_le_adv_update_data(adv_data, ARRAY_SIZE(adv_data), NULL, 0);
		//如果更新失败，代码会打印警告，停止广播，重新启动广播
		if (err < 0) {
			LOG_WRN("Advertising update failed: %d, restarting advertising", err);
			err = adv_stop();
			if (err < 0) {
				return err;
			}
			err = adv_start();
			if (err < 0) {
				return err;
			}
		}
	} else {   //如果当前没在广播，就启动广播
		err = adv_start();
		if (err < 0) {
			return err;
		}
	}
	//打印日志，published color =  seq = 
	LOG_INF("Published color=%c seq=%u", color, sequence);
	return 0;
}

//是shell命令color处理函数
static int cmd_color(const struct shell *sh, size_t argc, char **argv)
{
	char color;
	int ret;

	//检查参数数量
	if (argc != 2) {
		shell_error(sh, "Usage: color r|g|b|u");
		return -EINVAL;
	}

	//规范化颜色参数，把r转成R
	color = normalize_color_arg(argv[1]);
	if (color == '\0') {
		shell_error(sh, "Unknown color '%s'. Use r, g, b, or u.", argv[1]);
		return -EINVAL;
	}

	//把颜色写入BLE manufacturer data 并更新 advertising
	ret = publish_color(color);
	if (ret < 0) {
		shell_error(sh, "publish failed: %d", ret);
		return ret;
	}

	//shell输出
	shell_print(sh, "color=%c", color);
	return 0;
}

//用来手动控制 BLE advertising 的启动和停止  使用adv s	start advertising;  adv p stop advertising
static int cmd_adv(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (argc != 2) {
		shell_error(sh, "Usage: adv s|p");
		return -EINVAL;
	}

	if (strcmp(argv[1], "s") == 0) {
		ret = adv_start();
		if (ret < 0) {
			shell_error(sh, "adv start failed: %d", ret);
			return ret;
		}
		shell_print(sh, "advertising started");
		return 0;
	}

	if (strcmp(argv[1], "p") == 0) {
		ret = adv_stop();
		if (ret < 0) {
			shell_error(sh, "adv stop failed: %d", ret);
			return ret;
		}
		shell_print(sh, "advertising stopped");
		return 0;
	}

	shell_error(sh, "Usage: adv s|p");
	return -EINVAL;
}

//注册shell命令
SHELL_CMD_REGISTER(color, NULL, "Publish color command: color r|g|b|u", cmd_color);
SHELL_CMD_REGISTER(adv, NULL, "Advertising control: adv s|p", cmd_adv);

//主函数循环
int main(void)
{
	int ret;

	//确保manufacturer ID 和 project ID按little endian的方式写入到mfg_data中
	sys_put_le16(FINAL_PROJECT_MFG_ID, &mfg_data[0]);
	sys_put_le32(FINAL_PROJECT_ID, &mfg_data[2]);

	//对watchdog初始化开始
	ret = watchdog_start();
	if (ret < 0) {
		LOG_WRN("Watchdog unavailable: %d", ret);
	}

	//初始化Bluetooth
	ret = bt_enable(NULL);
	if (ret < 0) {
		LOG_ERR("Bluetooth init failed: %d", ret);
		return 0;
	}

	//设置Bluetooth ready
	atomic_set(&bt_ready, 1);

	//启动BLE advertising
	ret = adv_start();
	if (ret < 0) {
		LOG_ERR("Advertising start failed: %d", ret);
		return 0;
	}
	
	//启动成功后打印信息
	LOG_INF("FinalProject color TX ready");
	LOG_INF("Serial shell commands: color r|g|b|u, adv s|p");
	LOG_INF("BLE payload id=0x%08x type=0x%02x color=%c", FINAL_PROJECT_ID, COLOR_CMD_TYPE,
		mfg_data[7]);
	return 0;
}
