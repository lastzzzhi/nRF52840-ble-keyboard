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

#include "host_protocol.h"
#include "host_ble.h"
#include "power.h"

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
#define FEATURE_REP_HOST_REF_ID HOST_REPORT_ID
#define OUTPUT_REPORT_MAX_LEN 1
#define CONSUMER_REPORT_SIZE 1
/* Advertise quickly for initial pairing, then fall back to slower intervals. */
#define BLE_FAST_ADV_TIMEOUT_MS 300000
#define BLE_ADV_SLOW_INT_MIN 0x0c80
#define BLE_ADV_SLOW_INT_MAX 0x1000
/* Request a modest idle-friendly BLE connection interval after connection. */
#define BLE_CONN_INT_MIN 24
#define BLE_CONN_INT_MAX 40
#define BLE_CONN_LATENCY 4
#define BLE_CONN_TIMEOUT 400
#define CONSUMER_QUEUE_LEN 8
#define CONSUMER_PULSE_MS 10
#define HID_RETRY_MS 5
#define USB_HID_REPORT_TYPE_OUTPUT 2
#define USB_HID_REPORT_TYPE_FEATURE 3
#define HID_LED_NUMLOCK BIT(0)

BT_HIDS_DEF(hids_obj, OUTPUT_REPORT_MAX_LEN, HID_KEYBOARD_REPORT_SIZE);
K_MSGQ_DEFINE(consumer_msgq, sizeof(uint8_t), CONSUMER_QUEUE_LEN, 1);

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
static bool adv_slow;
static bool usb_ready;
static bool ble_hids_ready;
static bool ble_enable_started;
static bool force_repair_next;
static bool usb_boot_protocol;
static bool keyboard_pending;
static struct hid_keyboard_report keyboard_latest;
static struct k_mutex tx_lock;
static struct k_work_delayable tx_work;
static int64_t adv_fast_until_ms;
static int64_t consumer_release_due_ms;
static uint8_t active_consumer_bits;

enum consumer_tx_state {
	CONSUMER_TX_IDLE,
	CONSUMER_TX_PRESS,
	CONSUMER_TX_RELEASE,
};

static enum consumer_tx_state consumer_tx_state;

static void hid_tx_work_handler(struct k_work *work);
static void hid_tx_process(void);
static int hid_raw_send_keyboard(const struct hid_keyboard_report *report);
static int hid_raw_send_consumer_bits(uint8_t bits);
static int ble_send_consumer_bits(uint8_t bits);

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

	/* Vendor feature report, ID 3, 63-byte payload for host control. */
	0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01,
	0x85, FEATURE_REP_HOST_REF_ID,
	0x15, 0x00, 0x26, 0xff, 0x00,
	0x75, 0x08, 0x95, HOST_REPORT_SIZE - 1,
	0x09, 0x01, 0xb1, 0x02, 0xc0,
};

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
					  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, HOST_BLE_SERVICE_UUID_VAL),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
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

static bool ble_has_connection(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(conn_mode); i++) {
		if (conn_mode[i].conn != NULL) {
			return true;
		}
	}

	return false;
}

static size_t ble_connection_count(void)
{
	size_t count = 0;

	for (size_t i = 0; i < ARRAY_SIZE(conn_mode); i++) {
		if (conn_mode[i].conn != NULL) {
			count++;
		}
	}

	return count;
}

static void advertising_start(bool slow)
{
	int err;
	const struct bt_le_adv_param *adv_param =
		slow ? BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
				       BLE_ADV_SLOW_INT_MIN,
				       BLE_ADV_SLOW_INT_MAX,
				       NULL) :
		       BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
				       BT_GAP_ADV_FAST_INT_MIN_2,
				       BT_GAP_ADV_FAST_INT_MAX_2,
				       NULL);

	if (!bt_ready || current_mode != APP_MODE_BLE || adv_active ||
	    ble_connection_count() >= ARRAY_SIZE(conn_mode)) {
		return;
	}

	err = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err == -EALREADY) {
		adv_active = true;
		return;
	}

	if (err) {
		LOG_ERR("Bluetooth advertising start failed: %d", err);
		return;
	}

	adv_active = true;
	adv_slow = slow;
	LOG_INF("Bluetooth %s advertising started", slow ? "slow" : "fast");
}

