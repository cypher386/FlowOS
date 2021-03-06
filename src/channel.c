#include "../include/channel.h"
#include "../include/task.h"

#include <rte_mempool.h>
#include <assert.h>

static struct rte_mempool *channel_pool = NULL;
static const size_t channel_capacity = 1024;
int channel_pool_create(uint16_t capacity) {
  assert(channel_pool == NULL);
  channel_pool = rte_mempool_create("flowos_channel_pool",
				    capacity,
				    sizeof(struct channel),
				    0, 0, 
				    NULL, NULL, NULL, NULL,
				    0, 0);
  if (! channel_pool) {
    printf("channel_pool_create(): failed to create channel pool.\n");
    return -1;
  }
  return 0;
}

void channel_pool_destroy() {
  channel_pool = NULL;
}

void channel_init(channel_t channel, size_t size) {
  rte_atomic16_init(&channel->size);
  channel->capacity = size;
  TAILQ_INIT(&channel->head);
  rte_spinlock_init(&channel->lock);
}

channel_t channel_get() {
  channel_t channel;
  if (rte_mempool_mc_get(channel_pool, (void**)&channel) != 0) {
    printf("channel_get(): channel pool is empty.\n");
    return NULL;
  }
  channel_init(channel, channel_capacity);
  return channel;
}

void channel_put(channel_t channel) {
  rte_mempool_put(channel_pool, channel);
}

int channel_is_full(channel_t channel) {
  return rte_atomic16_cmpset(&channel->size, channel->capacity, channel->capacity);
}

int channel_is_empty(channel_t channel) {
  uint16_t exp = 0;
  return rte_atomic16_cmpset(&channel->size, exp, 0);
}

void channel_insert(channel_t channel, packet_t pkt) {
  rte_spinlock_lock(&channel->lock);
  assert(channel->size < channel->capacity);
  TAILQ_INSERT_TAIL(&channel->head, pkt, list);
  channel->size++;
  //printf("channel_insert(): size = %d\n", channel_size(channel));
  rte_spinlock_unlock(&channel->lock);
  if (channel->consumer && channel_is_full(channel)) {
    task_unblock_rx(channel->consumer, channel->consumerIndex);
  }
}

packet_t channel_remove(channel_t channel) {
  packet_t pkt;
  rte_spinlock_lock(&channel->lock);
  assert(channel->size > 0);
  pkt = TAILQ_FIRST(&channel->head);
  TAILQ_REMOVE(&channel->head, pkt, list);
  channel->size--;
  //printf("channel_remove(): size = %d\n", channel_size(channel));
  rte_spinlock_unlock(&channel->lock);
  if (channel->producer && channel_is_empty(channel)) {
    task_unblock_tx(channel->producer, channel->producerIndex);
  }
  return pkt;
}

int channel_size(channel_t channel) {
  return rte_atomic16_read(&channel->size);
}

packet_t channel_peek(channel_t channel) {
  assert(! channel_is_empty(channel));
  return TAILQ_FIRST(&channel->head);
}

void channel_close(channel_t channel) {
  rte_atomic16_set(&channel->closed, 1);
  if (channel->consumer) {
    task_unblock_rx(channel->consumer, channel->consumerIndex);
  }
}

int channel_is_closed(channel_t channel) {
  return rte_atomic16_read(&channel->closed);
}

void channel_register_producer(channel_t channel, uint8_t index, task_t producer) {
  assert(channel->producer == NULL);
  assert(index < producer->txCount);
  channel->producerIndex = index;
  channel->producer = producer;
}

void channel_register_consumer(channel_t channel, uint8_t index, task_t consumer) {
  assert(channel->consumer == NULL);
  assert(index < consumer->rxCount);
  channel->consumerIndex = index;
  channel->consumer = consumer;
}

void channel_notify_producer(channel_t channel) {
  if (channel->producer) {
    task_unblock_tx(channel->producer, channel->producerIndex);
  }
}

void channel_notify_consumer(channel_t channel) {
  if (channel->consumer) {
    task_unblock_rx(channel->consumer, channel->consumerIndex);
  }
}
