#ifndef HID_CONTROLLER_PRO2_H
#define HID_CONTROLLER_PRO2_H

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "controller/hid_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PRO2_STICK_CENTER 0x800

/* pro2 button format
 * https://github.com/ndeadly/switch2_controller_research/blob/master/hid_reports.md#button-format-3
 */
typedef struct {
    /* Buttons Byte 0 */
    uint8_t B        : 1; /* 0x01 */
    uint8_t A        : 1; /* 0x02 */
    uint8_t Y        : 1; /* 0x04 */
    uint8_t X        : 1; /* 0x08 */
    uint8_t R        : 1; /* 0x10 */
    uint8_t ZR       : 1; /* 0x20 */
    uint8_t Plus     : 1; /* 0x40 */
    uint8_t RClick   : 1; /* 0x80 */

    /* Buttons Byte 1 */
    uint8_t Down     : 1; /* 0x01 */
    uint8_t Right    : 1; /* 0x02 */
    uint8_t Left     : 1; /* 0x04 */
    uint8_t Up       : 1; /* 0x08 */
    uint8_t L        : 1; /* 0x10 */
    uint8_t ZL       : 1; /* 0x20 */
    uint8_t Minus    : 1; /* 0x40 */
    uint8_t LClick   : 1; /* 0x80 */

    /* Buttons Byte 2 */
    uint8_t Home     : 1; /* 0x01 */
    uint8_t Capture  : 1; /* 0x02 */
    uint8_t GR       : 1; /* 0x04 */
    uint8_t GL       : 1; /* 0x08 */
    uint8_t C        : 1; /* 0x10 */
    uint8_t reserved0: 3; /* 0x20-0x80 */
} btn_bits_pro2_t;

typedef enum {
    /* Byte 0 */
    B, A, Y, X, R, ZR, Plus, RClick,
    /* Byte 1 */
    Down, Right, Left, Up, L, ZL, Minus, LClick,
    /* Byte 2 */
    Home, Capture, GR, GL, C,
} btns_pro2;

typedef struct __attribute__((packed)) {
    uint8_t counter;            /* 0x00 */
    uint8_t power_info;         /* 0x01 */
    btn_bits_pro2_t buttons;    /* 0x02 */
    uint8_t left_stick[3];      /* 0x05 */
    uint8_t right_stick[3];     /* 0x08 */
    uint8_t unknown_0x0b;       /* 0x0B */
    uint8_t unknown_0x0c;       /* 0x0C */
    uint8_t headset_flag;       /* 0x0D */
    uint8_t motion_data_len;    /* 0x0E */
    uint8_t motion_data[0x28];  /* 0x0F */
    uint8_t reserved[8];        /* 0x37 */
} hid_report_pro2_t;
static_assert(sizeof(hid_report_pro2_t) == 63, "pro2 report must be 63 bytes");

extern controller_hid_ops_t controller_pro2_ops;

#define PRO2_FIRMWARE_INFO_SIZE 12
extern const uint8_t pro2_firmware_info[12];

#ifdef __cplusplus
}
#endif

#endif /* HID_CONTROLLER_PRO2_H */
