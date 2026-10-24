#ifndef RH_MANAGER_UI_H
#define RH_MANAGER_UI_H
#include "manager_data.h"
struct canopus_context_v1;
/* Called on the serialized UI owner, in the two staged publication turns. */
int rh_manager_ui_register(const struct canopus_context_v1 *);
int rh_manager_launcher_register(const struct canopus_context_v1 *);
/* Optional exact-platform adapters, set before the first page is opened. */
int rh_manager_ui_set_data_hooks(const struct rh_manager_data_hooks *);
/* Shared catalog used by the UI and the serialized Connect installer. */
int rh_manager_ui_get_data(const struct canopus_context_v1 *, struct rh_manager_data **);
/* Initial activation rollback only, before native pages can be published. */
void rh_manager_ui_reset_data(void);
/* Internal navigation entry for the receiver. */
int rh_manager_ui_show_receive(void);
#endif
