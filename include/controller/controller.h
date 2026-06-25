#ifndef _CONTROLLER_CONTROLLER_H_
#define _CONTROLLER_CONTROLLER_H_

#include <stdint.h>

#define ESP_BD_ADDR_LEN          6     /* kept for upstream call-site parity */

#define LTK_KEY_SIZE             16
#define MANUFACTURER_DATA_LEN    26

typedef enum {
    CONTROLLER_TYPE_PRO2,
    CONTROLLER_TYPE_JOYCON,
} controller_type_t;

typedef struct {
    uint8_t addr[ESP_BD_ADDR_LEN];        /* public identity, big-endian display */
    uint8_t addr_re[ESP_BD_ADDR_LEN];     /* reversed (little-endian) */
    uint8_t ltk[LTK_KEY_SIZE];
    uint8_t ltk_re[LTK_KEY_SIZE];
    uint8_t ltk_key_b1[LTK_KEY_SIZE];
    uint8_t manufacturer_data[MANUFACTURER_DATA_LEN];
    controller_type_t type;
} controller_firmware_t;

extern controller_firmware_t g_controller_firmware;

/* Read controller type from storage (default PRO2). */
void controller_type_init(void);

/* Generic controller firmware init, dispatches to pro2/joycon. Returns 0 on
 * success (pairing info found) or negative if not yet paired. */
int controller_init(void);

/* Persist / erase pairing info, dispatching to pro2/joycon. */
int controller_pairing_info_save(void);
int controller_pairing_info_erase(void);

#endif /* _CONTROLLER_CONTROLLER_H_ */
