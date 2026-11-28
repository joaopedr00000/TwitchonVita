#ifndef VT_PKTQUEUE_H
#define VT_PKTQUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

typedef struct packet {
    struct packet *next;
    uint8_t *data;
    int size;
    int64_t pts;
    int64_t dts;
} packet_t;

typedef struct {
    packet_t *head, *tail;
    int count;
    size_t bytes;
    int max_count;
    size_t max_bytes;
    int aborted;
    pthread_mutex_t lock;
    pthread_cond_t changed;
} pktqueue_t;

packet_t *pkt_new(const uint8_t *data, int size, int64_t pts, int64_t dts);
void pkt_free(packet_t *p);

int pq_init(pktqueue_t *q, int max_count, size_t max_bytes);
void pq_destroy(pktqueue_t *q);
/* Blocks while the queue is full. Takes ownership of p. Returns -1 if aborted. */
int pq_put(pktqueue_t *q, packet_t *p);
/* Returns NULL after timeout_ms or when aborted. */
packet_t *pq_get(pktqueue_t *q, int timeout_ms);
void pq_abort(pktqueue_t *q);
void pq_flush(pktqueue_t *q);
int pq_count(pktqueue_t *q);
/* Duration covered by queued packets (last pts - first pts), 90 kHz units. */
int64_t pq_span(pktqueue_t *q);

#endif
