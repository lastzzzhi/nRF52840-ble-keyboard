#include "host_ble.h"

#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "host_protocol.h"

LOG_MODULE_REGISTER(host_ble, LOG_LEVEL_INF);

static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(HOST_BLE_SERVICE_UUID_VAL);
static struct bt_uuid_128 request_uuid = BT_UUID_INIT_128(HOST_BLE_REQUEST_UUID_VAL);
static struct bt_uuid_128 response_uuid = BT_UUID_INIT_128(HOST_BLE_RESPONSE_UUID_VAL);
static uint8_t response_value[HOST_REPORT_SIZE];
static bool notify_enabled;

extern const struct bt_gatt_attr attr_host_control_svc[];

static ssize_t response_read(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr, void *buf,
			     uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);

	if (host_protocol_get_report(response_value, sizeof(response_value)) < 0) {
		memset(response_value, 0, sizeof(response_value));
		response_value[0] = HOST_REPORT_ID;
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset, response_value,
				 sizeof(response_value));
}

static ssize_t request_write(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr, const void *buf,
			     uint16_t len, uint16_t offset, uint8_t flags)
{
	uint8_t request[HOST_REPORT_SIZE];
	int err;

	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0 || len != HOST_REPORT_SIZE) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(request, buf, sizeof(request));
	err = host_protocol_set_report(request, sizeof(request));
	if (err) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	if (host_protocol_get_report(response_value, sizeof(response_value)) == 0 &&
	    notify_enabled) {
		(void)bt_gatt_notify(conn, &attr_host_control_svc[4],
				     response_value, sizeof(response_value));
	}

	return len;
}

static void response_ccc_changed(const struct bt_gatt_attr *attr,
				 uint16_t value)
{
	ARG_UNUSED(attr);

	notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Host BLE response notifications %s",
		notify_enabled ? "enabled" : "disabled");
}

BT_GATT_SERVICE_DEFINE(host_control_svc,
	BT_GATT_PRIMARY_SERVICE(&service_uuid.uuid),
	BT_GATT_CHARACTERISTIC(&request_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE,
			       NULL, request_write, NULL),
	BT_GATT_CHARACTERISTIC(&response_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ,
			       response_read, NULL, NULL),
	BT_GATT_CCC(response_ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);
