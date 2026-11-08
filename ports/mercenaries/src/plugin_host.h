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
/* Function hooks: wrappers in recomp/gen/foundry_hooks.c (written by
 * Mercenaries Foundry's port-hooks command) call recomp_foundry_hook_call. */
#include "../sdk/mercenaries_plugin.h"
int recomp_foundry_hook_index(uint32_t address);   /* -1 if not hookable */
void recomp_foundry_hook_enable(unsigned index);
int merc_plugins_hook_ready(void);                 /* on the owner thread, initialized */
void merc_plugins_hook_before(unsigned index, MercHookContext *call);
void merc_plugins_hook_after(unsigned index, MercHookContext *call);
/* Plugin script commands: command n has the guest address FOUNDRY_LUA_BASE +
 * 4n (in the XBE header, where no code lives); recomp_lookup_manual maps it
 * to a trampoline that calls merc_plugins_lua_command. */
#define FOUNDRY_LUA_BASE 0x00010F00u
#define FOUNDRY_LUA_MAX 64u
#define FOUNDRY_LUASCRIPT_REGISTER_COMMANDS 0x00112830u /* LuaScript_RegisterCommands */
#define FOUNDRY_LUA_PUSHSTRING 0x001DD000u
#define FOUNDRY_LUA_PUSHCCLOSURE 0x001DD0D0u
#define FOUNDRY_LUA_SETTABLE 0x001DD380u
#define FOUNDRY_LUA_GLOBALSINDEX 0xFFFFD8EFu /* -10001 */
/* Plugin tool windows: the host (main.c) gets told when the first opens and
 * the last closes, and treats that like the F9 panel. */
void merc_plugins_set_tool_display(void (*callback)(int open));
/* Plugin overlays: call fn for each plugin image (render thread, at present). */
void merc_plugins_each_overlay(void (*fn)(const void *pixels, unsigned width, unsigned height,
                                          unsigned x, unsigned y));
unsigned merc_plugins_lua_count(void);
int merc_plugins_lua_command(unsigned index, uint32_t L);
#ifdef __cplusplus
}
#endif
#endif

