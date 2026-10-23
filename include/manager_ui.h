#ifndef RH_MANAGER_UI_H
#define RH_MANAGER_UI_H
#include "manager_data.h"
struct canopus_context_v1;
/* Called on the serialized UI owner, in the two staged publication turns. */
int rh_manager_ui_register(const struct canopus_context_v1 *);
int rh_manager_launcher_register(const struct canopus_context_v1 *);
/* Optional exact-platform adapters, set before the first page is opened. */
int rh_manager_ui_set_data_hooks(const struct rh_manager_data_hooks *);
/* Internal navigation entry for the future receiver, no transport is started. */
int rh_manager_ui_show_receive(void);
#endif
