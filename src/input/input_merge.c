/*
 * Input merge composer — the ONLY writer to the HID engine's back buffer.
 *
 * Each input source (XInput pad, HID keyboard) keeps its own contribution
 * state here; every update/clear recomposes the merged Pro2 state and commits
 * it. Because the engine swaps front/back buffers on commit, every commit
 * writes the FULL merged button + stick state (set_button_custom + both
 * sticks) so each buffer is self-consistent regardless of swap history.
 *
 * Merge semantics:
 *   - buttons: bitwise OR of all present sources;
 *   - sticks:  copied from the present source with the newest stick-event
 *     stamp (strict `>` comparison, so on a tie the source earlier in the
 *     input_source_t enum wins — INPUT_SRC_XINPUT is declared first, hence
 *     ties resolve to the XInput pad);
 *   - no source present: neutral (all buttons released, sticks centred).
 *
 * A source's stick-event stamp bumps only when its stick tuple changes (or
 * the source re-appears), so buttons-only reports never steal the sticks.
 */

#include "input/input_merge.h"
#include "controller/hid_controller.h"
#include "controller/hid_controller_pro2.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(jb_ctrl, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

typedef struct {
    input_contrib_t contrib; /* last contribution reported by this source */
    bool     present;        /* source participates in the merge */
    uint32_t stick_stamp;    /* newest stick-event stamp (0 = none yet) */
} merge_src_t;

static merge_src_t merge_srcs[INPUT_SRC_COUNT];
static uint32_t stick_stamp_ctr; /* monotonic stick-event counter */

/*
 * Compose the merged state into the back buffer and commit it.
 * Caller must hold the scheduler lock (k_sched_lock).
 */
static void merge_compose_locked(void)
{
    const controller_hid_ops_t *h = g_hid_controller.hid_ops;
    controller_hid_report_t *bb = (g_hid_controller.ops != NULL)
        ? g_hid_controller.ops->get_back_buffer(&g_hid_controller)
        : NULL;
    if (h == NULL || bb == NULL || bb->report == NULL) {
        return; /* HID controller not initialised yet */
    }

    uint8_t btn[3] = {0, 0, 0};
    uint16_t lx = PRO2_STICK_CENTER, ly = PRO2_STICK_CENTER;
    uint16_t rx = PRO2_STICK_CENTER, ry = PRO2_STICK_CENTER;
    uint32_t best_stamp = 0;

    for (int s = 0; s < (int)INPUT_SRC_COUNT; s++) {
        if (!merge_srcs[s].present) {
            continue;
        }
        btn[0] |= merge_srcs[s].contrib.btn[0];
        btn[1] |= merge_srcs[s].contrib.btn[1];
        btn[2] |= merge_srcs[s].contrib.btn[2];
        /* Strict `>`: on equal stamps the earlier-iterated source (XInput)
         * keeps the sticks — see the file header. */
        if (merge_srcs[s].stick_stamp > best_stamp) {
            best_stamp = merge_srcs[s].stick_stamp;
            lx = merge_srcs[s].contrib.lx;
            ly = merge_srcs[s].contrib.ly;
            rx = merge_srcs[s].contrib.rx;
            ry = merge_srcs[s].contrib.ry;
        }
    }

    if (h->set_button_custom != NULL) {
        h->set_button_custom(bb, btn, sizeof(btn));
    }
    if (h->set_left_stick != NULL) {
        h->set_left_stick(bb, lx, ly);
    }
    if (h->set_right_stick != NULL) {
        h->set_right_stick(bb, rx, ry);
    }

    if (g_hid_controller.ops->hid_commit != NULL) {
        g_hid_controller.ops->hid_commit(&g_hid_controller);
    }
}

void input_merge_update(input_source_t src, const input_contrib_t *contrib)
{
    if (src < 0 || src >= INPUT_SRC_COUNT || contrib == NULL) {
        return;
    }

    k_sched_lock();

    merge_src_t *ms = &merge_srcs[src];
    /* Stamp a stick event when the source (re)appears or its stick tuple
     * differs from its previous one; buttons-only reports must NOT steal
     * the sticks from the other source. */
    if (!ms->present ||
        ms->contrib.lx != contrib->lx || ms->contrib.ly != contrib->ly ||
        ms->contrib.rx != contrib->rx || ms->contrib.ry != contrib->ry) {
        ms->stick_stamp = ++stick_stamp_ctr;
    }
    ms->contrib = *contrib;
    ms->present = true;

    merge_compose_locked();

    k_sched_unlock();
}

void input_merge_clear(input_source_t src)
{
    if (src < 0 || src >= INPUT_SRC_COUNT) {
        return;
    }

    k_sched_lock();

    merge_srcs[src].contrib = (input_contrib_t){0};
    merge_srcs[src].present = false;
    merge_srcs[src].stick_stamp = 0;

    merge_compose_locked();

    k_sched_unlock();
}
