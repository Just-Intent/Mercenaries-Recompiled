/* The game's main heap (retail 0x1806A0) is one fixed 18.6 MiB block with
 * 7,000 block descriptors at 0x406298. Large mods fill it while streaming;
 * the retail allocator then retries forever (0x1F70F0) instead of failing.
 *
 * With the extended graphics bank the retail 22.5 MiB graphics pool no longer
 * comes from the lower 64 MiB, so expanded_game_heap=1 gives 16 MiB of that
 * room to the main heap and moves its descriptors to a larger table. Without
 * the graphics bank the option is ignored: retail sizes leave no room.
 */
#include "kernel/xbox_memory_layout.h"
#include "mod_compatibility.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define RETAIL_HEAP_ALLOCATION 0x0129999Au
#define RETAIL_HEAP_SIZE 0x0129599Au
#define RETAIL_HEAP_TABLE 0x00406298u
#define RETAIL_HEAP_BLOCKS 0x1B58u
#define EXPANDED_HEAP_EXTRA (16u * 1024u * 1024u)
#define EXPANDED_HEAP_BLOCKS 16384u

static int expanded_heap(void)
{
    static int decided, expanded;
    if (!decided) {
        decided = 1;
        if (recomp_mod_expanded_game_heap()) {
            if (xbox_GetExtendedGraphicsPool())
                expanded = 1;
            else
                fprintf(stderr, "[GAME-HEAP] expanded_game_heap needs "
                                "extended_graphics_memory=1; using the retail size\n");
        }
    }
    return expanded;
}

uint32_t recomp_game_heap_allocation(void)
{
    uint32_t size = RETAIL_HEAP_ALLOCATION;
    if (expanded_heap()) {
        size += EXPANDED_HEAP_EXTRA;
        fprintf(stderr, "[GAME-HEAP] main heap %u bytes (retail %u), "
                        "%u bytes free in the lower bank before it\n",
                size, RETAIL_HEAP_ALLOCATION, xbox_HeapFrontierBytes());
        fflush(stderr);
    }
    return size;
}

uint32_t recomp_game_heap_size(void)
{
    return RETAIL_HEAP_SIZE + (expanded_heap() ? EXPANDED_HEAP_EXTRA : 0u);
}

uint32_t recomp_game_heap_blocks(void)
{
    return expanded_heap() ? EXPANDED_HEAP_BLOCKS : RETAIL_HEAP_BLOCKS;
}

uint32_t recomp_game_heap_table(void)
{
    static uint32_t table;
    if (!expanded_heap())
        return RETAIL_HEAP_TABLE;
    if (!table) {
        table = xbox_HeapAlloc(EXPANDED_HEAP_BLOCKS * 8u, 64u);
        if (!table) {
            fprintf(stderr, "[GAME-HEAP] cannot allocate %u block descriptors\n",
                    EXPANDED_HEAP_BLOCKS);
            fflush(stderr);
            exit(86);
        }
    }
    return table;
}

/* Called with the main heap's allocation; zero means the lower bank was full. */
void recomp_game_heap_check(uint32_t base)
{
    if (base)
        return;
    fprintf(stderr, "[GAME-HEAP] cannot allocate the %u-byte main heap%s\n",
            RETAIL_HEAP_ALLOCATION + (expanded_heap() ? EXPANDED_HEAP_EXTRA : 0u),
            expanded_heap() ? "; set expanded_game_heap=0" : "");
    fflush(stderr);
    exit(86);
}
