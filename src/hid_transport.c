#include "hid_transport.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <bluetooth/services/hids.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/services/dis.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/class/hid.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/usb/usb_device.h>

LOG_MODULE_REGISTER(hid_transport, LOG_LEVEL_INF);

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define BASE_USB_HID_SPEC_VERSION 0x0101
#define INPUT_REP_KEYS_IDX 0
#define INPUT_REP_CONSUMER_IDX 1
#define OUTPUT_REP_KEYS_IDX 0
#define INPUT_REP_KEYS_REF_ID 1
#define INPUT_REP_CONSUMER_REF_ID 2
#define OUTPUT_REP_KEYS_REF_ID 1
#define OUTPUT_REPORT_MAX_LEN 1
#define CONSUMER_REPORT_SIZE 1

BT_HIDS_DEF(hids_obj, OUTPUT_REPORT_MAX_LEN, HID_KEYBOARD_REPORT_SIZE);

struct conn_mode {
	struct bt_conn *conn;
	bool in_boot_mode;
};

static struct conn_mode conn_mode[CONFIG_BT_HIDS_MAX_CLIENT_COUNT];
static const struct device *usb_hid_dev;
static struct k_sem usb_ep_sem;
static enum app_mode current_mode = APP_MODE_BLE;
static enum usb_dc_status_code usb_status;
static bool bt_ready;
static bool adv_active;
static bool usb_ready;

static const uint8_t report_map[] = {
	/* Keyboard report, ID 1. */
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, INPUT_REP_KEYS_REF_ID,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00,
	0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06,
	0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
	0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
	0x85, OUTPUT_REP_KEYS_REF_ID, 0x95, 0x05, 0x75, 0x01,
	0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
	0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0xc0,

	/* Consumer control report, ID 2. Bits: mute, volume up, volume down. */
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, INPUT_REP_CONSUMER_REF_ID,
	0x15, 0x00, 0x25, 0x01, 0x09, 0xe2, 0x09, 0xe9, 0x09, 0xea,
	0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x75, 0x05,
	0x95, 0x01, 0x81, 0x03, 0xc0,
};

static const struct bt_data ad[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
					  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static uint8_t consumer_usage_to_bits(uint16_t usage)
{
	switch (usage) {
	case HID_CONSUMER_MUTE:
		return BIT(0);
	case HID_CONSUMER_VOLUME_UP:
		return BIT(1);
	case HID_CONSUMER_VOLUME_DOWN:
		return BIT(2);
	default:
		return 0;
	}
}

static void advertising_start(void)
{
	int err;
	const struct bt_le_adv_param *adv_param =
		BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
				BT_GAP_ADV_FAST_INT_MIN_2,
				BT_GAP_ADV_FAST_INT_MAX_2,
				NULL);

	if (!bt_ready || current_mode != APP_MODE_BLE || adv_active) {
		return;
	}

	err = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err == -EALREADY) {
		adv_active = true;
		return;
	}

	if (err) {
		LOG_ERR("Bluetooth advertising start failed: %d", err);
		return;
	}

	adv_active = true;
	LOG_INF("Bluetooth advertising started");
}

