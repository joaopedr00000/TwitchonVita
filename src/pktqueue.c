#include "pktqueue.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>

packet_t *pkt_new(const uint8_t *data, int size, int64_t pts, int64_t dts)
{
    if (!data || size <= 0) return NULL;
    packet_t *p = malloc(sizeof(*p));
    if (!p)
        return NULL;
    /* 64 bytes of zero padding: decoders may read slightly past the end */
    p->data = malloc((size_t)size + 64);
    if (!p->data) {
        free(p);
        return NULL;
    }
    memcpy(p->data, data, (size_t)size);
    memset(p->data + size, 0, 64);
    p->size = size;
    p->pts = pts;
    p->dts = dts;
    p->next = NULL;
    return p;
}

void pkt_free(packet_t *p)
{
    if (!p)
        return;
    free(p->data);
    free(p);
}

int pq_init(pktqueue_t *q, int max_count, size_t max_bytes)
{
    memset(q, 0, sizeof(*q));
    q->max_count = max_count;
    q->max_bytes = max_bytes;
    int rc = pthread_mutex_init(&q->lock, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&q->changed, NULL);
    if (rc != 0) pthread_mutex_destroy(&q->lock);
    return rc;
}

void pq_flush(pktqueue_t *q)
{
    pthread_mutex_lock(&q->lock);
    packet_t *p = q->head;
    while (p) {
        packet_t *n = p->next;
        pkt_free(p);
        p = n;
    }
    q->head = q->tail = NULL;
    q->count = 0;
    q->bytes = 0;
    pthread_cond_broadcast(&q->changed);
    pthread_mutex_unlock(&q->lock);
}

void pq_destroy(pktqueue_t *q)
{
    pq_flush(q);
    pthread_cond_destroy(&q->changed);
    pthread_mutex_destroy(&q->lock);
}

void pq_abort(pktqueue_t *q)
{
    pthread_mutex_lock(&q->lock);
    q->aborted = 1;
    pthread_cond_broadcast(&q->changed);
    pthread_mutex_unlock(&q->lock);
}

int pq_put(pktqueue_t *q, packet_t *p)
{
    if (!p) return -1;
    if ((size_t)p->size > q->max_bytes) { pkt_free(p); return -1; }
    pthread_mutex_lock(&q->lock);
    while (!q->aborted && (q->count >= q->max_count || (size_t)p->size > q->max_bytes - q->bytes)) {
        if (pthread_cond_wait(&q->changed, &q->lock) != 0) {
            pthread_mutex_unlock(&q->lock); pkt_free(p); return -1;
        }
    }
    if (q->aborted) { pthread_mutex_unlock(&q->lock); pkt_free(p); return -1; }
    p->next = NULL;
    if (q->tail) q->tail->next = p; else q->head = p;
    q->tail = p; q->count++; q->bytes += (size_t)p->size;
    pthread_cond_broadcast(&q->changed);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

packet_t *pq_get(pktqueue_t *q, int timeout_ms)
{
    struct timeval now;
    struct timespec deadline;
    gettimeofday(&now, NULL);
    deadline.tv_sec = now.tv_sec + (timeout_ms > 0 ? timeout_ms / 1000 : 0);
    deadline.tv_nsec = now.tv_usec * 1000L + (timeout_ms > 0 ? (timeout_ms % 1000) * 1000000L : 0);
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&q->lock);
    while (!q->aborted && !q->head && timeout_ms > 0) {
        if (pthread_cond_timedwait(&q->changed, &q->lock, &deadline) != 0) break;
    }
    packet_t *p = q->aborted ? NULL : q->head;
    if (p) {
        q->head = p->next;
        if (!q->head) q->tail = NULL;
        q->count--; q->bytes -= (size_t)p->size; p->next = NULL;
        pthread_cond_broadcast(&q->changed);
    }
    pthread_mutex_unlock(&q->lock);
    return p;
}

int pq_count(pktqueue_t *q)
{
    pthread_mutex_lock(&q->lock);
    int n = q->count;
    pthread_mutex_unlock(&q->lock);
    return n;
}

int64_t pq_span(pktqueue_t *q)
{
    int64_t span = 0;
    pthread_mutex_lock(&q->lock);
    if (q->head && q->tail && q->head->pts != INT64_MIN && q->tail->pts != INT64_MIN)
        span = q->tail->pts - q->head->pts;
    pthread_mutex_unlock(&q->lock);
    return span;
}
