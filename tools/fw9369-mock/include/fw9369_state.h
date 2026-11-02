/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FW9369_STATE_H
#define FW9369_STATE_H

#include "fw9369_transport.h"
#include "fw9369_calibration.h"
#include <stdbool.h>

enum fw9369_phase {
    FW9369_PHASE_UNINITIALIZED = 0,
    FW9369_PHASE_1_WAKEUP,
    FW9369_PHASE_2_IDLE,
    FW9369_PHASE_3_FDT,
    FW9369_PHASE_4_IMAGE,
    FW9369_PHASE_5_ARM,
    FW9369_PHASE_COMPLETED,
    FW9369_PHASE_ABORTED
};

enum fw9369_lock_outcome {
    FW9369_LOCK_NOT_ACCESSED = 0,
    FW9369_LOCK_TRANSMITTED_CLEAN,
    FW9369_LOCK_TRANSMISSION_FAILED,
    FW9369_LOCK_INTERRUPTED_UNATTEMPTED,
    FW9369_LOCK_STATE_PHYSICALLY_UNVERIFIABLE
};

struct fw9369_pipeline_context {
    enum fw9369_phase phase;
    uint16_t          identified_chip_id;
    uint16_t          identified_chip_crc;
    uint8_t           mcu_status;

    struct fw9369_fdt_calibration fdt;
    struct fw9369_image_baseline  baseline;

    enum fw9369_lock_outcome      lock_outcome;
    bool                          armed;
    int                           last_error;
};

/**
 * Executes the complete 5-phase initialization and arming pipeline.
 */
int fw9369_pipeline_run(struct fw9369_transport *t, struct fw9369_pipeline_context *ctx);

/**
 * Abort cleanup handler: guarantees attempt to lock 0x9A and record lock outcome.
 */
void fw9369_pipeline_abort_cleanup(struct fw9369_transport *t, struct fw9369_pipeline_context *ctx);

#endif /* FW9369_STATE_H */
