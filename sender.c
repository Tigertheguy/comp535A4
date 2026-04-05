#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdbool.h>

#include "multicast.h"
#include "message.h"

#define MCAST_ADDR "239.0.0.1"
#define PORT 5000

#define WINDOW_SIZE 32
#define DEFAULT_CHUNK_SIZE 1024

long long total_bytes_sent = 0;
long long total_chunks_sent = 0;
long long total_retransmissions = 0;
long long total_nacks_received = 0;

static long long get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void send_file(mcast_t *m, const char *filename, uint32_t file_id, int chunk_size) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("fopen");
        return;
    }

    fseek(fp, 0, SEEK_END);
    long filesize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint32_t total_chunks = (filesize + chunk_size - 1) / chunk_size;
    if (total_chunks == 0) total_chunks = 1;

    char *chunk_buffer = malloc(chunk_size);
    if (!chunk_buffer) {
        perror("malloc");
        fclose(fp);
        return;
    }

    long long *send_times = calloc(total_chunks, sizeof(long long));
    if (!send_times) {
        perror("calloc");
        free(chunk_buffer);
        fclose(fp);
        return;
    }

    uint32_t base = 0;
    uint32_t next = 0;

    printf("Sending file %s (ID: %u, Size: %ld, Chunks: %u, Chunk Size: %d)\n", 
           filename, file_id, filesize, total_chunks, chunk_size);
    
    long long file_start_time = get_time_ms();

    while (base < total_chunks) {
        
        while (next < base + WINDOW_SIZE && next < total_chunks) {
            msg_header hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.msg_type = MSG_TYPE_DATA;
            hdr.seq_num = next;
            hdr.file_id = file_id;
            hdr.total_chunks = total_chunks;
            hdr.chunk_size = chunk_size;

            size_t offset = (size_t)next * chunk_size;
            fseek(fp, offset, SEEK_SET);
            size_t bytes_to_read = chunk_size;
            if (offset + chunk_size > filesize) {
                bytes_to_read = filesize - offset;
            }
            if (filesize == 0) bytes_to_read = 0;
            
            memset(chunk_buffer, 0, chunk_size);
            int n = fread(chunk_buffer, 1, bytes_to_read, fp);
            if (n < 0) n = 0;

            hdr.checksum = calc_checksum(chunk_buffer, chunk_size);

            int packet_size = sizeof(msg_header) + chunk_size;
            char *packet = malloc(packet_size);
            memcpy(packet, &hdr, sizeof(msg_header));
            memcpy(packet + sizeof(msg_header), chunk_buffer, chunk_size);

            multicast_send(m, packet, packet_size);
            
            send_times[next] = get_time_ms();
            total_bytes_sent += packet_size;
            total_chunks_sent++;

            free(packet);

            printf("Sent chunk %u\n", next);
            next++;

            usleep(1000);
        }

        if (multicast_check_receive(m) > 0) {
            msg_header nack;
            int len = multicast_receive(m, &nack, sizeof(nack));

            if (len >= (int)sizeof(msg_header) && nack.msg_type == MSG_TYPE_NACK) {
                total_nacks_received++;
                uint32_t missing = nack.seq_num;
                
                if (missing < total_chunks) {
                    msg_header hdr;
                    memset(&hdr, 0, sizeof(hdr));
                    hdr.msg_type = MSG_TYPE_DATA;
                    hdr.seq_num = missing;
                    hdr.file_id = file_id;
                    hdr.total_chunks = total_chunks;
                    hdr.chunk_size = chunk_size;

                    size_t offset = (size_t)missing * chunk_size;
                    fseek(fp, offset, SEEK_SET);
                    size_t bytes_to_read = chunk_size;
                    if (offset + chunk_size > filesize) {
                        bytes_to_read = filesize - offset;
                    }
                    if (filesize == 0) bytes_to_read = 0;
            
                    memset(chunk_buffer, 0, chunk_size);
                    int n = fread(chunk_buffer, 1, bytes_to_read, fp);
                    if (n < 0) n = 0;

                    hdr.checksum = calc_checksum(chunk_buffer, chunk_size);

                    int packet_size = sizeof(msg_header) + chunk_size;
                    char *packet = malloc(packet_size);
                    memcpy(packet, &hdr, sizeof(msg_header));
                    memcpy(packet + sizeof(msg_header), chunk_buffer, chunk_size);

                    multicast_send(m, packet, packet_size);
                    
                    send_times[missing] = get_time_ms();
                    total_bytes_sent += packet_size;
                    total_chunks_sent++;
                    total_retransmissions++;

                    free(packet);

                    printf("Retransmitted chunk %u\n", missing);
                }
            }
        }

        long long current_time = get_time_ms();
        
        while (base < next && (current_time - send_times[base] > 300)) {
            base++;
        }

        if (next == total_chunks && current_time - send_times[total_chunks - 1] > 100) {
            uint32_t missing = total_chunks - 1;
            
            msg_header hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.msg_type = MSG_TYPE_DATA;
            hdr.seq_num = missing;
            hdr.file_id = file_id;
            hdr.total_chunks = total_chunks;
            hdr.chunk_size = chunk_size;

            size_t offset = (size_t)missing * chunk_size;
            fseek(fp, offset, SEEK_SET);
            size_t bytes_to_read = chunk_size;
            if (offset + chunk_size > filesize) {
                bytes_to_read = filesize - offset;
            }
            if (filesize == 0) bytes_to_read = 0;
    
            memset(chunk_buffer, 0, chunk_size);
            int n = fread(chunk_buffer, 1, bytes_to_read, fp);
            if (n < 0) n = 0;

            hdr.checksum = calc_checksum(chunk_buffer, chunk_size);

            int packet_size = sizeof(msg_header) + chunk_size;
            char *packet = malloc(packet_size);
            memcpy(packet, &hdr, sizeof(msg_header));
            memcpy(packet + sizeof(msg_header), chunk_buffer, chunk_size);

            multicast_send(m, packet, packet_size);
            
            send_times[missing] = current_time;
            total_bytes_sent += packet_size;
            total_chunks_sent++;
            total_retransmissions++;

            free(packet);
        }

        usleep(1000);
    }

    free(send_times);
    free(chunk_buffer);
    fclose(fp);

    printf("Finished sending file ID %u in %lld ms\n\n", file_id, get_time_ms() - file_start_time);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s [-c chunk_size] <file1> <file2> ...\n", argv[0]);
        exit(1);
    }

    int chunk_size = DEFAULT_CHUNK_SIZE;
    int opt;

    while ((opt = getopt(argc, argv, "c:")) != -1) {
        switch (opt) {
            case 'c':
                chunk_size = atoi(optarg);
                if (chunk_size <= 0) {
                    fprintf(stderr, "Invalid chunk size: %d\n", chunk_size);
                    exit(1);
                }
                break;
            default:
                fprintf(stderr, "Usage: %s [-c chunk_size] <file1> <file2> ...\n", argv[0]);
                exit(1);
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: missing input files\n");
        exit(1);
    }

    mcast_t *m = multicast_init(MCAST_ADDR, PORT, PORT);

    printf("Sender start");
    int cnt = 0;
    while(1){
        //Broadcast number of files being transmitted
        uint32_t num_files = (uint32_t)(argc - optind);
        msg_header session_info;
        memset(&session_info, 0, sizeof(session_info));
        session_info.msg_type = MSG_TYPE_Info;
        session_info.file_id = num_files;
        multicast_send(m, &session_info, sizeof(session_info));
        usleep(100000); 

        cnt ++;
        uint32_t file_id = 1;
        for (int i = optind; i < argc; i++) {
            send_file(m, argv[i], file_id++, chunk_size);
        }
    
        printf("\n--- Transmission Statistics for Loop %d ---\n", cnt);
        printf("Total chunks sent: %lld\n", total_chunks_sent);
        printf("Total bytes sent: %lld\n", total_bytes_sent);
        printf("Total NACKs received: %lld\n", total_nacks_received);
        printf("Total retransmissions: %lld\n", total_retransmissions);

        usleep(100000);    
    }

    multicast_destroy(m);
    return 0;
}