static void advertising_stop(void)
{
	if (!bt_ready || !adv_active) {
		return;
	}

	if (bt_le_adv_stop() == 0) {
		adv_active = false;
		adv_slow = false;
	}
}

static void advertising_restart(bool slow)
{
	advertising_stop();
	advertising_start(slow);
}

static void advertising_begin_fast_window(void)
{
	power_ip5306_keepalive_kick();
	adv_fast_until_ms = k_uptime_get() + BLE_FAST_ADV_TIMEOUT_MS;
	advertising_restart(false);
}

static void advertising_resume_window(void)
{
	int64_t now = k_uptime_get();

	if (adv_fast_until_ms == 0 || now < adv_fast_until_ms) {
		advertising_restart(false);
	} else {
		advertising_restart(true);
	}
}

static void disconnect_ble_connections(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(conn_mode); i++) {
		if (conn_mode[i].conn != NULL) {
			(void)bt_conn_disconnect(conn_mode[i].conn,
						 BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		}
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	bt_security_t security = BT_SECURITY_L2;
	struct bt_le_conn_param conn_param = {
		.interval_min = BLE_CONN_INT_MIN,
		.interval_max = BLE_CONN_INT_MAX,
		.latency = BLE_CONN_LATENCY,
		.timeout = BLE_CONN_TIMEOUT,
	};

	if (err) {
		LOG_WRN("Bluetooth connection failed: 0x%02x", err);
		return;
	}

	if (current_mode != APP_MODE_BLE) {
		LOG_INF("Rejecting BLE connection outside BLE mode");
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	LOG_INF("Bluetooth connected");
	power_ip5306_keepalive_kick();
	adv_active = false;

	if (force_repair_next) {
		LOG_INF("Forcing Bluetooth re-pair on this connection");
		security |= BT_SECURITY_FORCE_PAIR;
	}

	err = bt_conn_set_security(conn, security);
	if (err) {
		LOG_WRN("Bluetooth security request failed: %d", err);
	}

	err = bt_conn_le_param_update(conn, &conn_param);
	if (err) {
		LOG_DBG("Bluetooth connection parameter update request failed: %d", err);
	}

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

	if (ble_connection_count() < ARRAY_SIZE(conn_mode)) {
		LOG_INF("Bluetooth still has a free connection slot; advertising resumes");
		advertising_resume_window();
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Bluetooth disconnected: 0x%02x", reason);
	power_ip5306_keepalive_kick();
	(void)bt_hids_disconnected(&hids_obj, conn);

	for (size_t i = 0; i < ARRAY_SIZE(conn_mode); i++) {
		if (conn_mode[i].conn == conn) {
			bt_conn_unref(conn_mode[i].conn);
			conn_mode[i].conn = NULL;
			conn_mode[i].in_boot_mode = false;
		}
	}

	if (current_mode == APP_MODE_BLE) {
		advertising_resume_window();
	}
}

static bool hid_retryable_error(int err)
{
	return err == -ENOMEM || err == -EAGAIN || err == -EBUSY ||
	       err == -EINPROGRESS;
}

static void hid_tx_schedule(k_timeout_t delay)
{
	(void)k_work_reschedule(&tx_work, delay);
}

static void hid_tx_clear_pending(void)
{
	keyboard_pending = false;
	consumer_tx_state = CONSUMER_TX_IDLE;
	active_consumer_bits = 0;
	consumer_release_due_ms = 0;
	k_msgq_purge(&consumer_msgq);
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (!err) {
		LOG_INF("Bluetooth security changed: %s level %u", addr, level);
		return;
	}

	LOG_WRN("Bluetooth security failed: %s level %u err %d %s",
		addr, level, err, bt_security_err_to_str(err));

	if (err == BT_SECURITY_ERR_PIN_OR_KEY_MISSING ||
	    err == BT_SECURITY_ERR_AUTH_FAIL) {
		LOG_WRN("Deleting failed bond for %s and forcing re-pair next time", addr);
		force_repair_next = true;
		(void)bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(conn));
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static void auth_pairing_confirm(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Bluetooth pairing confirm: %s", addr);
	err = bt_conn_auth_pairing_confirm(conn);
	if (err) {
		LOG_WRN("Bluetooth pairing confirm failed: %d", err);
	}
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_WRN("Bluetooth pairing cancelled: %s", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Bluetooth pairing complete: %s bonded=%d", addr, bonded);
	force_repair_next = false;
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_WRN("Bluetooth pairing failed: %s reason %d %s",
		addr, reason, bt_security_err_to_str(reason));
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
	.pairing_confirm = auth_pairing_confirm,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
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

	advertising_begin_fast_window();
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
		power_ip5306_keepalive_kick();
		break;
	default:
		break;
	}
}

static void usb_int_in_ready_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
	k_sem_give(&usb_ep_sem);
}

static int usb_set_report_cb(const struct device *dev,
			     struct usb_setup_packet *setup,
			     int32_t *len, uint8_t **data)
{
	uint8_t report_type = (setup->wValue >> 8) & 0xff;
	uint8_t report_id = setup->wValue & 0xff;
	static uint8_t host_set_buf[HOST_REPORT_SIZE];

	ARG_UNUSED(dev);

	if (report_type == USB_HID_REPORT_TYPE_FEATURE &&
	    report_id == FEATURE_REP_HOST_REF_ID) {
		if (len == NULL || data == NULL || *data == NULL ||
		    *len <= 0 || *len > HOST_REPORT_SIZE) {
			return -ENOTSUP;
		}

		memset(host_set_buf, 0, sizeof(host_set_buf));
		if ((*data)[0] == FEATURE_REP_HOST_REF_ID) {
			memcpy(host_set_buf, *data, *len);
		} else {
			host_set_buf[0] = FEATURE_REP_HOST_REF_ID;
			memcpy(&host_set_buf[1], *data,
			       MIN((int32_t)(HOST_REPORT_SIZE - 1), *len));
		}
		return host_protocol_set_report(host_set_buf, HOST_REPORT_SIZE);
	}

	if (report_type != USB_HID_REPORT_TYPE_OUTPUT ||
	    (report_id != 0 && report_id != OUTPUT_REP_KEYS_REF_ID) ||
	    len == NULL || data == NULL || *data == NULL || *len < 1) {
		return -ENOTSUP;
	}

	if (current_mode != APP_MODE_USB) {
		LOG_DBG("Ignoring USB keyboard LEDs outside USB mode: 0x%02x",
			(*data)[0]);
		return 0;
	}

	LOG_INF("USB keyboard LEDs: 0x%02x", (*data)[0]);
	return 0;
}

static int usb_get_report_cb(const struct device *dev,
			     struct usb_setup_packet *setup,
			     int32_t *len, uint8_t **data)
{
	uint8_t report_type = (setup->wValue >> 8) & 0xff;
	uint8_t report_id = setup->wValue & 0xff;
	static uint8_t host_get_buf[HOST_REPORT_SIZE];
	int ret;

	ARG_UNUSED(dev);

	if (report_type != USB_HID_REPORT_TYPE_FEATURE ||
	    report_id != FEATURE_REP_HOST_REF_ID ||
	    len == NULL || data == NULL) {
		return -ENOTSUP;
	}

	ret = host_protocol_get_report(host_get_buf, sizeof(host_get_buf));
	if (ret < 0) {
		return ret;
	}

	*data = host_get_buf;
	*len = MIN((int32_t)setup->wLength, (int32_t)sizeof(host_get_buf));
	return 0;
}

static void usb_protocol_change_cb(const struct device *dev, uint8_t protocol)
{
	ARG_UNUSED(dev);

	usb_boot_protocol = (protocol == HID_PROTOCOL_BOOT);
	LOG_INF("USB protocol changed to %s",
		usb_boot_protocol ? "Boot Protocol" : "Report Protocol");
}

static const struct hid_ops usb_ops = {
	.get_report = usb_get_report_cb,
	.set_report = usb_set_report_cb,
	.protocol_change = usb_protocol_change_cb,
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
	err = usb_hid_set_proto_code(usb_hid_dev, HID_BOOT_IFACE_CODE_KEYBOARD);
	if (err) {
		LOG_WRN("USB HID boot protocol code setup failed: %d", err);
	}

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

static int ble_transport_start(void)
{
	int err;

	if (bt_ready || ble_enable_started) {
		advertising_begin_fast_window();
		return 0;
	}

	if (!ble_hids_ready) {
		err = bt_conn_auth_cb_register(&conn_auth_callbacks);
		if (err && err != -EALREADY) {
			LOG_ERR("Bluetooth auth callback register failed: %d", err);
			return err;
		}

		err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
		if (err && err != -EALREADY) {
			LOG_ERR("Bluetooth auth info callback register failed: %d", err);
			return err;
		}

		err = ble_hids_init();
		if (err) {
			return err;
		}
		ble_hids_ready = true;
	}

	ble_enable_started = true;
	err = bt_enable(bt_ready_cb);
	if (err) {
		ble_enable_started = false;
		LOG_ERR("Bluetooth enable failed: %d", err);
		return err;
	}

	return 0;
}

int hid_transport_init(enum app_mode initial_mode)
{
	int err;

	current_mode = initial_mode;
	host_protocol_init();
	k_sem_init(&usb_ep_sem, 0, 1);
	k_mutex_init(&tx_lock);
	k_work_init_delayable(&tx_work, hid_tx_work_handler);

	err = usb_hid_init_transport();
	if (err) {
		LOG_WRN("USB HID unavailable: %d", err);
	}

	if (initial_mode == APP_MODE_BLE) {
		err = ble_transport_start();
		if (err) {
			return err;
		}
	} else {
		LOG_INF("Bluetooth init deferred in non-BLE mode");
	}

	return 0;
}

void hid_transport_set_mode(enum app_mode mode)
{
	if (current_mode == mode) {
		return;
	}

	(void)hid_transport_release_all();

	k_mutex_lock(&tx_lock, K_FOREVER);
	hid_tx_clear_pending();
	current_mode = mode;
	k_mutex_unlock(&tx_lock);

	if (mode == APP_MODE_BLE) {
		(void)ble_transport_start();
	} else {
		advertising_stop();
		disconnect_ble_connections();
	}
}

void hid_transport_tick(void)
{
	hid_tx_process();

	if (current_mode == APP_MODE_BLE && bt_ready && !ble_has_connection() &&
	    adv_active && !adv_slow && k_uptime_get() >= adv_fast_until_ms) {
		advertising_restart(true);
	}
}

enum app_mode hid_transport_get_mode(void)
{
	return current_mode;
}

bool hid_transport_ble_ready(void)
{
	return bt_ready;
}

bool hid_transport_connected(void)
{
	if (current_mode == APP_MODE_USB) {
		return usb_ready;
	}

	return ble_has_connection();
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

static int usb_send_keyboard(const uint8_t *data, size_t len)
{
	int err;

	if (usb_hid_dev == NULL || !usb_ready) {
		return -ENOTCONN;
	}

	if (!usb_boot_protocol) {
		return usb_send_report(INPUT_REP_KEYS_REF_ID, data, len);
	}

	if (IS_ENABLED(CONFIG_USB_DEVICE_REMOTE_WAKEUP) && usb_status == USB_DC_SUSPEND) {
		(void)usb_wakeup_request();
	}

	err = hid_int_ep_write(usb_hid_dev, data, len, NULL);
	if (err) {
		return err;
	}

	(void)k_sem_take(&usb_ep_sem, K_MSEC(20));
	return 0;
}

static int hid_raw_send_keyboard(const struct hid_keyboard_report *report)
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
	return usb_send_keyboard(data, sizeof(data));
}

static int hid_raw_send_consumer_bits(uint8_t bits)
{
	if (current_mode == APP_MODE_BLE) {
		return ble_send_consumer_bits(bits);
	}

	if (usb_boot_protocol) {
		return -ENOTSUP;
	}

	return usb_send_report(INPUT_REP_CONSUMER_REF_ID, &bits, sizeof(bits));
}

static void hid_tx_process_locked(void)
{
	int err;
	int64_t now = k_uptime_get();

	if (keyboard_pending) {
		err = hid_raw_send_keyboard(&keyboard_latest);
		if (!err) {
			keyboard_pending = false;
		} else if (hid_retryable_error(err)) {
			hid_tx_schedule(K_MSEC(HID_RETRY_MS));
			return;
		} else {
			keyboard_pending = false;
		}
	}

	if (consumer_tx_state == CONSUMER_TX_RELEASE) {
		if (now < consumer_release_due_ms) {
			hid_tx_schedule(K_MSEC(consumer_release_due_ms - now));
			return;
		}

		err = hid_raw_send_consumer_bits(0);
		if (!err) {
			consumer_tx_state = CONSUMER_TX_IDLE;
			active_consumer_bits = 0;
		} else if (hid_retryable_error(err)) {
			hid_tx_schedule(K_MSEC(HID_RETRY_MS));
			return;
		} else {
			consumer_tx_state = CONSUMER_TX_IDLE;
			active_consumer_bits = 0;
		}
	}

	if (consumer_tx_state == CONSUMER_TX_IDLE) {
		if (k_msgq_get(&consumer_msgq, &active_consumer_bits, K_NO_WAIT) != 0) {
			return;
		}
		consumer_tx_state = CONSUMER_TX_PRESS;
	}

	if (consumer_tx_state == CONSUMER_TX_PRESS) {
		err = hid_raw_send_consumer_bits(active_consumer_bits);
		if (!err) {
			consumer_release_due_ms = now + CONSUMER_PULSE_MS;
			consumer_tx_state = CONSUMER_TX_RELEASE;
			hid_tx_schedule(K_MSEC(CONSUMER_PULSE_MS));
		} else if (hid_retryable_error(err)) {
			hid_tx_schedule(K_MSEC(HID_RETRY_MS));
		} else {
			consumer_tx_state = CONSUMER_TX_IDLE;
			active_consumer_bits = 0;
			if (k_msgq_num_used_get(&consumer_msgq) > 0) {
				hid_tx_schedule(K_NO_WAIT);
			}
		}
	}
}

static void hid_tx_process(void)
{
	k_mutex_lock(&tx_lock, K_FOREVER);
	hid_tx_process_locked();
	k_mutex_unlock(&tx_lock);
}

static void hid_tx_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	hid_tx_process();
}

int hid_transport_send_keyboard(const struct hid_keyboard_report *report)
{
	int err;

	if (report == NULL || current_mode == APP_MODE_OFF) {
		return -EINVAL;
	}

	k_mutex_lock(&tx_lock, K_FOREVER);
	err = hid_raw_send_keyboard(report);
	if (!err) {
		keyboard_pending = false;
	} else if (hid_retryable_error(err)) {
		keyboard_latest = *report;
		keyboard_pending = true;
		hid_tx_schedule(K_MSEC(HID_RETRY_MS));
	} else {
		keyboard_pending = false;
	}
	k_mutex_unlock(&tx_lock);

	return err;
}

int hid_transport_release_all(void)
{
	struct hid_keyboard_report empty_keyboard = { 0 };
	int err;
	int consumer_err;

	if (current_mode == APP_MODE_OFF) {
		return 0;
	}

	k_mutex_lock(&tx_lock, K_FOREVER);
	hid_tx_clear_pending();
	err = hid_raw_send_keyboard(&empty_keyboard);
	consumer_err = hid_raw_send_consumer_bits(0);
	k_mutex_unlock(&tx_lock);

	return err ? err : consumer_err;
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
	int err = 0;

	if (bits == 0 || current_mode == APP_MODE_OFF) {
		return -EINVAL;
	}

	k_mutex_lock(&tx_lock, K_FOREVER);
	if (!hid_transport_connected()) {
		err = -ENOTCONN;
	} else if (k_msgq_put(&consumer_msgq, &bits, K_NO_WAIT) != 0) {
		err = -ENOSPC;
	} else {
		hid_tx_process_locked();
	}
	k_mutex_unlock(&tx_lock);

	return err;
}
