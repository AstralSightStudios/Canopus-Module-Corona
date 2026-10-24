#ifndef CORONA_CONNECT_H
#define CORONA_CONNECT_H

#include "canopus_services.h"
#include "manager_state.h"

/* All methods, callbacks and hooks run on the serialized module/UI owner.
 * The caller owns the background timer: poll once per tick, including screen-off.
 * Hook errors are negative protocol codes (1..11), not platform errno values.
 * An installer must release its state in abort, including after step returns 1.
 * begin/step return 0 while working, 1 only after recoverable registry commit.
 * find_hash returns 1 installed / 0 absent / negative protocol code.
 * list writes at most *size bytes of protocol LIST payload and updates *size.
 * changed must only copy/post state, never recursively mutate this receiver. */
struct rh_connect;
struct rh_connect_hooks {
    void *cookie;
    int (*install_begin)(void *, const char *archive_path, uint32_t archive_bytes,
                         uint32_t generation);
    int (*install_step)(void *, const uint8_t hash[32]);
    void (*install_abort)(void *);
    /* 0: intent durably revoked; 1: registry already committed; negative
     * protocol code: cancellation failed, so do not report CANCELED. */
    int (*install_cancel)(void *, const uint8_t hash[32]);
    int (*find_hash)(void *, const uint8_t hash[32]);
    int (*list)(void *, uint32_t revision, uint32_t index, uint8_t *payload,
                uint32_t *size);
    void (*changed)(void *, const struct rh_transfer_ui_state *);
};

/* Returns 0 or a negative protocol code. create recovers durable progress;
 * start registers the listener. pause unregisters, flushes and checkpoints;
 * it may be called again after IO recovers. destroy calls pause before freeing.
 * A caller requiring a successful durable shutdown must check pause first.
 * The receiver owns only incoming/archive.zip and fixed-size state records.
 * CANCEL revokes the task and unlinks the ZIP; staging cleanup belongs to hooks.
 * No directory deletion or background timer capability is assumed here. */
int rh_connect_create(const struct canopus_context_v1 *,
                      const struct rh_connect_hooks *, struct rh_connect **);
int rh_connect_start(struct rh_connect *);
void rh_connect_poll(struct rh_connect *);
int rh_connect_pause(struct rh_connect *);
void rh_connect_destroy(struct rh_connect *);

#endif
