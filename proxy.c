#include "proxy.h"
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/types.h>
#include <netdb.h>
#include <fcntl.h>
#include <limits.h>

#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)

// GLOBAL VARIABLES 
int number_of_threads = 0;

void proxy_print(proxy_settings_t* proxy_settings, const char* format, ...) {
	if (!proxy_settings->should_log) {
		return;
	}
	va_list argptr;
	va_start(argptr, format);
	vfprintf(stdout, format, argptr);
	va_end(argptr);
}

void proxy_settings_init(proxy_settings_t* proxy_settings) {
	proxy_settings->port = DEFAULT_PORT;
	proxy_settings->queue_len = DEFAULT_QUEUE_LEN;
	proxy_settings->max_number_of_threads = DEFAULT_MAX_NUMBER_OF_THREADS;
	proxy_settings->buffer_size = DEFAULT_BUFFER_SIZE;
	proxy_settings->max_buffer_size = DEFAULT_MAX_BUFFER_SIZE;
	proxy_settings->should_log = 0;
	proxy_settings->cache_enabled = 0;
	proxy_settings->max_cache_nodes = DEFAULT_MAX_CACHE_NODES;
	proxy_settings->max_cache_block_size = DEFAULT_MAX_CACHE_BLOCK_SIZE;
}

void* proxy_serve_client(void* client_args) {

	int client_socket_fd = ((proxy_client_arguments_t*)client_args)->client_socket_fd;
	proxy_settings_t* proxy_settings = ((proxy_client_arguments_t*)client_args)->proxy_settings;
	cache_storage_t* cache_storage = ((proxy_client_arguments_t*)client_args)->cache_storage;
	free(client_args);

	proxy_print(proxy_settings, "[%d] Serving client #%d\n", gettid(), client_socket_fd);

	// Считываем данные, отпарвленные клиентом
	char* request = NULL;
	int request_length = proxy_recv(client_socket_fd, &request, proxy_settings);
	if (request_length == -1) {
		proxy_send_s(client_socket_fd, HTTP_STATUS_400);
		proxy_print(proxy_settings, "[%d] Could not read data from client!\n", gettid());
		close(client_socket_fd);
		__sync_fetch_and_sub(&number_of_threads, 1);
		return NULL;
	}

	//proxy_print(proxy_settings, "[%d] Received data from client:\n%s\n", gettid(), request);

	// POST, GET и т.д.
	char* method = proxy_parse_method(request, request_length);
	if (method == NULL || !proxy_is_method_supported(method)) {
		proxy_print(proxy_settings, "[%d] Could not parse method!\n", gettid());
		proxy_send_s(client_socket_fd, HTTP_STATUS_501);
		free(request);
		close(client_socket_fd);
		__sync_fetch_and_sub(&number_of_threads, 1);
		return NULL;
	}

	proxy_print(proxy_settings, "[%d] Parsed method: %s\n", gettid(), method);

	// HTTP 1.0
	if (!proxy_is_version_supported(request, request_length)) {
		proxy_print(proxy_settings, "[%d] HTTP version is unsupported!\n", gettid());
		proxy_send_s(client_socket_fd, HTTP_STATUS_400);
		free(request);
		free(method);
		close(client_socket_fd);
		__sync_fetch_and_sub(&number_of_threads, 1);
		return NULL;
	}

	char* url = proxy_parse_url(request, request_length);
	if (url == NULL) {
		proxy_print(proxy_settings, "[%d] Could not parse URL!\n", gettid());
		proxy_send_s(client_socket_fd, HTTP_STATUS_400);
		free(request);
		free(method);
		close(client_socket_fd);
		__sync_fetch_and_sub(&number_of_threads, 1);
		return NULL;
	}

	proxy_print(proxy_settings, "[%d] Parsed url: %s\n", gettid(), url);

	char* host = NULL;
	char* path = NULL;
	int err = proxy_split_url(url, &host, &path);
	if (err) {
		proxy_print(proxy_settings, "[%d] Could not split url!\n", gettid());
		proxy_send_s(client_socket_fd, HTTP_STATUS_400);
		free(request);
		free(method);
		free(url);
		close(client_socket_fd);
		__sync_fetch_and_sub(&number_of_threads, 1);
		return NULL;
	}

	proxy_print(proxy_settings, "[%d] Parsed path: %s\n", gettid(), path);

	char* port = proxy_parse_port(host);

	proxy_print(proxy_settings, "[%d] Parsed host: %s\n", gettid(), host);
	proxy_print(proxy_settings, "[%d] Parsed port: %s\n", gettid(), port);

	int server_socket_fd = proxy_try_for_server_socket(host, port);
	if (server_socket_fd < 0) {
		proxy_print(proxy_settings, "[%d] Could not get address info!\n", gettid());
		if (err == EAI_SYSTEM) {
			printf("proxy_serve_client: getaddrinfo() failed: %s\n", strerror(errno));
		} else {
			printf("proxy_serve_client: getaddrinfo() failed: %s\n", gai_strerror(server_socket_fd));
		}
		proxy_send_s(client_socket_fd, HTTP_STATUS_502);
		__sync_fetch_and_sub(&number_of_threads, 1);
		free(request);
		free(method);
		free(host);
		free(path);
		free(url);
		close(client_socket_fd);
		return NULL;
	}

	int should_request_be_cached = 	(proxy_settings->cache_enabled) &&
					(cache_storage != NULL) &&
					(strcmp(method, "GET") == 0);

	if (!should_request_be_cached) {

		err = proxy_cache_request_and_send_partly(
			proxy_settings,
			server_socket_fd,
			client_socket_fd,
			NULL,
			NULL,
			request,
			request_length
		);

		if (err) {
			printf("proxy_serve_client: proxy_cache_request_and_send_partly() failed\n");
		} else {
			proxy_print(proxy_settings, "[%d] Successfuly worked with %s\n", gettid(), url);
		}

		__sync_fetch_and_sub(&number_of_threads, 1);
		free(request);
		free(method);
		free(host);
		free(path);
		free(url);
		close(client_socket_fd);
		return NULL;
	}

	proxy_print(proxy_settings, "[%d] LOCKING CACHE_STORAGE MUTEX...\n", gettid());
	pthread_mutex_lock(&(cache_storage->mutex));
	proxy_print(proxy_settings, "[%d] OBTAINED CACHE_STORAGE MUTEX\n", gettid());

	cache_node_t* cache_node = cache_find_pop(cache_storage, url);
	if (cache_node != NULL) {
		cache_add_most_recent(cache_storage, cache_node);

		pthread_mutex_unlock(&(cache_storage->mutex));
		proxy_print(proxy_settings, "[%d] UNLOCKED CACHE_STORAGE MUTEX...\n", gettid());

		if (!cache_node->marked_for_deletion) {
			proxy_print(proxy_settings, "[%d] Cache hit!\n", gettid());

			proxy_print(proxy_settings, "[%d] LOCKING CACHE_NODE MUTEX...\n", gettid());
			pthread_mutex_lock(&(cache_node->mutex));
			proxy_print(proxy_settings, "[%d] OBTAINED CACHE_NODE MUTEX\n", gettid());
			cache_node->readers_amount++;
			pthread_mutex_unlock(&(cache_node->mutex));
			proxy_print(proxy_settings, "[%d] UNLOCKED CACHE_NODE MUTEX...\n", gettid());

			proxy_cache_send_partly(proxy_settings, client_socket_fd, cache_node->block);

			proxy_print(proxy_settings, "[%d] LOCKING CACHE_NODE MUTEX...\n", gettid());
			pthread_mutex_lock(&(cache_node->mutex));
			proxy_print(proxy_settings, "[%d] OBTAINED CACHE_NODE MUTEX\n", gettid());
			cache_node->readers_amount--;
			pthread_cond_signal(&(cache_node->someone_finished_using));
			pthread_mutex_unlock(&(cache_node->mutex));
			proxy_print(proxy_settings, "[%d] UNLOCKED CACHE_NODE MUTEX...\n", gettid());

			__sync_fetch_and_sub(&number_of_threads, 1);
			free(request);
			free(method);
			free(host);
			free(path);
			free(url);
			close(client_socket_fd);
			return NULL;
		}
	}

	proxy_print(proxy_settings, "[%d] Cache miss.\n", gettid());

	struct timeval timeout;
	timeout.tv_sec = 60;
	timeout.tv_usec = 0;

	err = setsockopt(server_socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
	if (err) {
        	printf("proxy_serve_client: setsockopt() failed: %s\n", strerror(errno));
		proxy_send_s(client_socket_fd, HTTP_STATUS_500);
		__sync_fetch_and_sub(&number_of_threads, 1);
		free(request);
		free(method);
		free(host);
		free(path);
		free(url);
		close(client_socket_fd);
		close(server_socket_fd);
		pthread_mutex_unlock(&(cache_storage->mutex));
		proxy_print(proxy_settings, "[%d] UNLOCKED CACHE_STORAGE MUTEX...\n", gettid());
		return NULL;
	}

	err = proxy_cache_request_and_send_partly(
		proxy_settings,
		server_socket_fd,
		client_socket_fd,
		cache_storage,
		url,
		request,
		request_length
	);

	pthread_mutex_unlock(&(cache_storage->mutex));
	proxy_print(proxy_settings, "[%d] UNLOCKED CACHE_STORAGE MUTEX...\n", gettid());

	if (err) {
		printf("proxy_serve_client: proxy_cache_request_and_send_partly() failed\n");
	} else {
		proxy_print(proxy_settings, "[%d] Successfuly cached %s\n", gettid(), url);
	}

	__sync_fetch_and_sub(&number_of_threads, 1);
	free(request);
	free(method);
	free(host);
	free(path);
	free(url);
	close(client_socket_fd);
	return NULL;
}

void proxy_start(proxy_settings_t* proxy_settings) {
	int err;

	proxy_print(proxy_settings, "Starting proxy with following settings:\n\tPORT:\t\t\t%d\n\tQUEUE LEN:\t\t%d\n\tMAX NUMBER OF THREADS:\t%d\n\tBUFFER SIZE:\t\t%d\n\tMAX BUFFER SIZE:\t%d\n\tSHOULD LOG:\t\t%d\n\tCACHE ENABLED:\t\t%d\n\tMAX CACHE NODES:\t%d\n\tMAX CACHE BLOCK SIZE:\t%d\n", proxy_settings->port, proxy_settings->queue_len, proxy_settings->max_number_of_threads, proxy_settings->buffer_size, proxy_settings->max_buffer_size, proxy_settings->should_log, proxy_settings->cache_enabled, proxy_settings->max_cache_nodes, proxy_settings->max_cache_block_size);

	// Отменяем падение приложения при сигпайпе, обрыв сети обраюатываем сами
	if (SIG_ERR == signal(SIGPIPE, SIG_IGN)) {
		printf("proxy_start: signal() failed: %s\n", strerror(errno));
		abort();
	}

	// Создание серверного сокета
	int proxy_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (proxy_socket_fd < 0) {
		printf("proxy_start: socket() failed: %s\n", strerror(errno));
		abort();
	}

	// Настройка серверного сокета
	int option = 1;
	err = setsockopt(proxy_socket_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
	if (err) {
		printf("proxy_start: socket() failed: %s\n", strerror(errno));
		close(proxy_socket_fd);
		abort();
	}

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_port = htons(proxy_settings->port);

	// Присвоение сокету выданного адреса с указанным портом
	err = bind(proxy_socket_fd, (struct sockaddr *) &sin, sizeof(sin));
	if (err) {
		printf("proxy_start: bind() failed: %s\n", strerror(errno));
		close(proxy_socket_fd);
		abort();
	}

	// Запускаем прослушивание серверным сокетом на новые подключения
	err = listen(proxy_socket_fd, proxy_settings->queue_len);
	if (err) {
		printf("proxy_start: listen() failed: %s\n", strerror(errno));
		close(proxy_socket_fd);
		abort();
	}

	// Создание параметров для цикличных функций
	unsigned int socket_length = 0;

	pthread_attr_t thread_attribute;
	pthread_attr_init(&thread_attribute);
	pthread_attr_setdetachstate(&thread_attribute, PTHREAD_CREATE_DETACHED);

	cache_storage_t* cache_storage = NULL;
	if (proxy_settings->cache_enabled) {
		cache_storage = cache_storage_init(proxy_settings->max_cache_nodes);
		if (cache_storage == NULL) {
			printf("proxy_start: cache_storage_init() failed: %s\n", strerror(errno));
			close(proxy_socket_fd);
			abort();
		}
	}

	// Цикл сервера
	while (1) {
		if (number_of_threads < proxy_settings->max_number_of_threads) {
			int client_socket_fd = accept(
				proxy_socket_fd,
				(struct sockaddr *)&sin,
				&socket_length
			);

			if (client_socket_fd < 0) {
				printf("proxy_start: accept() failed: %s\n", strerror(errno));
				close(proxy_socket_fd);
				abort();
			}

			pthread_t tid;
			proxy_client_arguments_t* client_args = malloc(sizeof(proxy_client_arguments_t));
			if (client_args == NULL) {
				printf("proxy_start: malloc() failed: %s\n", strerror(errno));
				close(proxy_socket_fd);
				close(client_socket_fd);
				abort();
			}
			client_args->client_socket_fd = client_socket_fd;
			client_args->proxy_settings = proxy_settings;
			client_args->cache_storage = cache_storage;
			err = pthread_create(
				&tid,
				&thread_attribute,
				proxy_serve_client,
				(void*)client_args
			);
			if (err) {
				printf("proxy_start: pthread_create() failed: %s\n", strerror(errno));
				err = proxy_send_s(client_socket_fd, HTTP_STATUS_500);
				if (err) {
					printf("proxy_start: proxy_send() failed: %s\n", strerror(errno));
				}
				close(client_socket_fd);
			} else {
				__sync_fetch_and_add(&number_of_threads, 1);
			}
		}
	}
}

int proxy_send(int socket_fd, char* data, int length) {

	int total = 0;
	int bytes_left = length;
	int bytes_sent = 0;

	while (total < length) {
		bytes_sent = send(socket_fd, data + total, bytes_left, 0);
		if (bytes_sent == -1) {
			return -1;
		}
		total += bytes_sent;
		bytes_left -= bytes_sent;
	}
	return 0;
}

int proxy_send_s(int client_socket_fd, char* data) {
	return proxy_send(client_socket_fd, data, strlen(data));
}

int proxy_recv(int socket_fd, char** buffer, proxy_settings_t* proxy_settings) {
	(*buffer) = malloc(proxy_settings->buffer_size + 1);
	if ((*buffer) == NULL) {
		printf("proxy_recv: malloc() failed: %s\n", strerror(errno));
		return -1;
	}
	int length = 0;
	int requested_buffer_size = proxy_settings->buffer_size;
	int available = requested_buffer_size;

	proxy_print(proxy_settings, "[%d] Start receiving from socket...\n", gettid());

	int received = recv(socket_fd, (*buffer) + length, available, 0);
	while (received && received != -1) {
		proxy_print(proxy_settings, "[%d] Received %d byte(s) from socket.\n", gettid(), received);

		available -= received;
		length += received;

		// Проверяем, терминируются ли данные с сокета
		if (strstr((*buffer), "\r\n\r\n") || strstr((*buffer), "\n\n")) {
			break;
		}

		if (available == 0) { // Закончилось место в буфере
			if (requested_buffer_size*2 < proxy_settings->max_buffer_size) {
				requested_buffer_size *= 2;
				available = requested_buffer_size / 2;
			} else {
				available = proxy_settings->max_buffer_size - requested_buffer_size;
				requested_buffer_size = proxy_settings->max_buffer_size;
			}

			// TODO: Как это хендлить??? Работаем с огрызком или 500 Интернал еррор?
			if (available == 0) {
				printf("proxy_recv: too much data for max buffer size.\n");
				//break;
				free((*buffer));
				return -1;
			}

			(*buffer) = realloc((*buffer), requested_buffer_size + 1);
			if ((*buffer) == NULL) {
				printf("proxy_recv: realloc() failed: %s\n", strerror(errno));
				break;
			}
		}

		received = recv(socket_fd, (*buffer) + length, available, 0);
	}
	if (received == -1) {
		printf("proxy_recv: recv() failed: %s\n", strerror(errno));
		free((*buffer));
		return -1;
	}
	return length;
}

int proxy_request(int socket_fd, char* data, int data_length, char** response, proxy_settings_t* proxy_settings) {
	int err = proxy_send(socket_fd, data, data_length);
	if (err) {
		return -1;
	}
	return proxy_recv(socket_fd, response, proxy_settings);
}

int proxy_is_method_supported(char* method) {
	if (strcmp("GET", method) == 0) {
		return 1;
	}
	if (strcmp("HEAD", method) == 0) {
		return 1;
	}
	if (strcmp("POST", method) == 0) {
		return 1;
	}
	if (strcmp("PUT", method) == 0) {
		return 1;
	}
	if (strcmp("DELETE", method) == 0) {
		return 1;
	}
	if (strcmp("CONNECT", method) == 0) {
		return 1;
	}
	if (strcmp("OPTIONS", method) == 0) {
		return 1;
	}
	if (strcmp("TRACE", method) == 0) {
		return 1;
	}
	if (strcmp("PATCH", method) == 0) {
		return 1;
	}
	return 0;
}

int proxy_is_version_supported(char* data, int length) {
	int i;
	// method
	for (i = 0; data[i] != ' '; i++) {
		if (i > length - 1) {
			return 0;
		}
	}
	// пробелы между методом и url
	for (; data[i] == ' '; i++) {
		if (i > length - 1) {
			return 0;
		}
	}
	// url
	for (; data[i] != ' '; i++) {
		if (i > length - 1) {
			return 0;
		}
	}
	// пробелы между url и version
	for (; data[i] == ' '; i++) {
		if (i > length - 1) {
			return 0;
		}
	}
	char version[9];
	int j;
	for (j = 0; j < 8 && i < length; j++) {
		version[j] = data[i++];
	}
	version[j] = '\0';

	if (strcmp("HTTP/1.1", version) == 0) {
		return 1;
	}
	if (strcmp("HTTP/1.0", version) == 0) {
		return 1;
	}
	if (strcmp("HTTP/0.9", version) == 0) {
		return 1;
	}
	return 0;
}

char* proxy_parse_method(char* data, int length) {
	int max_length_of_http_method = 7;
	char buffer[max_length_of_http_method + 1];
	int i;
	for (i = 0; data[i] != ' '; i++) {
		if (i > max_length_of_http_method - 1 || i > length - 1) {
			// метод с слишком длинным названием
			return NULL;
		}
		buffer[i] = data[i];
	}
	buffer[i] = '\0';
	int method_length = strlen(buffer);
	char* method = malloc(sizeof(char) * (method_length + 1));
	if (method == NULL) {
		printf("proxy_parse_method: malloc() failed: %s\n", strerror(errno));
		return NULL;
	}
	strcpy(method, buffer);
	return method;
}

char* proxy_parse_url(char* data, int length) {
	int i;
	// method
	for (i = 0; data[i] != ' '; i++) {
		if (i > length - 1) {
			return NULL;
		}
	}
	// пробелы между методом и url
	for (; data[i] == ' '; i++) {
		if (i > length - 1) {
			return NULL;
		}
	}

	// Считаем длину url
	int url_length = 0;
	for (; data[i] != ' '; i++) {
		if (i > length - 1) {
			return NULL;
		}
		url_length++;
	}
	if (url_length == 0) {
		return NULL;
	}
	i -= url_length;

	char* url = (char*)malloc(sizeof(char) * (url_length + 1));
	if (url == NULL) {
		printf("proxy_parse_url: malloc() failed: %s\n", strerror(errno));
		return NULL;
	}
	int j = 0;
	for (; data[i] != ' '; i++) {
		// в таком же цикле уже проверили, что длина даты нормальная
		url[j++] = data[i];
	}
	url[j] = '\0';
	return url;
}

int proxy_split_url(char* url, char** host_ptr, char** path_ptr) {
	int url_length = strlen(url);
	int host_index = -1;
	int path_index = -1;
	char* url_without_protocol = strstr(url, "://");
	if (url_without_protocol != NULL) {
		url = url_without_protocol + 3;
	}
	host_index = 0;
	for (int i = 0; i < url_length; i++) {
		if (url[i] != '/') {
			continue;
		}
		path_index = i;
		break;
	}

	if (host_index == path_index) {
		return 1;
	}
	if (path_index == -1) {
		path_index = url_length;
	}

	char* host = (char*)malloc(sizeof(char) * (path_index - host_index + 1));
	if (host == NULL) {
		printf("proxy_split_url: malloc() failed: %s\n", strerror(errno));
		return 1;
	}
	int j = 0;
	for (int i = host_index; i < path_index; i++) {
		host[j++] = url[i];
	}
	host[j] = '\0';

	char* path = (char*)malloc(sizeof(char) * (url_length - path_index + 1));
	if (path == NULL) {
		printf("proxy_split_url: malloc() failed: %s\n", strerror(errno));
		return 1;
	}
	j = 0;
	for (int i = path_index; i < url_length; i++) {
		path[j++] = url[i];
	}
	path[j] = '\0';

	*host_ptr = host;
	*path_ptr = path;
	return 0;
}

char* proxy_parse_port(char* host) {
	char* port = strstr(host, ":");
	if (port != NULL) {
		*port = '\0';
		return port + 1;
	}
	return "80";
}

int proxy_cache_send_partly(
	proxy_settings_t* proxy_settings,
	int socket_fd,
	cache_block_t* cache_block
) {
	cache_block_t* current_cache_block = cache_block;
	while (current_cache_block != NULL) {
		int err = proxy_send(socket_fd, current_cache_block->data, current_cache_block->size);
		if (err) {
			printf("proxy_cache_send_partly: proxy_send() failed\n");
			return -1;
		}
		current_cache_block = current_cache_block->next;
	}
	return 0;
}

int proxy_cache_request_and_send_partly(
        proxy_settings_t* proxy_settings,
        int server_socket_fd,
        int client_socket_fd,
        cache_storage_t* cache_storage, /*Nullable*/
        char* url,                      /*Nullable*/
        char* request,
        int request_length
) {
	/*char* con = "\nConnection: close";
	char request_x[strlen(request) + strlen(con) + 1];
	int len = (int)(strstr(request, "\r\n\r\n") - request);
	strncpy(request_x, request, len);
	request_x[len] = '\0';
	strcat(request_x, con);
	strcat(request_x, request + len);
	printf("%s\n", request_x);*/
	int err = proxy_send(server_socket_fd, request, request_length);
	if (err) {
		printf("proxy_cache_request_and_send_partly: proxy_send() failed\n");
		return -1;
	}

	char* buffer = (char*) malloc(sizeof(char) * (proxy_settings->max_cache_block_size));
	if (buffer == NULL) {
		printf("proxy_cache_request_and_send_partly: malloc() failed: %s\n", strerror(errno));
		return -1;
	}

	cache_node_t* cache_node = NULL;
	cache_block_t* current_cache_block = NULL;
	if (cache_storage != NULL && url != NULL) {
		cache_node = cache_node_init(url, NULL);
	}

	unsigned long expected = ULONG_MAX;
	unsigned long total = 0;
//	int received = read(server_socket_fd, buffer, proxy_settings->max_cache_block_size);
	int received = recv(server_socket_fd, buffer, proxy_settings->max_cache_block_size, 0);
	while (received && received != -1) {
		proxy_print(proxy_settings, "[%d] Received from %d byte(s) from server.\n", gettid(), received);

		// Шаг 1. Отправить юзеру кусок ответа.
		//puts("Step 1.");
		err = proxy_send(client_socket_fd, buffer, received);
		if (err) {
			// Не смогли отправить (соединение упало)
			// Абортим кеширование
			printf("proxy_cache_request_and_send_partly: proxy_send() failed\n");
			free(buffer);
			if (cache_storage != NULL && url != NULL) {
				cache_node_destroy(cache_node);
			}
			return -1;
		}

		// Шаг 2. Превращаем кусок ответа в кэш
		//puts("Step 2.");
		if (cache_storage != NULL && url != NULL) {
			cache_block_t* new_cache_block = cache_block_init(received);
			if (new_cache_block == NULL) {
				printf("proxy_cache_request_and_send_partly: cache_block_init() failed\n");
				free(buffer);
				cache_node_destroy(cache_node);
				return -1;
			}
			memcpy(new_cache_block->data, buffer, received);
			if (current_cache_block == NULL) {
				cache_node->block = new_cache_block;
			} else {
				current_cache_block->next = new_cache_block;
			}
			current_cache_block = new_cache_block;
		}

		// Шаг 3. Повторить
		//puts("Step 3.");
		printf("... total: %lu\n", total);
		printf("... expected: %lu\n", expected);

		total += (unsigned long)received;
		if (expected == ULONG_MAX) {
			expected = parse_content_length_if_present(buffer, received);
			char buffer_terminated[received + 1];
			buffer_terminated[received] = '\0';
			memcpy(buffer_terminated, buffer, received);
			char* terminator = strstr(buffer, "\r\n\r\n");
			if (terminator != NULL) {
				expected += total - (received - (int)(terminator - buffer + 4));
			}
		}

		if (total >= expected) { // Как бы == должно быть но на всякий :)
			break;
		}
//		received = read(server_socket_fd, buffer, proxy_settings->max_cache_block_size);
		received = recv(server_socket_fd, buffer, proxy_settings->max_cache_block_size, 0);
	}

	cache_block_t* block = cache_node->block;
	while (block != NULL) {
		fwrite(block->data,sizeof(char),block->size,stdout);
		block = block->next;
	}

	if (received == -1) {
		printf("proxy_cache_request_and_send_partly: recv() failed: %s\n", strerror(errno));
		free(buffer);
		if (cache_storage == NULL || url == NULL) {
			return -1;
		}
		cache_node_destroy(cache_node);
		return -1;
	}

	if (cache_storage == NULL || url == NULL) {
		free(buffer);
		return 0;
	}

	// Кешируем данные для повторного использования.
	if (!cache_storage->space_left) {
		cache_destroy_least_used(cache_storage);
	}

	err = cache_add_most_recent(cache_storage, cache_node);

	if (err) {
		printf("proxy_cache_request_and_send_partly: cache_add_most_recent() failed: %s\n", strerror(errno));
		free(buffer);
		if (cache_storage == NULL || url == NULL) {
			return -1;
		}
		cache_node_destroy(cache_node);
		return -1;
	}

	free(buffer);
	return 0;
}

int proxy_try_for_server_socket(char* host, char* port) {
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo* server_infos;
	int err = getaddrinfo(host, port, &hints, &server_infos);
	if (err) {
		return err;
	}

	struct addrinfo* current_server_info = server_infos;
	int server_socket_fd;
	while (current_server_info != NULL) {
		server_socket_fd = socket(
			current_server_info->ai_family,
			current_server_info->ai_socktype,
			current_server_info->ai_protocol
		);

		if (server_socket_fd == -1) {
			printf("proxy_serve_client: socket() failed: %s\n", strerror(errno));
			return -1;
		}

		err = connect(
			server_socket_fd,
			current_server_info->ai_addr,
			current_server_info->ai_addrlen
		);
		if (err) {
			printf("proxy_serve_client: connect() failed: %s\n", strerror(errno));
			close(server_socket_fd);
			current_server_info = current_server_info->ai_next;
			continue;
		}
		break;
	}
	freeaddrinfo(server_infos);

	// Вышли из цикла, так и не найдя рабочий адрес
	if (current_server_info == NULL) {
		return -1;
	}

	return server_socket_fd;
}

unsigned long parse_content_length_if_present(char* headers, int headers_length) {
	char* header = "Content-Length:";
	char headers_terminated[headers_length + 1];
	headers_terminated[headers_length] = '\0';
	memcpy(headers_terminated, headers, headers_length);
	headers = strstr(headers_terminated, header);
	//printf("%s\n", headers_terminated);
	if (headers == NULL) {
		return ULONG_MAX;
	}

	headers += strlen(header);
	for (; *headers < '0' || *headers > '9'; headers++) {}

	int length_of_str = 0;
	for (; '0' < *headers && *headers < '9'; headers++) {length_of_str++;}

	headers -= length_of_str;
	char length[length_of_str + 1];
	length[length_of_str] = '\0';
	memcpy(length, headers, length_of_str);

	return strtoul(length, NULL, 10);
}
