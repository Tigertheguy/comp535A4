#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MSG_TYPE_DATA 1
#define MSG_TYPE_NACK 2
#define MSG_TYPE_Info 3

typedef struct {
    uint32_t msg_type;    // MSG_TYPE_DATA or MSG_TYPE_ACK
    uint32_t seq_num;     // Chunk sequence number
    uint32_t file_id;     // ID
    uint32_t total_chunks;
    uint32_t checksum;    // Payload checksum
    uint32_t chunk_size;  // Configured chunk size (needed for offset calculation)
} msg_header;

// Checksum utility definition
static inline uint32_t calc_checksum(const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum;
}

#endif
