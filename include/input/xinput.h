#ifndef INPUT_XINPUT_H
#define INPUT_XINPUT_H

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * XInput (Xbox 360 USB protocol) gamepad INPUT report — 20 bytes, type 0x00.
 * Layout per the well-documented XInput wire format; sticks are signed 16-bit
 * little-endian (+ up / + right), triggers are 0..255.
 */
typedef struct __attribute__((packed)) {
    uint8_t type;          /* 0x00 = input report */
    uint8_t size;          /* 0x14 (20) */
    uint8_t buttons1;      /* d-pad + start/back/L3/R3 */
    uint8_t buttons2;      /* shoulders/guide + A/B/X/Y */
    uint8_t left_trigger;  /* 0..255 */
    uint8_t right_trigger; /* 0..255 */
    int16_t lx;            /* left stick X  */
    int16_t ly;            /* left stick Y  */
    int16_t rx;            /* right stick X */
    int16_t ry;            /* right stick Y */
    uint8_t reserved[6];
} xinput_report_t;
static_assert(sizeof(xinput_report_t) == 20, "xinput report must be 20 bytes");

/* buttons1 bits */
#define XINPUT_DPAD_UP    0x01
#define XINPUT_DPAD_DOWN  0x02
#define XINPUT_DPAD_LEFT  0x04
#define XINPUT_DPAD_RIGHT 0x08
#define XINPUT_START      0x10
#define XINPUT_BACK       0x20
#define XINPUT_L3         0x40
#define XINPUT_R3         0x80

/* buttons2 bits */
#define XINPUT_LB         0x01
#define XINPUT_RB         0x02
#define XINPUT_GUIDE      0x04
#define XINPUT_A          0x10
#define XINPUT_B          0x20
#define XINPUT_X          0x40
#define XINPUT_Y          0x80

/* Analog trigger press threshold (0..255) for the digital ZL/ZR mapping. */
#define XINPUT_TRIGGER_THRESHOLD 64

/*
 * Map a decoded XInput report onto the Pro2 HID report (the report engine's
 * back buffer) and commit it. NULL-safe; a no-op until the HID controller has
 * been initialised.
 */
void xinput_map_report(const xinput_report_t *r);

/*
 * Reset the emulated controller to neutral (all buttons released, sticks
 * centred). Call on controller disconnect.
 */
void xinput_map_neutral(void);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_XINPUT_H */
