#ifndef MERCENARIES_PLUGIN_HOST_H
#define MERCENARIES_PLUGIN_HOST_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int merc_plugins_initialize(void *guest_base, size_t guest_bytes);
int merc_plugins_update(void);
void merc_plugins_shutdown(void);
const char *merc_plugins_error(void);
void merc_plugins_show_error(void);
/* Guest calls for plugins (recomp_manual.c). */
int recomp_plugin_call_guest(uint32_t address, uint32_t this_ptr, const uint32_t *args, unsigned count,
                             uint32_t *eax, uint32_t *edx, double *st0, char *error, size_t error_size);
int recomp_plugin_guest_scratch(uint32_t *address, uint32_t *bytes);
int recomp_plugin_in_gameplay(void);
#ifdef __cplusplus
}
#endif
#endif

