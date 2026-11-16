// Shim side of the bridge: owns the shared mapping, starts / reconnects to the host, appends commands, flushes.
#pragma once
#include "../common/protocol.h"
#include <string.h>

namespace bridge {

bool Connect();                 // open or create the mapping and make sure a live host is attached
void Disconnect();              // close handles (the host keeps running until the game exits)
bool Alive();                   // false once the host died or failed: all slots become no-ops
lr::SharedHeader *Header();

// Reserve a command of `payload` bytes (plus header). Flushes first when the arena is full. Returns the payload
// pointer (zeroed struct area is NOT guaranteed), or a scratch sink when the bridge is dead, never null.
void *Begin(lr::Cmd type, uint32_t payload);
// Shrink the last Begin to its real size (for streams built in place).
void End(void *payloadStart, uint32_t actualPayload);
uint32_t Room();                // bytes left for one more command payload after a flush
void Flush(bool wait = true);   // hand the arena to the host and wait until it has been decoded

template <class T> T *Emit(lr::Cmd type, uint32_t extra = 0) {
    T *p = (T *)Begin(type, (uint32_t)sizeof(T) + extra);
    memset(p, 0, sizeof(T));
    return p;
}

void Log(const char *fmt, ...); // lithray_shim.log in the game directory

} // namespace bridge
