#ifndef RH_MANAGER_CONFIG_STORE_H
#define RH_MANAGER_CONFIG_STORE_H

#include <stdint.h>
struct canopus_context_v1;

/* Serialized module/UI-owner calls only. bind reads active-config once after
 * validating get_data_path. A missing pointer selects the top-level directory.
 * Malformed/read/close failures preserve the validated context for candidate()
 * and commit(), but leave current_dir() NULL. Subsequent same-context binds
 * return the cached pointer error without I/O until commit repairs the store. */
int rh_manager_config_bind(const struct canopus_context_v1 *ctx);
/* Absolute directory ending in '/', or NULL while the active pointer is invalid
 * or before a validated binding. */
const char *rh_manager_config_current_dir(void);
/* Accept canonical active/<nonzero uint32 decimal>, with an optional final '/'.
 * No leading zero, extra segment, whitespace or alternate spelling is accepted.
 * On success out holds the absolute directory plus trailing '/', at most 255
 * bytes and NUL. On failure out is unchanged. */
int rh_manager_config_candidate(const char *relative_dir, char out[256]);
/* Complete-write + checked close + rename active-config.part -> active-config.
 * The on-disk record is canonical "active/<generation>\n". Only a successful
 * rename changes current_dir and clears any cached startup pointer error; no
 * mappings/background/calendar files are moved. A failed repair retains the
 * startup error and NULL current_dir.
 * Rename publication does not by itself promise power-loss durability. */
int rh_manager_config_commit(const char *relative_dir);

#endif
