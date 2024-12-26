#include "cache.h"
#include <errno.h>
#include <string.h>
#include <malloc.h>

cache_storage_t* cache_storage_init(unsigned int max_nodes) {
	cache_storage_t* cache_storage = (cache_storage_t*) malloc(sizeof(cache_storage_t));
	if (cache_storage == NULL) {
		printf("cache_storage_init: malloc() failed: %s\n", strerror(errno));
		return NULL;
	}
	cache_storage->first = NULL;
	cache_storage->last= NULL;
	cache_storage->space_left = max_nodes;
	cache_storage->sync = sync_init();
	return cache_storage;
}

void cache_storage_destroy(cache_storage_t* cache_storage) {
	if (cache_storage == NULL) {
		return;
	}
	cache_node_t* current_node = cache_storage->first;
	while (current_node != NULL) {
		cache_node_t* temp = current_node->next;
		cache_node_destroy(current_node);
		current_node = temp;
	}
	sync_destroy(cache_storage->sync);
	free(cache_storage);
}

cache_node_t* cache_node_init(char* key, /*Nullable*/ cache_block_t* block) {
	if (key == NULL) {
		return NULL;
	}
	cache_node_t* cache_node = (cache_node_t*) malloc(sizeof(cache_node_t));
	if (cache_node == NULL) {
		printf("cache_node_init: malloc() failed: %s\n", strerror(errno));
		return NULL;
	}
	cache_node->next = NULL;
	cache_node->prev = NULL;
	cache_node->key = (char*) malloc(sizeof(char) * (strlen(key) + 1));
	if (cache_node->key == NULL) {
		printf("cache_node_init: malloc() failed: %s\n", strerror(errno));
		free(cache_node);
		return NULL;
	}
	strcpy(cache_node->key, key);
	cache_node->block = block;
	cache_node->sync = sync_init();
	if (cache_node->sync == NULL) {
		printf("cache_node_init: sync_init() failed: %s\n", strerror(errno));
		free(cache_node);
		return NULL;
	}
	return cache_node;
}

void cache_node_destroy(cache_node_t* cache_node) {
	if (cache_node == NULL) {
		return;
	}
	cache_block_t* current_block = cache_node->block;
	while (current_block != NULL) {
		cache_block_t* temp = current_block->next;
		cache_block_destroy(current_block);
		current_block = temp;
	}
	sync_destroy(cache_node->sync);
	free(cache_node);
}

cache_block_t* cache_block_init(int size) {
	cache_block_t* cache_block = (cache_block_t*) malloc(sizeof(cache_block_t));
	if (cache_block == NULL) {
		printf("cache_block_init: malloc() failed: %s\n", strerror(errno));
		return NULL;
	}
	cache_block->next = NULL;
	cache_block->data = (char*) malloc(sizeof(char)*size);
	if (cache_block->data == NULL) {
		printf("cache_block_init: malloc() failed: %s\n", strerror(errno));
		free(cache_block);
		return NULL;
	}
	cache_block->size = size;
	return cache_block;
}

void cache_block_destroy(cache_block_t* cache_block) {
	if (cache_block == NULL) {
		return;
	}
	free(cache_block->data);
	free(cache_block);
}

/* Searches for key. If found, pops element from cache, as if it never been there.
   Intended to be used alongside cache_set_most_recent()
   Thread-safe.
*/
cache_node_t* cache_find_pop(/*Nullable*/ cache_storage_t* cache_storage, /*Nullable*/ char* key) {
	if (cache_storage == NULL || key == NULL) {
		return NULL;
	}

	sync_lock_w(cache_storage->sync);

	cache_node_t* current_node = cache_storage->first;
	while (current_node != NULL) {
		if (strcmp(key, current_node->key) == 0) {
			if (current_node->prev != NULL) {	// not first
				sync_lock_w(current_node->prev->sync);
				current_node->prev->next = current_node->next;
				sync_unlock(current_node->prev->sync);
			} else {				// first
				cache_storage->first = current_node->next;
			}
			if (current_node->next != NULL) {	// not last
				sync_lock_w(current_node->next->sync);
				current_node->next->prev = current_node->prev;
				sync_unlock(current_node->next->sync);
			} else {				// last
				cache_storage->last = current_node->prev;
			}
			// remove cache context
			current_node->prev = NULL;
			current_node->next = NULL;
			cache_storage->space_left++;

			sync_unlock(cache_storage->sync);

			return current_node;
		}
		current_node = current_node->next;
	}

	sync_unlock(cache_storage->sync);

	return NULL;
}

/* Basically add to cache as the first element in the queue.
   Returns 0 on success, -1 if error.
   Thread-safe.
*/
int cache_add_most_recent(
	cache_storage_t* cache_storage,
	cache_node_t* cache_node
) {
	if (cache_storage == NULL) {
		printf("cache_set_most_recent: cache_storage is NULL\n");
		return -1;
	}

	sync_lock_w(cache_storage->sync);

	if (cache_storage->space_left == 0) {
		sync_unlock(cache_storage->sync);
		return -1;
	}

	cache_node_t* old_first = cache_storage->first;
	cache_storage->first = cache_node;
	cache_node->prev = NULL;
	cache_node->next = old_first;

	if (old_first != NULL) {
		sync_lock_w(old_first->sync);
		old_first->prev = cache_node;
		sync_unlock(old_first->sync);
	}

	if (cache_storage->last == NULL) {
		cache_storage->last = cache_node;
	}
	cache_storage->space_left--;

	sync_unlock(cache_storage->sync);
	return 0;
}
/* Pops least used element (which is stored as last), if such exists.
   Thread-safe.
*/
cache_node_t* cache_pop_least_used(cache_storage_t* cache_storage) {
	if (cache_storage == NULL) {
		printf("cache_pop_least_used: cache_storage is NULL\n");
		return NULL;
	}

	sync_lock_w(cache_storage->sync);

	if (cache_storage->last == NULL) {
		sync_unlock(cache_storage->sync);
		return NULL;
	}
	cache_node_t* old_last = cache_storage->last;
	cache_storage->last = old_last->prev;

	sync_lock_w(old_last->sync);
	old_last->prev = NULL;
	old_last->next = NULL; // Should be NULL already, but lets ensure.
	sync_unlock(old_last->sync);

	sync_lock_w(cache_storage->last->sync);
	cache_storage->last->next = NULL;
	sync_unlock(cache_storage->last->sync);

	if (cache_storage->first == old_last) { // If the element was alone in cache.
		cache_storage->first = NULL;
	}
	cache_storage->space_left++;
	sync_unlock(cache_storage->sync);
	return old_last;
}

/* Destroys least used element (which is stored as last), if such exists.
   Use cache_pop_least_used() if no destruction needed.
   Thread-safe.
*/
void cache_destroy_least_used(cache_storage_t* cache_storage) {
	if (cache_storage == NULL) {
		printf("cache_destroy_least_used: cache_storage is NULL\n");
		return;
	}
	cache_node_destroy(cache_pop_least_used(cache_storage));
}
