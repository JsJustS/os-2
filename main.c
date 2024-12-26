#include "proxy.h"
#include "stdlib.h"

int main(int argc, char* argv[]) {

	proxy_settings_t proxy_settings;
	proxy_settings_init(&proxy_settings);

	if (argc >= 2) {
		proxy_settings.port = atoi(argv[1]);
	}

	proxy_settings.should_log = 1;

	proxy_settings.queue_len = 100;
	proxy_settings.max_number_of_threads = 100;
	proxy_settings.buffer_size = 400;
	proxy_settings.max_buffer_size = 8000;

	proxy_settings.cache_enabled = 1;
	proxy_settings.max_cache_nodes = 100;
	proxy_settings.max_cache_block_size = 400;

	proxy_start(&proxy_settings);
	return 0;
}
