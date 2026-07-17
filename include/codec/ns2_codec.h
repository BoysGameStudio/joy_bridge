#ifndef _NS2_CODEC_H_
#define _NS2_CODEC_H_

/**
 * @brief NS2 controller BLE application protocol (command + flash-sim codec).
 * @defgroup ns2_codec NS2 Codec
 * @{
 */

#include <stdint.h>
#include <stddef.h>

#include "uthash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Special data feature lengths. */
#define PRO2_DATA_EMPTY_LEN 0x0e /* notify 0e/1e 0x00 len 14 */
#define NS2_DATA_EMPTY_LEN  0x21 /* write 0x0016 0x00 len 33 */

/** @brief GATT response container. */
typedef struct {
    uint8_t  cmd;
    uint8_t  subcmd;
    uint8_t *rsp_data;
    uint16_t rsp_len;
} pro2_gatt_rsp_t;

/** @brief Command handler; returns the response data length. data_out is a
 * CMD_PROCESS_MAX_RSP_DATA_LEN (0x78) buffer: the handler MUST bound its own
 * writes, since the post-hoc length check in cmd_process() can only run after
 * the handler has already written. */
typedef uint8_t (*cmd_handler)(const uint8_t subcmd, const uint16_t payload_len,
                               const uint8_t *data_in, uint8_t *data_out);

/** @brief Command handler hashmap entry. */
typedef struct {
    uint8_t     cmd;
    cmd_handler handler;
    UT_hash_handle hh;
} g_cmd_handler_entry_t;

extern g_cmd_handler_entry_t *g_cmd_handlers;

int         cmd_handler_register(uint8_t cmd, cmd_handler handler);
cmd_handler cmd_handler_find(uint8_t cmd);
int         cmd_system_init(void);

/**
 * @brief Process a received command payload.
 * @return 0 on success, negative errno on failure.
 */
int cmd_process(pro2_gatt_rsp_t *rsp, uint8_t *data_in, uint16_t payload_len);

/* Feature flags. */
#define FEATURE_0C_01_BUTTON_STATE  (0x01U)
#define FEATURE_0C_02_ANALOG_STICKS (0x02U)
#define FEATURE_0C_04_IMU           (0x04U)
#define FEATURE_0C_08_UNUSED        (0x08U)
#define FEATURE_0C_10_MOUSE_DATA    (0x10U) /* JoyCon only */
#define FEATURE_0C_20_RUMBLE        (0x20U)
#define FEATURE_0C_40_UNUSED        (0x40U)
#define FEATURE_0C_80_MAGNETOMETER  (0x80U)

/* Flash-memory simulation. */
typedef struct {
    uint32_t       start_addr;
    size_t         block_len;
    const uint8_t *data;
} mem_sim_t;

void set_controller_specific(uint16_t product_id,
                             const uint8_t *serial, size_t serial_len,
                             const uint8_t version[3],
                             const uint8_t body_color[3],
                             const uint8_t buttons_color[3],
                             const uint8_t highlight_color[3],
                             const uint8_t grip_color[3]);

int read_memory(uint32_t addr, size_t read_len, uint8_t *out_buffer);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* _NS2_CODEC_H_ */
