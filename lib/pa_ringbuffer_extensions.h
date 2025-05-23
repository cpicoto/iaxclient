#ifndef PA_RINGBUFFER_EXTENSIONS_H
#define PA_RINGBUFFER_EXTENSIONS_H

#include "pa_ringbuffer.h"

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

/**
 * Get the number of elements currently in the ring buffer (filled count).
 * This is a helper function that is not in the standard PortAudio library.
 *
 * @param rbuf The ring buffer.
 * @return The number of elements available for reading (filled count).
 */
static __inline ring_buffer_size_t PaUtil_GetRingBufferFullCount(const PaUtilRingBuffer *rbuf)
{
    return PaUtil_GetRingBufferReadAvailable(rbuf);
}

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PA_RINGBUFFER_EXTENSIONS_H */
