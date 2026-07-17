/*
 * USB HID boot-protocol keyboard report parser + default keymap (pure C, no
 * Zephyr dependencies — see include/input/kbd_hid.h for the report/state
 * semantics).
 *
 * The keymap is a single static const table; each entry is either a button
 * bit in one of the three Pro2 button bytes or a stick-axis deflection.
 * Stick axes use 0x800 as center, 0xFFF as full up/right and 0x000 as full
 * down/left (NS2 "up = high value", matching src/input/xinput_map.c).
 * Opposing direction keys held on one axis cancel to center.
 */

#include "input/kbd_hid.h"

#define KBD_REPORT_MODS 0u
#define KBD_REPORT_KEYS 2u

#define KBD_STICK_CENTER 0x800u
#define KBD_STICK_HIGH 0xFFFu
#define KBD_STICK_LOW 0x000u

#define KBD_AXIS_LX 0u
#define KBD_AXIS_LY 1u
#define KBD_AXIS_RX 2u
#define KBD_AXIS_RY 3u
#define KBD_NUM_AXES 4u

#define KBD_KIND_BTN 0u
#define KBD_KIND_STICK 1u

/* HID usages: error/phantom band is 0x01..0x03 (0x00 = empty slot). */
#define KBD_USAGE_ERROR_MIN 0x01u
#define KBD_USAGE_ERROR_MAX 0x03u

struct kbd_keymap_entry {
    uint8_t usage; /* HID usage ID (keyboard page 0x07) */
    uint8_t kind;  /* KBD_KIND_* */
    uint8_t dst;   /* BTN: btn[] byte index; STICK: KBD_AXIS_* */
    uint8_t arg;   /* BTN: bit mask; STICK: 1 = deflect high, 0 = deflect low */
};

static const struct kbd_keymap_entry kbd_keymap[] = {
    /* WASD -> left stick */
    {0x1Au, KBD_KIND_STICK, KBD_AXIS_LY, 1u}, /* W -> ly up    */
    {0x04u, KBD_KIND_STICK, KBD_AXIS_LX, 0u}, /* A -> lx left  */
    {0x16u, KBD_KIND_STICK, KBD_AXIS_LY, 0u}, /* S -> ly down  */
    {0x07u, KBD_KIND_STICK, KBD_AXIS_LX, 1u}, /* D -> lx right */
    /* IJKL -> right stick */
    {0x0Cu, KBD_KIND_STICK, KBD_AXIS_RY, 1u}, /* I -> ry up    */
    {0x0Du, KBD_KIND_STICK, KBD_AXIS_RX, 0u}, /* J -> rx left  */
    {0x0Eu, KBD_KIND_STICK, KBD_AXIS_RY, 0u}, /* K -> ry down  */
    {0x0Fu, KBD_KIND_STICK, KBD_AXIS_RX, 1u}, /* L -> rx right */
    /* Arrow keys -> dpad */
    {0x51u, KBD_KIND_BTN, 1u, 0x01u}, /* Down  */
    {0x4Fu, KBD_KIND_BTN, 1u, 0x02u}, /* Right */
    {0x50u, KBD_KIND_BTN, 1u, 0x04u}, /* Left  */
    {0x52u, KBD_KIND_BTN, 1u, 0x08u}, /* Up    */
    /* Face buttons, shoulders, meta */
    {0x1Du, KBD_KIND_BTN, 0u, 0x01u}, /* Z -> B        */
    {0x1Bu, KBD_KIND_BTN, 0u, 0x02u}, /* X -> A        */
    {0x06u, KBD_KIND_BTN, 0u, 0x04u}, /* C -> Y        */
    {0x19u, KBD_KIND_BTN, 0u, 0x08u}, /* V -> X        */
    {0x08u, KBD_KIND_BTN, 0u, 0x10u}, /* E -> R        */
    {0x20u, KBD_KIND_BTN, 0u, 0x20u}, /* 3 -> ZR       */
    {0x28u, KBD_KIND_BTN, 0u, 0x40u}, /* Enter -> Plus */
    {0x0Au, KBD_KIND_BTN, 0u, 0x80u}, /* G -> RClick   */
    {0x14u, KBD_KIND_BTN, 1u, 0x10u}, /* Q -> L        */
    {0x1Eu, KBD_KIND_BTN, 1u, 0x20u}, /* 1 -> ZL       */
    {0x2Au, KBD_KIND_BTN, 1u, 0x40u}, /* Backspace -> Minus  */
    {0x09u, KBD_KIND_BTN, 1u, 0x80u}, /* F -> LClick         */
    {0x0Bu, KBD_KIND_BTN, 2u, 0x01u}, /* H -> Home           */
    {0x13u, KBD_KIND_BTN, 2u, 0x02u}, /* P -> Capture        */
};

