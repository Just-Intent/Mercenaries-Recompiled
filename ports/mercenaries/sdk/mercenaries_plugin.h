#ifndef MERCENARIES_PLUGIN_H
#define MERCENARIES_PLUGIN_H
#include <stdint.h>
#include <stddef.h>
#define MERCENARIES_PLUGIN_API_VERSION 1u
#define MERC_PLUGIN_CALL __cdecl
#define MERC_PLUGIN_EXPORT __declspec(dllexport)
#ifdef __cplusplus
extern "C" {
#endif
/* Windows x64 C ABI. Existing v1 fields/signatures must never change. */
typedef struct MercPluginUpdate {
    uint32_t struct_size, api_version;
    uint64_t update_index;
    double wall_delta_seconds; /* Real time, NOT simulation time; advances while paused. */
} MercPluginUpdate;
/* A call into the game's own (recompiled) code. Fill address, this_ptr,
 * arg_count and args; the host fills the results. Arguments are 32-bit
 * stack words in source order (left to right); pass a float by its bit
 * pattern and a pointer as a guest address. */
#define MERC_GUEST_CALL_MAX_ARGS 16u
typedef struct MercGuestCall {
    uint32_t struct_size;
    uint32_t address;      /* guest virtual address of the function */
    uint32_t this_ptr;     /* ECX for __thiscall methods; 0 otherwise */
    uint32_t arg_count;    /* at most MERC_GUEST_CALL_MAX_ARGS */
    uint32_t args[MERC_GUEST_CALL_MAX_ARGS];
    uint32_t eax, edx;     /* out: integer/pointer result */
    double st0;            /* out: x87 top after the call (float/double results) */
    char error[96];        /* out: why the call was refused */
} MercGuestCall;
/* A hooked call of a game function. In `before`: address, ecx (this, may be
 * changed) and esp ([esp] = return address, [esp + 4] = first argument; change
 * arguments with write_guest). Set skip = 1 to not run the original; then set
 * eax/edx and pop_bytes (bytes of arguments the original pops itself: 4 x
 * argument count for __stdcall/__thiscall, 0 for __cdecl). In `after`: eax,
 * edx (may be changed) and st0 (the x87 top: float results, read only). */
typedef struct MercHookContext {
    uint32_t struct_size;
    uint32_t address;
    uint32_t ecx;
    uint32_t esp;
    uint32_t eax, edx;
    double st0;
    int32_t skip;
    uint32_t pop_bytes;
} MercHookContext;
typedef void (MERC_PLUGIN_CALL *MercHookFn)(void *userdata, MercHookContext *call);
typedef struct MercPluginHost {
    uint32_t struct_size, api_version;
    void *context;
    const char *plugin_id;          /* UTF-8 strings, valid until shutdown. */
    const char *plugin_directory;   /* Absolute private staged files. */
    const char *settings_directory; /* Absolute persistent folder outside mod-cache. */
    void (MERC_PLUGIN_CALL *log)(void *, const char *);
    int (MERC_PLUGIN_CALL *read_guest)(void *, uint32_t, void *, size_t);
    int (MERC_PLUGIN_CALL *write_guest)(void *, uint32_t, const void *, size_t);
    /* Guest calls (appended; check MERC_PLUGIN_HOST_HAS before use). Valid only
     * inside on_update, on the game's own thread, at its input checkpoint. */
    int (MERC_PLUGIN_CALL *call_guest)(void *, MercGuestCall *);
    /* Guest memory free for this update (strings, matrices passed to calls):
     * returns 1 and an address/size, or 0. Contents don't survive the update. */
    int (MERC_PLUGIN_CALL *guest_scratch)(void *, uint32_t *, uint32_t *);
    /* 1 while normal gameplay is running (not menus or loading). */
    int (MERC_PLUGIN_CALL *in_gameplay)(void *);
    /* Run before/after (either may be NULL) whenever the game calls the
     * function at address, on the game's main thread. Only functions built
     * as hookable (named in the Foundry function map) can be hooked; returns
     * 0 otherwise. Register from init or on_update; hooks last until shutdown.
     * Callbacks may use read_guest, write_guest, call_guest and log. */
    int (MERC_PLUGIN_CALL *hook_guest)(void *, uint32_t, MercHookFn, MercHookFn, void *);
} MercPluginHost;
/* True when the host provides a field (older hosts have a smaller struct). */
#define MERC_PLUGIN_HOST_HAS(host, field) \
    ((host)->struct_size >= offsetof(MercPluginHost, field) + sizeof((host)->field) && (host)->field)
typedef struct MercPlugin {
    uint32_t struct_size, api_version;
    void *userdata;
    int (MERC_PLUGIN_CALL *on_update)(void *, const MercPluginUpdate *);
    void (MERC_PLUGIN_CALL *on_shutdown)(void *);
} MercPlugin;
/* Export with C linkage. Different from the legacy MercenariesMod_Init ABI.
 * Return 1 on success, 0 on failure; error is UTF-8/NUL terminated.
 * Fill callbacks/userdata. A supplied shutdown callback is also called if init fails.
 */
typedef int (MERC_PLUGIN_CALL *MercPluginEntry)(const MercPluginHost *,
    MercPlugin *, char *, uint32_t);
#ifdef MERC_PLUGIN_IMPLEMENTATION
MERC_PLUGIN_EXPORT int MERC_PLUGIN_CALL MercenariesPlugin_Init(
    const MercPluginHost *, MercPlugin *, char *, uint32_t);
#endif
#ifdef __cplusplus
}
#endif
#endif

