#ifndef __FITOS_SYNC_H__
#define __FITOS_SYNC_H__

#include <pthread.h>
typedef struct _Sync {
	pthread_mutex_t pthread_mutex;
} sync_t;

sync_t* sync_init();
void sync_destroy(sync_t* sync);

int sync_lock_r(sync_t* sync);
int sync_lock_w(sync_t* sync);
int sync_unlock(sync_t* sync);

#endif		// __FITOS_SYNC_H__
