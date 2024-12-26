#ifndef __FIT_OS_CACHE_ROZHKOV_22202__
#define __FIT_OS_CACHE_ROZHKOV_22202__

#include "sync.h"

typedef struct _cache_block_t {
	struct _cache_block_t* next;
	char* data;
	int size;
} cache_block_t;

typedef struct _cache_node_t {
	struct _cache_node_t* next;
	struct _cache_node_t* prev;
	char* key;
	cache_block_t* block;
	sync_t* sync;
} cache_node_t;

typedef struct _cache_storage_t {
	cache_node_t* first;
	cache_node_t* last;
	unsigned int space_left;
	sync_t* sync;
} cache_storage_t;

cache_storage_t* cache_storage_init(unsigned int max_nodes);
void cache_storage_destroy(cache_storage_t* cache_storage);

cache_node_t* cache_node_init(char* key, cache_block_t* block);
void cache_node_destroy(cache_node_t* cache_node);

cache_block_t* cache_block_init(int size);
void cache_block_destroy(cache_block_t* cache_block);

cache_node_t* cache_find_pop(/*Nullable*/ cache_storage_t* cache_storage, char* key);
int cache_add_most_recent(cache_storage_t* cache_storage, cache_node_t* cache_node);
cache_node_t* cache_pop_least_used(cache_storage_t* cache_storage);
void cache_destroy_least_used(cache_storage_t* cache_storage);

#endif		// __FIT_OS_CACHE_ROZHKOV_22202__
