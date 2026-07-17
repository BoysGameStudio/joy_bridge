/*
 * ztest suite for the pure USB HID boot-protocol keyboard parser
 * (src/input/kbd_parse.c). native_sim only; the parser under test has zero
 * Zephyr dependencies and is compiled standalone into this app.
 *
 * Cases (per .omo/plans/keyboard-support.md Task 1):
 *  1. press -> held, absent-from-next-report -> released (slot diff)
 *  2. 6 simultaneous keys
 *  3. phantom/rollover (all slots 0x01) -> keys ignored, mods preserved
 *  4. usages 0x02/0x03 in any slot -> same filter
 *  5. len < 8 rejected
 *  6. modifiers tracked (bit0 LCtrl .. bit7 RGUI), unused by default map
 *  7. stick mapping (WASD / IJKL, cancel + diagonal)
 *  8. button mapping (full keymap table)
 *  9. full release -> neutral output
 */

#include <zephyr/ztest.h>

#include "input/kbd_hid.h"

/* HID usage IDs (boot keyboard page 0x07). */
#define HID_A 0x04u
#define HID_C 0x06u
#define HID_D 0x07u
#define HID_E 0x08u
#define HID_F 0x09u
#define HID_G 0x0Au
#define HID_H 0x0Bu
#define HID_I 0x0Cu
#define HID_J 0x0Du
#define HID_K 0x0Eu
#define HID_L 0x0Fu
#define HID_P 0x13u
#define HID_Q 0x14u
#define HID_S 0x16u
#define HID_V 0x19u
#define HID_W 0x1Au
#define HID_X 0x1Bu
#define HID_Z 0x1Du
#define HID_1 0x1Eu
#define HID_3 0x20u
#define HID_ENTER 0x28u
#define HID_BKSP 0x2Au
#define HID_ARR_RIGHT 0x4Fu
#define HID_ARR_LEFT 0x50u
#define HID_ARR_DOWN 0x51u
#define HID_ARR_UP 0x52u

/* Stick axis values per keymap spec. */
#define STICK_MIN 0x000u
#define STICK_CTR 0x800u
#define STICK_MAX 0xFFFu

/* Modifier bits. */
#define MOD_LCTRL 0x01u
#define MOD_LSHIFT 0x02u
#define MOD_LALT 0x04u
#define MOD_LGUI 0x08u
#define MOD_RCTRL 0x10u
#define MOD_RSHIFT 0x20u
#define MOD_RALT 0x40u
#define MOD_RGUI 0x80u

static void mk_report(uint8_t r[8], uint8_t mods, uint8_t k0, uint8_t k1,
                      uint8_t k2, uint8_t k3, uint8_t k4, uint8_t k5)
{
    r[0] = mods;
    r[1] = 0u; /* reserved */
    r[2] = k0;
    r[3] = k1;
    r[4] = k2;
    r[5] = k3;
    r[6] = k4;
    r[7] = k5;
}

static void assert_neutral(const kbd_output_t *out)
{
    zassert_equal(out->btn[0], 0u, "btn[0] not zero");
    zassert_equal(out->btn[1], 0u, "btn[1] not zero");
    zassert_equal(out->btn[2], 0u, "btn[2] not zero");
    zassert_equal(out->lx, STICK_CTR, "lx not centered");
    zassert_equal(out->ly, STICK_CTR, "ly not centered");
    zassert_equal(out->rx, STICK_CTR, "rx not centered");
    zassert_equal(out->ry, STICK_CTR, "ry not centered");
}

/* Feed a single-key report and assert the full button-byte expectation. */
static void assert_button_map(uint8_t usage, uint8_t byte, uint8_t mask)
{
    kbd_state_t st;
    kbd_output_t out;
    uint8_t r[8];

    kbd_state_init(&st);
    mk_report(r, 0u, usage, 0u, 0u, 0u, 0u, 0u);
    zassert_true(kbd_state_update(&st, r, sizeof(r)), "update rejected");
    zassert_true(kbd_key_held(&st, usage), "key not held");

    kbd_state_to_output(&st, &out);
    for (uint8_t b = 0u; b < 3u; b++) {
        zassert_equal(out.btn[b], (b == byte) ? mask : 0u,
                      "usage 0x%02x: btn[%u]=0x%02x, expected 0x%02x",
                      usage, b, out.btn[b], (b == byte) ? mask : 0u);
    }
    /* Button keys must not deflect sticks. */
    zassert_equal(out.lx, STICK_CTR, "lx deflected");
    zassert_equal(out.ly, STICK_CTR, "ly deflected");
    zassert_equal(out.rx, STICK_CTR, "rx deflected");
    zassert_equal(out.ry, STICK_CTR, "ry deflected");
}

