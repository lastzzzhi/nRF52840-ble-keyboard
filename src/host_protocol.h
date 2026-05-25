#ifndef HOST_PROTOCOL_H
#define HOST_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"
#include "power.h"

#define HOST_REPORT_ID 3
#define HOST_REPORT_SIZE 64
#define HOST_PROTOCOL_VERSION_MAJOR 1
#define HOST_PROTOCOL_VERSION_MINOR 0

enum host_cmd {
	HOST_CMD_GET_FW_INFO = 0x01,
	HOST_CMD_GET_STATUS = 0x02,
	HOST_CMD_SYNC_TIME = 0x03,
	HOST_CMD_SET_RGB = 0x04,
	HOST_CMD_GET_RGB = 0x05,
	HOST_CMD_SET_MODE = 0x06,
	HOST_CMD_PING = 0x7f,
};

enum host_status {
	HOST_STATUS_OK = 0x00,
	HOST_STATUS_ERR_BAD_CMD = 0x01,
	HOST_STATUS_ERR_BAD_LEN = 0x02,
	HOST_STATUS_ERR_UNSUPPORTED = 0x03,
};

struct host_status_snapshot {
	enum app_mode mode;
	bool connected;
	bool numlock;
	bool idle;
	int battery_percent;
	int battery_mv;
	enum power_charge_state charge_state;
};

void host_protocol_init(void);
void host_protocol_update_status(const struct host_status_snapshot *status);
int host_protocol_set_report(const uint8_t *data, uint16_t len);
int host_protocol_get_report(uint8_t *data, uint16_t len);

#endif
