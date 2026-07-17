#ifndef INPUT_INPUT_MERGE_H
#define INPUT_INPUT_MERGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Input merge composer — merges per-source input contributions into the
 * single Pro2 HID state and is the ONLY writer to the HID engine's back
 * buffer. Merge semantics: buttons are bitwise OR-ed across present sources;
 * sticks are last-writer-wins per stick event (the present source with the
 * newest stick-change supplies both sticks; ties resolve to INPUT_SRC_XINPUT).
 */

typedef enum {
    INPUT_SRC_XINPUT = 0,
    INPUT_SRC_KBD,
    INPUT_SRC_COUNT
} input_source_t;

/* One source's contribution to the merged Pro2 state: the three Pro2 button
 * bytes (see btn_bits_pro2_t) plus both 12-bit stick axes (centre 0x800). */
typedef struct {
    uint8_t btn[3];
    uint16_t lx;
    uint16_t ly;
    uint16_t rx;
    uint16_t ry;
} input_contrib_t;

/*
 * Publish a source's latest contribution and commit the merged state.
 * A stick event (stick tuple differing from this source's previous tuple)
 * makes this source the stick owner. NULL-safe; a no-op until the HID
 * controller has been initialised.
 */
void input_merge_update(input_source_t src, const input_contrib_t *contrib);

/*
 * Mark a source absent (its contribution no longer participates in the merge)
 * and commit the recomposed state — neutral when no source remains. Call on
 * source disconnect.
 */
void input_merge_clear(input_source_t src);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_INPUT_MERGE_H */
