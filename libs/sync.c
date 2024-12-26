#include "sync.h"
#include <malloc.h>

sync_t* sync_init() {
	sync_t* sync = (sync_t*)malloc(sizeof(sync_t));
	if (sync == NULL) {
		return NULL;
	}
	pthread_mutex_init(&(sync->pthread_mutex), NULL);
	return sync;
}

void sync_destroy(sync_t* sync) {
	if (sync == NULL) {
		return;
	}
	pthread_mutex_destroy(&(sync->pthread_mutex));
	free(sync);
}

int sync_lock_r(sync_t* sync) {
	return pthread_mutex_lock(&(sync->pthread_mutex));
}

int sync_lock_w(sync_t* sync) {
	return pthread_mutex_lock(&(sync->pthread_mutex));
}

int sync_unlock(sync_t* sync) {
	return pthread_mutex_unlock(&(sync->pthread_mutex));
}
