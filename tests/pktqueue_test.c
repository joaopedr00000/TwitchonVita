#include "pktqueue.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
static const uint8_t b[4] = {0};
static void *producer(void *arg) { return (void *)(intptr_t)pq_put(arg,pkt_new(b,4,0,0)); }
static void *consumer(void *arg) { return pq_get(arg,1000); }
int main(void) {
    pktqueue_t q; pthread_t t; void *result;
    pq_init(&q,1,4);
    assert(pq_get(&q,1)==NULL);
    assert(pq_put(&q,pkt_new(b,4,10,10))==0);
    assert(pq_count(&q)==1);
    packet_t *p=pq_get(&q,0); assert(p && p->pts==10); pkt_free(p);
    assert(pq_put(&q,pkt_new(b,4,0,0))==0);
    assert(pthread_create(&t,NULL,producer,&q)==0);
    pq_abort(&q); assert(pthread_join(t,&result)==0); assert((intptr_t)result==-1);
    assert(pq_get(&q,0)==NULL); pq_destroy(&q);
    pq_init(&q,1,4); assert(pthread_create(&t,NULL,consumer,&q)==0);
    pq_abort(&q); assert(pthread_join(t,&result)==0 && result==NULL);
    pq_destroy(&q); puts("PASS packet queue wake/abort"); return 0;
}
