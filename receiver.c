#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include "multicast.h"
#include "message.h"

#define MCAST_ADDR "239.0.0.1"
#define PORT 5000
#define MAX_PAYLOAD 65536
#define MAX_CHUNKS 65536

bool received[MAX_CHUNKS];

int main()
{
    mcast_t *m = multicast_init(MCAST_ADDR, PORT, PORT);
    multicast_setup_recv(m);

    FILE *fp = NULL;
    uint32_t current_file_id = 0;
    uint32_t total_chunks = 0;
    uint32_t highest_seen = 0;
    bool done = false;

    int num_files_received = 0;
    int num_files_expected = 3;
    bool receivedFiles[MAX_CHUNKS] = {0};

    // Allocate buffer large enough for configurable payload chunks
    char *recv_buf = malloc(sizeof(msg_header) + MAX_PAYLOAD);
    if (!recv_buf) {
        perror("malloc");
        exit(1);
    }

    printf("Receiver started and listening on %s:%d\n", MCAST_ADDR, PORT);

    while (1) {
        if (multicast_check_receive(m) > 0) {
            int len = multicast_receive(m, recv_buf, sizeof(msg_header) + MAX_PAYLOAD);

            if (len < (int)sizeof(msg_header))
                continue;

            msg_header *hdr = (msg_header *)recv_buf;

            if(hdr->msg_type == MSG_TYPE_Info) {
                num_files_expected = hdr->file_id;
                printf("Session info received: expecting %u files\n", num_files_expected);
                continue;
            }


            if (hdr->msg_type != MSG_TYPE_DATA)
                continue;

            uint32_t seq = hdr->seq_num;
            uint32_t fid = hdr->file_id;

            if (seq >= MAX_CHUNKS) {
                printf("Chunk sequence out of range (%u >= %u)\n", seq, MAX_CHUNKS);
                continue;
            }

            if (receivedFiles[hdr->file_id]){ //fid < current_file_id) {
                // Packet from an older completed file, ignore it.
                continue;
            }

            if (fid != current_file_id && !receivedFiles[fid]){//fid > current_file_id) {
                if (fp) {
                    if (!done) {
                        printf("Warning: File ID %u incomplete, moving to File ID %u\n", current_file_id, fid);
                    }
                    fclose(fp);
                }
                
                char filename[256];
                snprintf(filename, sizeof(filename), "output_%u.bin", fid);
                fp = fopen(filename, "wb+");
                if (!fp) {
                    perror("fopen");
                    exit(1);
                }
                
                current_file_id = fid;
                memset(received, 0, sizeof(received));
                total_chunks = hdr->total_chunks;
                highest_seen = 0;
                done = false;
                
                printf("\nStarted receiving file ID %u (Total chunks: %u)\n", current_file_id, total_chunks);
            }

            if (done) {
                // Already completed this file, ignore redundant overlapping sender transmissions.
                continue;
            }

            if (total_chunks == 0)
                total_chunks = hdr->total_chunks;

            // Checksum validation
            char *data = recv_buf + sizeof(msg_header);
            uint32_t cs = calc_checksum(data, hdr->chunk_size);
            if (cs != hdr->checksum) {
                continue;  // Corrupt chunk
            }

            if (!received[seq]) {
                received[seq] = true;

                // Position cursor correctly adjusting per payload size
                fseek(fp, (size_t)seq * hdr->chunk_size, SEEK_SET);
                fwrite(data, 1, hdr->chunk_size, fp);

                printf("Received chunk %u\n", seq);

                if (seq > highest_seen)
                    highest_seen = seq;
            }

            // Detect gaps and predictably send requests for them
            int nacks_sent = 0;
            for (uint32_t i = 0; i < highest_seen; i++) {
                if (!received[i]) {
                    msg_header nack;
                    memset(&nack, 0, sizeof(nack));

                    nack.msg_type = MSG_TYPE_NACK;
                    nack.seq_num = i;
                    nack.file_id = current_file_id; 

                    multicast_send(m, &nack, sizeof(nack));

                    if (nacks_sent == 0) {
                         printf("NACK sent for chunk %u\n", i);
                    }
                    nacks_sent++;
                    
                    // suppress storm, handle sequentially cleanly
                    if (nacks_sent > 5) break;
                }
            }

            // Completion check loop mapping arrays against tracked completion max metrics
            done = true;
            for (uint32_t i = 0; i < total_chunks; i++) {
                if (!received[i]) {
                    done = false;
                    break;
                }
            }

            if (done) {
                printf("File ID %u complete\n", current_file_id);
                // We keep listening infinitely because we expect sequential files indefinitely, 
                // but we safely seal our generated disk file per transaction out.
                fclose(fp);
                fp = NULL;

                receivedFiles[current_file_id] = true;
                num_files_received++;
                if (num_files_received >= num_files_expected) {
                    printf("All expected files received.\n");
                }
            }
        }
    }

    if (fp) fclose(fp);
    free(recv_buf);
    multicast_destroy(m);
    return 0;
}