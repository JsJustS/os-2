#ifndef __FIT_OS_PROXY_ROZHKOV_22202__
#define __FIT_OS_PROXY_ROZHKOV_22202__

#include "cache.h"

#define DEFAULT_PORT 9192
#define DEFAULT_QUEUE_LEN 10
#define DEFAULT_MAX_NUMBER_OF_THREADS 10
#define DEFAULT_BUFFER_SIZE 100
#define DEFAULT_MAX_BUFFER_SIZE 800
#define DEFAULT_MAX_CACHE_NODES 10
#define DEFAULT_MAX_CACHE_BLOCK_SIZE 800

#define HTTP_STATUS_200 "HTTP/1.0 200 OK\r\n\r\n"
#define HTTP_STATUS_400 "HTTP/1.0 400 Bad Request\r\n\r\n"
#define HTTP_STATUS_500 "HTTP/1.0 500 Internal Server Error\r\n\r\n"
#define HTTP_STATUS_501 "HTTP/1.0 501 Not Implemented\r\n\r\n"
#define HTTP_STATUS_502 "HTTP/1.0 502 Bad Gateway\r\n\r\n"

typedef struct _proxy_settings_t {
	int port;
	int queue_len;
	int max_number_of_threads;
	int buffer_size;
	int max_buffer_size;
	int should_log;
	int cache_enabled;
	unsigned int max_cache_nodes;
	int max_cache_block_size;
} proxy_settings_t;

typedef struct _proxy_client_arguments_t {
	int client_socket_fd;
	proxy_settings_t* proxy_settings;
	cache_storage_t* cache_storage;
} proxy_client_arguments_t;

void proxy_settings_init(proxy_settings_t* proxy_settings);

void proxy_start(proxy_settings_t* proxy_settings);
void* proxy_serve_client(void* client_args);

int proxy_send_s(int socket_fd, char* data);
int proxy_send(int socket_fd, char* data, int length);

int proxy_cache_send_partly(
	proxy_settings_t* proxy_settings,
	int socket_fd,
	cache_block_t* cache_block
);
int proxy_cache_request_and_send_partly(
	proxy_settings_t* proxy_settings,
	int server_socket_fd,
	int client_socket_fd,
	cache_storage_t* cache_storage,	/*Nullable*/
	char* url, 			/*Nullable*/
	char* request,
	int request_length
);

int proxy_recv(int socket_fd, char** buffer, proxy_settings_t* proxy_settings);

int proxy_request(int socket_fd, char* data, int data_length, char** response, proxy_settings_t* proxy_settings);

char* proxy_parse_method(char* data, int length);
char* proxy_parse_url(char* data, int length);
int proxy_split_url(char* url, char** host, char** path);
char* proxy_parse_port(char* host);

int proxy_is_method_supported(char* method);
int proxy_is_version_supported(char* buffer, int length);

int proxy_try_for_server_socket(char* host, char* port);
unsigned long parse_content_length_if_present(char* request, int request_length);

#endif		// __FIT_OS_PROXY_ROZHKOV_22202__
