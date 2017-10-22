#ifndef FIFO_STREAM_H
#define FIFO_STREAM_H

#include "types.h"

typedef struct fifo_stream_t
{
    struct FifoStreamRegs *reg_base;
} fifo_stream_t;

result_t init_fifo_stream(fifo_stream_t *fifo, uintptr_t base_addr);

result_t has_packet(fifo_stream_t *fifo, bool *packet_available);

result_t get_word(fifo_stream_t *fifo, uint32_t *word);

#endif