/* Feed a single report (mods + up to 2 keys) into a fresh state and output. */
static void output_for_keys(kbd_output_t *out, uint8_t k0, uint8_t k1)
{
    kbd_state_t st;
    uint8_t r[8];

    kbd_state_init(&st);
    mk_report(r, 0u, k0, k1, 0u, 0u, 0u, 0u);
    zassert_true(kbd_state_update(&st, r, sizeof(r)), "update rejected");
    kbd_state_to_output(&st, out);
}

/* (1) press -> held, absent-from-next-report -> released (slot diff). */
ZTEST(kbd_parse, test_press_release_slot_diff)
{
    kbd_state_t st;
    kbd_output_t out;
    uint8_t r[8];

    zassert_equal(sizeof(kbd_boot_report_t), 8u, "boot report not 8 bytes");

    kbd_state_init(&st);
    zassert_false(st.primed, "primed before any report");
    zassert_false(kbd_key_held(&st, HID_W), "held before any report");

    mk_report(r, 0u, HID_W, 0u, 0u, 0u, 0u, 0u);
    zassert_true(kbd_state_update(&st, r, sizeof(r)), "press rejected");
    zassert_true(st.primed, "not primed after valid report");
    zassert_true(kbd_key_held(&st, HID_W), "W not held after press");
    zassert_equal(st.prev[0], 0u, "prev should be empty on first report");

    kbd_state_to_output(&st, &out);
    zassert_equal(out.ly, STICK_MAX, "W held: ly not deflected up");

    /* W absent from the next report -> released; prev captures the diff. */
    mk_report(r, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
    zassert_true(kbd_state_update(&st, r, sizeof(r)), "release rejected");
    zassert_false(kbd_key_held(&st, HID_W), "W still held after release");
    zassert_equal(st.prev[0], HID_W, "prev lost the released key");

    kbd_state_to_output(&st, &out);
    zassert_equal(out.ly, STICK_CTR, "W released: ly not re-centered");
}

/* (2) 6 simultaneous keys all register. */
ZTEST(kbd_parse, test_six_simultaneous_keys)
{
    kbd_state_t st;
    kbd_output_t out;
    uint8_t r[8];

    kbd_state_init(&st);
    mk_report(r, 0u, HID_W, HID_D, HID_I, HID_L, HID_Z, HID_ENTER);
    zassert_true(kbd_state_update(&st, r, sizeof(r)), "6-key report rejected");

    zassert_true(kbd_key_held(&st, HID_W), "W missing");
    zassert_true(kbd_key_held(&st, HID_D), "D missing");
    zassert_true(kbd_key_held(&st, HID_I), "I missing");
    zassert_true(kbd_key_held(&st, HID_L), "L missing");
    zassert_true(kbd_key_held(&st, HID_Z), "Z missing");
    zassert_true(kbd_key_held(&st, HID_ENTER), "Enter missing");
    zassert_false(kbd_key_held(&st, HID_A), "phantom A");

    kbd_state_to_output(&st, &out);
    zassert_equal(out.lx, STICK_MAX, "D: lx");
    zassert_equal(out.ly, STICK_MAX, "W: ly");
    zassert_equal(out.rx, STICK_MAX, "L: rx");
    zassert_equal(out.ry, STICK_MAX, "I: ry");
    zassert_equal(out.btn[0], 0x01u | 0x40u, "Z(B)+Enter(Plus)");
    zassert_equal(out.btn[1], 0u, "btn[1]");
    zassert_equal(out.btn[2], 0u, "btn[2]");
}

/* (3) phantom/rollover (all slots 0x01) -> keys ignored, mods preserved. */
ZTEST(kbd_parse, test_phantom_rollover_all_0x01)
{
    kbd_state_t st;
    uint8_t r[8];

    kbd_state_init(&st);
    mk_report(r, 0u, HID_W, 0u, 0u, 0u, 0u, 0u);
    zassert_true(kbd_state_update(&st, r, sizeof(r)), "press rejected");
    zassert_true(kbd_key_held(&st, HID_W), "W not held");

    /* Rollover report: every slot 0x01, with new modifier state. */
    mk_report(r, MOD_LSHIFT, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u);
    zassert_false(kbd_state_update(&st, r, sizeof(r)), "rollover accepted");
    zassert_true(kbd_key_held(&st, HID_W), "key slots not ignored on rollover");
    zassert_equal(st.mods, MOD_LSHIFT, "mods not applied on rollover");
    zassert_true(st.primed, "primed cleared by rollover");

    /* Recovery: a normal report parses again. */
    mk_report(r, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
    zassert_true(kbd_state_update(&st, r, sizeof(r)), "post-rollover rejected");
    zassert_false(kbd_key_held(&st, HID_W), "W stuck after rollover");
}

/* (4) usages 0x02/0x03 in ANY slot -> same filter; 0x00 slots are fine. */
ZTEST(kbd_parse, test_phantom_usages_0x02_0x03)
{
    kbd_state_t st;
    uint8_t r[8];

    /* 0x02 in one slot poisons the whole key array (Z must NOT register). */
    kbd_state_init(&st);
    mk_report(r, 0u, HID_W, 0u, 0u, 0u, 0u, 0u);
    zassert_true(kbd_state_update(&st, r, sizeof(r)), "press rejected");

    mk_report(r, 0u, 0x02u, HID_Z, 0u, 0u, 0u, 0u);
    zassert_false(kbd_state_update(&st, r, sizeof(r)), "0x02 accepted");
    zassert_true(kbd_key_held(&st, HID_W), "0x02: previous keys not kept");
    zassert_false(kbd_key_held(&st, HID_Z), "0x02: slot keys not ignored");

    /* 0x03 in a later slot -> same. */
    mk_report(r, 0u, 0u, 0u, 0u, 0x03u, HID_ENTER, 0u);
    zassert_false(kbd_state_update(&st, r, sizeof(r)), "0x03 accepted");
    zassert_false(kbd_key_held(&st, HID_ENTER), "0x03: slot keys not ignored");

    /* Fresh state + phantom-only report: not primed, mods still applied. */
    kbd_state_init(&st);
    mk_report(r, MOD_RGUI, 0u, 0u, 0u, 0u, 0u, 0x01u);
    zassert_false(kbd_state_update(&st, r, sizeof(r)), "0x01 tail accepted");
    zassert_false(st.primed, "primed by phantom-only report");
    zassert_equal(st.mods, MOD_RGUI, "mods not applied on phantom");

    /* 0x00 is the empty-slot marker, NOT an error usage. */
    kbd_state_init(&st);
    mk_report(r, 0u, HID_Q, 0u, 0u, 0u, 0u, 0u);
    zassert_true(kbd_state_update(&st, r, sizeof(r)), "0x00 slots rejected");
    zassert_true(kbd_key_held(&st, HID_Q), "Q not held");
}

/* (5) len < 8 rejected; state untouched. Longer buffers parse first 8. */
ZTEST(kbd_parse, test_short_report_rejected)
{
    kbd_state_t st;
    uint8_t r[16];

    kbd_state_init(&st);
    mk_report(r, MOD_LCTRL, HID_W, 0u, 0u, 0u, 0u, 0u);
    zassert_true(kbd_state_update(&st, r, 8u), "press rejected");

    for (size_t len = 0u; len < 8u; len++) {
        zassert_false(kbd_state_update(&st, r, len), "len %u accepted", len);
    }
    zassert_true(kbd_key_held(&st, HID_W), "state touched by short report");
    zassert_equal(st.mods, MOD_LCTRL, "mods touched by short report");

    zassert_false(kbd_state_update(&st, NULL, 8u), "NULL report accepted");

    /* MPS-sized buffers may exceed 8: first 8 bytes still parse. */
    mk_report(r, 0u, HID_E, 0u, 0u, 0u, 0u, 0u);
    r[8] = 0xFFu;
    zassert_true(kbd_state_update(&st, r, sizeof(r)), "len 16 rejected");
    zassert_true(kbd_key_held(&st, HID_E), "E not held from long report");
}

/* (6) modifiers tracked bit0 LCtrl .. bit7 RGUI; unused by default map. */
ZTEST(kbd_parse, test_modifiers_tracked)
{
    kbd_state_t st;
    kbd_output_t out;
    uint8_t r[8];
    static const uint8_t mod_values[] = {
        MOD_LCTRL, MOD_LSHIFT, MOD_LALT, MOD_LGUI,
        MOD_RCTRL, MOD_RSHIFT, MOD_RALT, MOD_RGUI, 0xFFu, 0x00u,
    };

    kbd_state_init(&st);
    for (size_t i = 0u; i < sizeof(mod_values); i++) {
        mk_report(r, mod_values[i], 0u, 0u, 0u, 0u, 0u, 0u);
        zassert_true(kbd_state_update(&st, r, sizeof(r)), "update rejected");
        zassert_equal(st.mods, mod_values[i], "mods 0x%02x not tracked",
                      mod_values[i]);
        kbd_state_to_output(&st, &out);
        assert_neutral(&out);
    }
}

/* (7) stick mapping: WASD -> left, IJKL -> right, cancel + diagonal. */
ZTEST(kbd_parse, test_stick_mapping)
{
    kbd_output_t out;

    output_for_keys(&out, HID_W, 0u);
    zassert_equal(out.ly, STICK_MAX, "W: ly");
    zassert_equal(out.lx, STICK_CTR, "W: lx");
    zassert_equal(out.rx, STICK_CTR, "W: rx");
    zassert_equal(out.ry, STICK_CTR, "W: ry");

    /* Diagonal deflects both axes. */
    output_for_keys(&out, HID_W, HID_D);
    zassert_equal(out.lx, STICK_MAX, "W+D: lx");
    zassert_equal(out.ly, STICK_MAX, "W+D: ly");

    /* Opposing keys on one axis cancel to center. */
    output_for_keys(&out, HID_W, HID_S);
    zassert_equal(out.ly, STICK_CTR, "W+S: ly not canceled");
    output_for_keys(&out, HID_A, HID_D);
    zassert_equal(out.lx, STICK_CTR, "A+D: lx not canceled");

    output_for_keys(&out, HID_S, 0u);
    zassert_equal(out.ly, STICK_MIN, "S: ly");
    output_for_keys(&out, HID_A, 0u);
    zassert_equal(out.lx, STICK_MIN, "A: lx");
    output_for_keys(&out, HID_D, 0u);
    zassert_equal(out.lx, STICK_MAX, "D: lx");

    /* IJKL -> right stick (independent of left). */
    output_for_keys(&out, HID_I, 0u);
    zassert_equal(out.ry, STICK_MAX, "I: ry");
    zassert_equal(out.ly, STICK_CTR, "I: ly touched");
    output_for_keys(&out, HID_K, 0u);
    zassert_equal(out.ry, STICK_MIN, "K: ry");
    output_for_keys(&out, HID_J, 0u);
    zassert_equal(out.rx, STICK_MIN, "J: rx");
    output_for_keys(&out, HID_L, 0u);
    zassert_equal(out.rx, STICK_MAX, "L: rx");
    output_for_keys(&out, HID_I, HID_K);
    zassert_equal(out.ry, STICK_CTR, "I+K: ry not canceled");
}

/* (8) full button keymap. */
ZTEST(kbd_parse, test_button_mapping)
{
    assert_button_map(HID_Z, 0u, 0x01u);     /* B      */
    assert_button_map(HID_X, 0u, 0x02u);     /* A      */
    assert_button_map(HID_C, 0u, 0x04u);     /* Y      */
    assert_button_map(HID_V, 0u, 0x08u);     /* X      */
    assert_button_map(HID_E, 0u, 0x10u);     /* R      */
    assert_button_map(HID_3, 0u, 0x20u);     /* ZR     */
    assert_button_map(HID_ENTER, 0u, 0x40u); /* Plus   */
    assert_button_map(HID_G, 0u, 0x80u);     /* RClick */

    assert_button_map(HID_ARR_DOWN, 1u, 0x01u);
    assert_button_map(HID_ARR_RIGHT, 1u, 0x02u);
    assert_button_map(HID_ARR_LEFT, 1u, 0x04u);
    assert_button_map(HID_ARR_UP, 1u, 0x08u);
    assert_button_map(HID_Q, 1u, 0x10u);     /* L      */
    assert_button_map(HID_1, 1u, 0x20u);     /* ZL     */
    assert_button_map(HID_BKSP, 1u, 0x40u);  /* Minus  */
    assert_button_map(HID_F, 1u, 0x80u);     /* LClick */

    assert_button_map(HID_H, 2u, 0x01u);     /* Home    */
    assert_button_map(HID_P, 2u, 0x02u);     /* Capture */

    /* Simultaneous buttons OR into the same byte. */
    kbd_state_t st;
    kbd_output_t out;
    uint8_t r[8];

    kbd_state_init(&st);
    mk_report(r, 0u, HID_Z, HID_X, HID_V, 0u, 0u, 0u);
    zassert_true(kbd_state_update(&st, r, sizeof(r)), "update rejected");
    kbd_state_to_output(&st, &out);
    zassert_equal(out.btn[0], 0x01u | 0x02u | 0x08u, "Z+X+V not OR-ed");
}

/* (9) full release -> all buttons zero, all sticks centered. */
ZTEST(kbd_parse, test_full_release_neutral)
{
    kbd_state_t st;
    kbd_output_t out;
    uint8_t r[8];

    kbd_state_init(&st);

    /* Fresh, never-reported state is already neutral. */
    kbd_state_to_output(&st, &out);
    assert_neutral(&out);

    mk_report(r, MOD_LCTRL, HID_W, HID_D, HID_Z, HID_ENTER, HID_I, HID_G);
    zassert_true(kbd_state_update(&st, r, sizeof(r)), "press rejected");
    kbd_state_to_output(&st, &out);
    zassert_not_equal(out.btn[0], 0u, "no buttons after press");
    zassert_not_equal(out.ly, STICK_CTR, "ly not deflected");

    mk_report(r, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
    zassert_true(kbd_state_update(&st, r, sizeof(r)), "release rejected");
    kbd_state_to_output(&st, &out);
    assert_neutral(&out);
}

ZTEST_SUITE(kbd_parse, NULL, NULL, NULL, NULL, NULL);
