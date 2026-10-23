#ifndef RH_MANAGER_STATE_H
#define RH_MANAGER_STATE_H

#include <stdint.h>

/* All calls and notifications belong to the serialized module/UI owner.
 * Snapshots are copied values; neither files nor protocol requests are used. */
enum rh_manager_module_state {
    RH_MGR_STATE_RUNNING = 0,
    RH_MGR_STATE_CONFIG_ERROR,
    RH_MGR_STATE_UNRESPONSIVE,
    RH_MGR_STATE_UNAVAILABLE
};
struct rh_module_status_snapshot {
    uint8_t state;
    uint8_t refresh_pending;
    uint16_t active_rules;
    int32_t error_code;
    int32_t last_font_result;
    uint32_t last_changed;
    uint32_t reload_generation;
    int32_t reload_result; /* 1 = pending, 0 = complete, negative = failed. */
};

/* Mirrors the Connect protocol phase values, without implementing transport. */
enum rh_transfer_phase {
    RH_XFER_PHASE_IDLE = 0,
    RH_XFER_PHASE_RECEIVING = 1,
    RH_XFER_PHASE_VERIFYING = 2,
    RH_XFER_PHASE_INSTALLING = 3,
    RH_XFER_PHASE_INSTALLED = 4,
    RH_XFER_PHASE_ERROR = 5,
    RH_XFER_PHASE_CANCELED = 6
};
struct rh_transfer_ui_state {
    uint8_t phase;
    uint8_t detail;
    uint8_t remote_connected;
    uint8_t percent; /* Derived from written_offset, never CPC2 acceptance. */
    uint32_t written_offset;
    uint32_t durable_offset;
    uint32_t archive_bytes;
};
#define RH_MANAGER_CHANGED_STATUS 1u
#define RH_MANAGER_CHANGED_TRANSFER 2u
/* Notification only: copy state now and post rendering through ctx->ui->post.
 * A listener must not initiate a reload or mutate the state recursively. */
typedef void (*rh_manager_state_listener)(void *cookie, uint32_t changes);
void rh_module_get_status_snapshot(struct rh_module_status_snapshot *out);
void rh_manager_get_transfer_state(struct rh_transfer_ui_state *out);
int rh_manager_set_transfer_state(const struct rh_transfer_ui_state *state);
int rh_manager_subscribe(rh_manager_state_listener listener, void *cookie);
void rh_manager_unsubscribe(rh_manager_state_listener listener, void *cookie);
/* Config generation must already be prepared. Nonzero generation identifies
 * this transaction. BUSY rejects without replacing the active transaction.
 * Accepted requests return 0; completion is observed in the status snapshot. */
int rh_module_request_reload(uint32_t generation);
/* Native immutable configuration trio below active/<generation>. */
int rh_module_request_reload_from(uint32_t generation, const char *relative_config_dir);

#endif