#define KBD_KEYMAP_LEN (sizeof(kbd_keymap) / sizeof(kbd_keymap[0]))

void kbd_state_init(kbd_state_t *st)
{
    if (st == NULL) {
        return;
    }
    st->mods = 0u;
    for (size_t i = 0u; i < KBD_NUM_KEY_SLOTS; i++) {
        st->held[i] = 0u;
        st->prev[i] = 0u;
    }
    st->primed = false;
}

static bool report_has_error_usage(const uint8_t *report)
{
    for (size_t i = 0u; i < KBD_NUM_KEY_SLOTS; i++) {
        const uint8_t usage = report[KBD_REPORT_KEYS + i];
        if (usage >= KBD_USAGE_ERROR_MIN && usage <= KBD_USAGE_ERROR_MAX) {
            return true;
        }
    }
    return false;
}

bool kbd_state_update(kbd_state_t *st, const uint8_t *report, size_t len)
{
    if (st == NULL || report == NULL || len < KBD_BOOT_REPORT_LEN) {
        return false;
    }

    st->mods = report[KBD_REPORT_MODS];

    /* Phantom/rollover: the whole key array is invalid this report; keep the
     * last known held/prev state (mods above still apply). */
    if (report_has_error_usage(report)) {
        return false;
    }

    for (size_t i = 0u; i < KBD_NUM_KEY_SLOTS; i++) {
        st->prev[i] = st->held[i];
        st->held[i] = report[KBD_REPORT_KEYS + i];
    }
    st->primed = true;
    return true;
}

bool kbd_key_held(const kbd_state_t *st, uint8_t usage)
{
    if (st == NULL || usage == 0u) {
        return false;
    }
    for (size_t i = 0u; i < KBD_NUM_KEY_SLOTS; i++) {
        if (st->held[i] == usage) {
            return true;
        }
    }
    return false;
}

static uint16_t resolve_axis(uint8_t high_count, uint8_t low_count)
{
    if (high_count != 0u && low_count == 0u) {
        return KBD_STICK_HIGH;
    }
    if (low_count != 0u && high_count == 0u) {
        return KBD_STICK_LOW;
    }
    return KBD_STICK_CENTER; /* idle or canceled by opposing keys */
}

void kbd_state_to_output(const kbd_state_t *st, kbd_output_t *out)
{
    uint8_t high[KBD_NUM_AXES] = {0u, 0u, 0u, 0u};
    uint8_t low[KBD_NUM_AXES] = {0u, 0u, 0u, 0u};

    if (out == NULL) {
        return;
    }
    out->btn[0] = 0u;
    out->btn[1] = 0u;
    out->btn[2] = 0u;

    if (st != NULL) {
        for (size_t slot = 0u; slot < KBD_NUM_KEY_SLOTS; slot++) {
            const uint8_t usage = st->held[slot];
            if (usage == 0u) {
                continue;
            }
            for (size_t m = 0u; m < KBD_KEYMAP_LEN; m++) {
                if (kbd_keymap[m].usage != usage) {
                    continue;
                }
                if (kbd_keymap[m].kind == KBD_KIND_BTN) {
                    out->btn[kbd_keymap[m].dst] |= kbd_keymap[m].arg;
                } else if (kbd_keymap[m].arg != 0u) {
                    high[kbd_keymap[m].dst]++;
                } else {
                    low[kbd_keymap[m].dst]++;
                }
                break;
            }
        }
    }

    out->lx = resolve_axis(high[KBD_AXIS_LX], low[KBD_AXIS_LX]);
    out->ly = resolve_axis(high[KBD_AXIS_LY], low[KBD_AXIS_LY]);
    out->rx = resolve_axis(high[KBD_AXIS_RX], low[KBD_AXIS_RX]);
    out->ry = resolve_axis(high[KBD_AXIS_RY], low[KBD_AXIS_RY]);
}