static void advertising_stop(void)
{
	if (!bt_ready || !adv_active) {
		return;
	}

	if (bt_le_adv_stop() == 0) {
		adv_active = false;
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("Bluetooth connection failed: 0x%02x", err);
		return;
	}

	LOG_INF("Bluetooth connected");
	adv_active = false;

	err = bt_hids_connected(&hids_obj, conn);
	if (err) {
		LOG_WRN("HIDS connected callback failed: %d", err);
	}

	for (size_t i = 0; i < ARRAY_SIZE(conn_mode); i++) {
		if (conn_mode[i].conn == NULL) {
			conn_mode[i].conn = bt_conn_ref(conn);
			conn_mode[i].in_boot_mode = false;
			break;
		}
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Bluetooth disconnected: 0x%02x", reason);
	(void)bt_hids_disconnected(&hids_obj, conn);

	for (size_t i = 0; i < ARRAY_SIZE(conn_mode); i++) {
		if (conn_mode[i].conn == conn) {
			bt_conn_unref(conn_mode[i].conn);
			conn_mode[i].conn = NULL;
			conn_mode[i].in_boot_mode = false;
		}
	}

	advertising_start();
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	LOG_INF("Bluetooth security level %u err %d", level, err);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static void hids_outp_rep_handler(struct bt_hids_rep *rep,
				  struct bt_conn *conn, bool write)
{
	ARG_UNUSED(conn);

	if (write && rep->data != NULL) {
		LOG_INF("BLE keyboard LEDs: 0x%02x", *rep->data);
	}
}

static void hids_boot_kb_outp_rep_handler(struct bt_hids_rep *rep,
					  struct bt_conn *conn, bool write)
{
	hids_outp_rep_handler(rep, conn, write);
}

static void hids_pm_evt_handler(enum bt_hids_pm_evt evt, struct bt_conn *conn)
{
	for (size_t i = 0; i < ARRAY_SIZE(conn_mode); i++) {
		if (conn_mode[i].conn == conn) {
			conn_mode[i].in_boot_mode = (evt == BT_HIDS_PM_EVT_BOOT_MODE_ENTERED);
			break;
		}
	}
}

static int ble_hids_init(void)
{
	struct bt_hids_init_param hids_init = { 0 };
	struct bt_hids_inp_rep *inp_rep;
	struct bt_hids_outp_feat_rep *out_rep;
	int err;

	hids_init.rep_map.data = report_map;
	hids_init.rep_map.size = sizeof(report_map);
	hids_init.info.bcd_hid = BASE_USB_HID_SPEC_VERSION;
	hids_init.info.b_country_code = 0;
	hids_init.info.flags = BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE;

	inp_rep = &hids_init.inp_rep_group_init.reports[INPUT_REP_KEYS_IDX];
	inp_rep->size = HID_KEYBOARD_REPORT_SIZE;
	inp_rep->id = INPUT_REP_KEYS_REF_ID;
	hids_init.inp_rep_group_init.cnt++;

	inp_rep = &hids_init.inp_rep_group_init.reports[INPUT_REP_CONSUMER_IDX];
	inp_rep->size = CONSUMER_REPORT_SIZE;
	inp_rep->id = INPUT_REP_CONSUMER_REF_ID;
	hids_init.inp_rep_group_init.cnt++;

	out_rep = &hids_init.outp_rep_group_init.reports[OUTPUT_REP_KEYS_IDX];
	out_rep->size = OUTPUT_REPORT_MAX_LEN;
	out_rep->id = OUTPUT_REP_KEYS_REF_ID;
	out_rep->handler = hids_outp_rep_handler;
	hids_init.outp_rep_group_init.cnt++;

	hids_init.is_kb = true;
	hids_init.boot_kb_outp_rep_handler = hids_boot_kb_outp_rep_handler;
	hids_init.pm_evt_handler = hids_pm_evt_handler;

	err = bt_hids_init(&hids_obj, &hids_init);
	if (err) {
		LOG_ERR("HIDS init failed: %d", err);
	}

	return err;
}

static void bt_ready_cb(int err)
{
	if (err) {
		LOG_ERR("Bluetooth init failed: %d", err);
		return;
	}

	bt_ready = true;
	LOG_INF("Bluetooth initialized");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		(void)settings_load();
	}

	advertising_start();
}

static void usb_status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
	ARG_UNUSED(param);

	usb_status = status;
	switch (status) {
	case USB_DC_CONNECTED:
	case USB_DC_CONFIGURED:
	case USB_DC_RESUME:
		usb_ready = true;
		break;
	case USB_DC_DISCONNECTED:
	case USB_DC_RESET:
	case USB_DC_ERROR:
		usb_ready = false;
		break;
	default:
		break;
	}
	LOG_INF("USB status: %d ready=%d", status, usb_ready);
}

static void usb_int_in_ready_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
	k_sem_give(&usb_ep_sem);
}

static const struct hid_ops usb_ops = {
	.int_in_ready = usb_int_in_ready_cb,
};

static int usb_hid_init_transport(void)
{
	int err;

	usb_hid_dev = device_get_binding("HID_0");
	if (usb_hid_dev == NULL) {
		LOG_ERR("USB HID device not found");
		return -ENODEV;
	}

	usb_hid_register_device(usb_hid_dev, report_map, sizeof(report_map), &usb_ops);
	err = usb_hid_init(usb_hid_dev);
	if (err) {
		LOG_ERR("USB HID init failed: %d", err);
		return err;
	}

	err = usb_enable(usb_status_cb);
	if (err) {
		LOG_ERR("USB enable failed: %d", err);
		return err;
	}

	return 0;
}

int hid_transport_init(void)
{
	int err;

	k_sem_init(&usb_ep_sem, 0, 1);

	err = usb_hid_init_transport();
	if (err) {
		LOG_WRN("USB HID unavailable: %d", err);
	}

	err = ble_hids_init();
	if (err) {
		return err;
	}

	err = bt_enable(bt_ready_cb);
	if (err) {
		LOG_ERR("Bluetooth enable failed: %d", err);
		return err;
	}

	return 0;
}

