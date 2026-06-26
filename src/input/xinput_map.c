/*
 * XInput -> Pro2 HID report mapping.
 *
 * Translates a 20-byte XInput (Xbox 360 protocol) gamepad report into the Pro2
 * controller report and commits it to the HID engine's double buffer. Because
 * the engine swaps front/back buffers on commit, every frame writes the FULL
 * button + stick state (via set_button_custom + both sticks) so each buffer is
 * self-consistent regardless of swap history.
 *
 * Button map is physical-position (preserves muscle memory): the Xbox face
 * button in a given position maps to the Switch button in the same position
 * (Xbox A(bottom)->Switch B, B(right)->A, X(left)->Y, Y(top)->X).
 */

#include "input/xinput.h"
#include "controller/hid_controller.h"
#include "controller/hid_controller_pro2.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_xinput_map, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

/*
 * Stick Y inversion. Default OFF: XInput reports +Y up and the NS2 Pro2 report
 * also uses up = high value, so no axis is inverted — matching the verified
 * upstream (ESP32-BLE5-NSController-Emulator `main/src/transport/xinput_host.c`,
 * which notes "NS2 also uses up = high value, so no axis is inverted"). Tunable
 * if a particular pad reads reversed.
 */
#define XINPUT_INVERT_LY 0
#define XINPUT_INVERT_RY 0

/* XInput 16-bit signed axis -> Pro2 12-bit unsigned axis (centre 0x800). */
static inline uint16_t scale_axis(int16_t v, bool invert)
{
    uint16_t s = (uint16_t)(((int32_t)v + 32768) >> 4); /* 0..4095, centre 0x800 */
    if (s > 0x0FFF) {
        s = 0x0FFF;
    }
    return invert ? (uint16_t)(0x0FFF - s) : s;
}

void xinput_map_report(const xinput_report_t *r)
{
    if (r == NULL) {
        return;
    }

#ifdef CONFIG_JOY_BRIDGE_DIAG_LATENCY
    /* Stamp the moment this XInput report arrived (consumed by the report
     * thread to compute arrival->notify latency). */
    extern volatile uint32_t jb_diag_in_cyc;
    extern volatile uint32_t jb_diag_in_cnt;
    jb_diag_in_cyc = k_cycle_get_32();
    jb_diag_in_cnt++;
#endif

    const controller_hid_ops_t *h = g_hid_controller.hid_ops;
    controller_hid_report_t *bb = (g_hid_controller.ops != NULL)
        ? g_hid_controller.ops->get_back_buffer(&g_hid_controller)
        : NULL;
    if (h == NULL || bb == NULL || bb->report == NULL) {
        return; /* HID controller not initialised yet */
    }

    const uint8_t b1 = r->buttons1;
    const uint8_t b2 = r->buttons2;
    const bool lt = r->left_trigger  >= XINPUT_TRIGGER_THRESHOLD;
    const bool rt = r->right_trigger >= XINPUT_TRIGGER_THRESHOLD;

    /* Build the three Pro2 button bytes directly (see btn_bits_pro2_t). */
    uint8_t btn[3] = {0, 0, 0};

    /* byte 0: B,A,Y,X,R,ZR,Plus,RClick */
    if (b2 & XINPUT_A)     btn[0] |= 0x01; /* Xbox A (bottom) -> Switch B */
    if (b2 & XINPUT_B)     btn[0] |= 0x02; /* Xbox B (right)  -> Switch A */
    if (b2 & XINPUT_X)     btn[0] |= 0x04; /* Xbox X (left)   -> Switch Y */
    if (b2 & XINPUT_Y)     btn[0] |= 0x08; /* Xbox Y (top)    -> Switch X */
    if (b2 & XINPUT_RB)    btn[0] |= 0x10; /* R  */
    if (rt)                btn[0] |= 0x20; /* ZR */
    if (b1 & XINPUT_START) btn[0] |= 0x40; /* Plus   */
    if (b1 & XINPUT_R3)    btn[0] |= 0x80; /* RClick */

    /* byte 1: Down,Right,Left,Up,L,ZL,Minus,LClick */
    if (b1 & XINPUT_DPAD_DOWN)  btn[1] |= 0x01;
    if (b1 & XINPUT_DPAD_RIGHT) btn[1] |= 0x02;
    if (b1 & XINPUT_DPAD_LEFT)  btn[1] |= 0x04;
    if (b1 & XINPUT_DPAD_UP)    btn[1] |= 0x08;
    if (b2 & XINPUT_LB)    btn[1] |= 0x10; /* L  */
    if (lt)                btn[1] |= 0x20; /* ZL */
    if (b1 & XINPUT_BACK)  btn[1] |= 0x40; /* Minus  */
    if (b1 & XINPUT_L3)    btn[1] |= 0x80; /* LClick */

    /* byte 2: Home,Capture,GR,GL,C (only Home mapped) */
    if (b2 & XINPUT_GUIDE) btn[2] |= 0x01; /* Home */

    if (h->set_button_custom != NULL) {
        h->set_button_custom(bb, btn, sizeof(btn));
    }
    if (h->set_left_stick != NULL) {
        h->set_left_stick(bb, scale_axis(r->lx, false), scale_axis(r->ly, XINPUT_INVERT_LY));
    }
    if (h->set_right_stick != NULL) {
        h->set_right_stick(bb, scale_axis(r->rx, false), scale_axis(r->ry, XINPUT_INVERT_RY));
    }

    if (g_hid_controller.ops->hid_commit != NULL) {
        g_hid_controller.ops->hid_commit(&g_hid_controller);
    }
}

void xinput_map_neutral(void)
{
    const controller_hid_ops_t *h = g_hid_controller.hid_ops;
    controller_hid_report_t *bb = (g_hid_controller.ops != NULL)
        ? g_hid_controller.ops->get_back_buffer(&g_hid_controller)
        : NULL;
    if (h == NULL || bb == NULL || bb->report == NULL) {
        return;
    }

    uint8_t btn[3] = {0, 0, 0};
    if (h->set_button_custom != NULL) {
        h->set_button_custom(bb, btn, sizeof(btn));
    }
    if (h->set_left_stick != NULL) {
        h->set_left_stick(bb, PRO2_STICK_CENTER, PRO2_STICK_CENTER);
    }
    if (h->set_right_stick != NULL) {
        h->set_right_stick(bb, PRO2_STICK_CENTER, PRO2_STICK_CENTER);
    }
    if (g_hid_controller.ops->hid_commit != NULL) {
        g_hid_controller.ops->hid_commit(&g_hid_controller);
    }
}
