#include "host_protocol.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "display.h"
#include "rgb.h"

LOG_MODULE_REGISTER(host_protocol, LOG_LEVEL_INF);

#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 1
#define FW_VERSION_PATCH 0

static uint8_t response[HOST_REPORT_SIZE];
static struct host_status_snapshot current_status;
static struct rgb_host_config current_rgb;
static struct k_mutex protocol_lock;

static void response_begin(uint8_t cmd, uint8_t seq, uint8_t status)
{
	memset(response, 0, sizeof(response));
	response[0] = HOST_REPORT_ID;
	response[1] = cmd;
	response[2] = seq;
	response[3] = status;
	response[4] = 0;
}

static void response_payload(const uint8_t *payload, uint8_t len)
{
	len = MIN(len, (uint8_t)(HOST_REPORT_SIZE - 5));
	if (payload != NULL && len > 0) {
		memcpy(&response[5], payload, len);
	}
	response[4] = len;
}

static void handle_ping(uint8_t seq)
{
	static const uint8_t pong[] = { 'P', 'O', 'N', 'G' };

	response_begin(HOST_CMD_PING, seq, HOST_STATUS_OK);
	response_payload(pong, sizeof(pong));
}

static void handle_fw_info(uint8_t seq)
{
	uint8_t payload[58] = { 0 };
	size_t off = 0;
	const char board[] = "ai_ble_keyboard";
	const char build[] = __DATE__ " " __TIME__;

	payload[off++] = HOST_PROTOCOL_VERSION_MAJOR;
	payload[off++] = HOST_PROTOCOL_VERSION_MINOR;
	payload[off++] = FW_VERSION_MAJOR;
	payload[off++] = FW_VERSION_MINOR;
	payload[off++] = FW_VERSION_PATCH;
	payload[off++] = HOST_REPORT_ID;
	payload[off++] = HOST_REPORT_SIZE;
	payload[off++] = 0;

	memcpy(&payload[off], board, MIN(sizeof(board), sizeof(payload) - off));
	off += 24;
	memcpy(&payload[off], build, MIN(sizeof(build), sizeof(payload) - off));

	response_begin(HOST_CMD_GET_FW_INFO, seq, HOST_STATUS_OK);
	response_payload(payload, sizeof(payload));
}

static void handle_status(uint8_t seq)
{
	uint8_t payload[16] = { 0 };

	payload[0] = (uint8_t)current_status.mode;
	payload[1] = current_status.connected ? 1 : 0;
	payload[2] = current_status.numlock ? 1 : 0;
	payload[3] = current_status.idle ? 1 : 0;
	payload[4] = current_status.battery_percent < 0 ?
		     0xff : (uint8_t)current_status.battery_percent;
	sys_put_le16(current_status.battery_mv < 0 ? 0 :
		     (uint16_t)current_status.battery_mv, &payload[5]);
	payload[7] = (uint8_t)current_status.charge_state;

	response_begin(HOST_CMD_GET_STATUS, seq, HOST_STATUS_OK);
	response_payload(payload, sizeof(payload));
}

static void handle_sync_time(uint8_t seq, const uint8_t *payload, uint8_t len)
{
	uint32_t unix_time;
	int16_t tz_offset_min;
	uint8_t flags;

	if (len < 7) {
		response_begin(HOST_CMD_SYNC_TIME, seq, HOST_STATUS_ERR_BAD_LEN);
		return;
	}

	unix_time = sys_get_le32(&payload[0]);
	tz_offset_min = (int16_t)sys_get_le16(&payload[4]);
	flags = payload[6];
	display_set_host_time(unix_time, tz_offset_min, flags);

	response_begin(HOST_CMD_SYNC_TIME, seq, HOST_STATUS_OK);
}

static void handle_set_rgb(uint8_t seq, const uint8_t *payload, uint8_t len)
{
	struct rgb_host_config config;

	if (len < 6) {
		response_begin(HOST_CMD_SET_RGB, seq, HOST_STATUS_ERR_BAD_LEN);
		return;
	}

	config.enabled = payload[0] != 0;
	config.r = payload[1];
	config.g = payload[2];
	config.b = payload[3];
	config.brightness = payload[4];
	config.idle_brightness = payload[5];
	rgb_set_host_config(&config);
	current_rgb = config;

	response_begin(HOST_CMD_SET_RGB, seq, HOST_STATUS_OK);
}

static void handle_get_rgb(uint8_t seq)
{
	uint8_t payload[8] = { 0 };

	rgb_get_host_config(&current_rgb);
	payload[0] = current_rgb.enabled ? 1 : 0;
	payload[1] = current_rgb.r;
	payload[2] = current_rgb.g;
	payload[3] = current_rgb.b;
	payload[4] = current_rgb.brightness;
	payload[5] = current_rgb.idle_brightness;

	response_begin(HOST_CMD_GET_RGB, seq, HOST_STATUS_OK);
	response_payload(payload, sizeof(payload));
}

void host_protocol_init(void)
{
	k_mutex_init(&protocol_lock);
	rgb_get_host_config(&current_rgb);
	response_begin(HOST_CMD_PING, 0, HOST_STATUS_OK);
}

void host_protocol_update_status(const struct host_status_snapshot *status)
{
	if (status == NULL) {
		return;
	}

	k_mutex_lock(&protocol_lock, K_FOREVER);
	current_status = *status;
	k_mutex_unlock(&protocol_lock);
}

int host_protocol_set_report(const uint8_t *data, uint16_t len)
{
	uint8_t cmd;
	uint8_t seq;
	uint8_t payload_len;
	const uint8_t *payload;

	if (data == NULL || len < 4 || data[0] != HOST_REPORT_ID) {
		return -EINVAL;
	}

	cmd = data[1];
	seq = data[2];
	payload_len = data[3];
	payload = &data[4];
	if (payload_len > len - 4) {
		return -EINVAL;
	}

	k_mutex_lock(&protocol_lock, K_FOREVER);
	switch (cmd) {
	case HOST_CMD_PING:
		handle_ping(seq);
		break;
	case HOST_CMD_GET_FW_INFO:
		handle_fw_info(seq);
		break;
	case HOST_CMD_GET_STATUS:
		handle_status(seq);
		break;
	case HOST_CMD_SYNC_TIME:
		handle_sync_time(seq, payload, payload_len);
		break;
	case HOST_CMD_SET_RGB:
		handle_set_rgb(seq, payload, payload_len);
		break;
	case HOST_CMD_GET_RGB:
		handle_get_rgb(seq);
		break;
	case HOST_CMD_SET_MODE:
		response_begin(cmd, seq, HOST_STATUS_ERR_UNSUPPORTED);
		break;
	default:
		response_begin(cmd, seq, HOST_STATUS_ERR_BAD_CMD);
		break;
	}
	k_mutex_unlock(&protocol_lock);

	return 0;
}

int host_protocol_get_report(uint8_t *data, uint16_t len)
{
	if (data == NULL || len < HOST_REPORT_SIZE) {
		return -EINVAL;
	}

	k_mutex_lock(&protocol_lock, K_FOREVER);
	memcpy(data, response, HOST_REPORT_SIZE);
	k_mutex_unlock(&protocol_lock);
	return HOST_REPORT_SIZE;
}