void hid_transport_set_mode(enum app_mode mode)
{
	if (current_mode == mode) {
		return;
	}

	current_mode = mode;

	if (mode == APP_MODE_BLE) {
		advertising_start();
	} else {
		advertising_stop();
	}
}

enum app_mode hid_transport_get_mode(void)
{
	return current_mode;
}

bool hid_transport_connected(void)
{
	if (current_mode == APP_MODE_USB) {
		return usb_ready;
	}

	for (size_t i = 0; i < ARRAY_SIZE(conn_mode); i++) {
		if (conn_mode[i].conn != NULL) {
			return true;
		}
	}

	return false;
}

static int ble_send_keyboard(const struct hid_keyboard_report *report)
{
	uint8_t data[HID_KEYBOARD_REPORT_SIZE];
	int ret = 0;

	data[0] = report->modifiers;
	data[1] = report->reserved;
	memcpy(&data[2], report->keys, sizeof(report->keys));

	for (size_t i = 0; i < ARRAY_SIZE(conn_mode); i++) {
		if (conn_mode[i].conn == NULL) {
			continue;
		}

		if (conn_mode[i].in_boot_mode) {
			ret = bt_hids_boot_kb_inp_rep_send(&hids_obj,
							   conn_mode[i].conn,
							   data, sizeof(data),
							   NULL);
		} else {
			ret = bt_hids_inp_rep_send(&hids_obj, conn_mode[i].conn,
						   INPUT_REP_KEYS_IDX, data,
						   sizeof(data), NULL);
		}
		if (ret) {
			return ret;
		}
	}

	return 0;
}

static int usb_send_report(uint8_t report_id, const uint8_t *data, size_t len)
{
	uint8_t report[1 + HID_KEYBOARD_REPORT_SIZE] = { 0 };
	int err;

	if (usb_hid_dev == NULL || !usb_ready) {
		return -ENOTCONN;
	}

	if (IS_ENABLED(CONFIG_USB_DEVICE_REMOTE_WAKEUP) && usb_status == USB_DC_SUSPEND) {
		(void)usb_wakeup_request();
	}

	report[0] = report_id;
	memcpy(&report[1], data, len);
	err = hid_int_ep_write(usb_hid_dev, report, len + 1, NULL);
	if (err) {
		return err;
	}

	(void)k_sem_take(&usb_ep_sem, K_MSEC(20));
	return 0;
}

int hid_transport_send_keyboard(const struct hid_keyboard_report *report)
{
	uint8_t data[HID_KEYBOARD_REPORT_SIZE];

	if (report == NULL || current_mode == APP_MODE_OFF) {
		return -EINVAL;
	}

	if (current_mode == APP_MODE_BLE) {
		return ble_send_keyboard(report);
	}

	data[0] = report->modifiers;
	data[1] = report->reserved;
	memcpy(&data[2], report->keys, sizeof(report->keys));
	return usb_send_report(INPUT_REP_KEYS_REF_ID, data, sizeof(data));
}

static int ble_send_consumer_bits(uint8_t bits)
{
	int ret = 0;

	for (size_t i = 0; i < ARRAY_SIZE(conn_mode); i++) {
		if (conn_mode[i].conn == NULL) {
			continue;
		}

		ret = bt_hids_inp_rep_send(&hids_obj, conn_mode[i].conn,
					   INPUT_REP_CONSUMER_IDX,
					   &bits, sizeof(bits), NULL);
		if (ret) {
			return ret;
		}
	}

	return 0;
}

int hid_transport_send_consumer(uint16_t usage)
{
	uint8_t bits = consumer_usage_to_bits(usage);
	int err;

	if (bits == 0 || current_mode == APP_MODE_OFF) {
		return -EINVAL;
	}

	if (current_mode == APP_MODE_BLE) {
		err = ble_send_consumer_bits(bits);
		if (!err) {
			k_sleep(K_MSEC(10));
			err = ble_send_consumer_bits(0);
		}
		return err;
	}

	err = usb_send_report(INPUT_REP_CONSUMER_REF_ID, &bits, sizeof(bits));
	if (!err) {
		uint8_t release = 0;

		k_sleep(K_MSEC(10));
		err = usb_send_report(INPUT_REP_CONSUMER_REF_ID, &release, sizeof(release));
	}

	return err;
}
