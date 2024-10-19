#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#define WORKER_THREADS 5
#define QUEUE_SIZE 5

volatile int server_running = 1;
pthread_mutex_t server_running_mutex;

typedef struct soc_queue {
  int *sockets;
  int size;
  int first;
  int last;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
} soc_queue;

typedef struct tpool {
  pthread_t *threads;
  int count;
  soc_queue *queue;
} tpool;

typedef struct worker_req {
  int client_fd;
} worker_req;

tpool *pool;

void* worker(void* arg) {
  tpool *pool = (tpool *)arg;

  printf("Starting new worker.\n");
  // Waiting for work
  while (1) {
    // Check whether we're running
    pthread_mutex_lock(&server_running_mutex);
    int running = server_running;
    pthread_mutex_unlock(&server_running_mutex);
    if (!running) break;

    // Getting client socket
    pthread_mutex_lock(&(pool->queue->mutex));

    while (pool->queue->first == pool->queue->last) {
        pthread_mutex_lock(&server_running_mutex);
        int running = server_running;
        pthread_mutex_unlock(&server_running_mutex);
        if (!running) {
          pthread_mutex_unlock(&(pool->queue->mutex));
          break;
        }
        pthread_cond_wait(&(pool->queue->cond), &(pool->queue->mutex));
    }
    int client_fd = pool->queue->sockets[pool->queue->first];
    pool->queue->first = (pool->queue->first + 1) % pool->queue->size;
    pthread_mutex_unlock(&(pool->queue->mutex));
    printf("[debug] socket %d\n", client_fd);

    printf("Handling request.\n");
    char buffer[1024];

    if (recv(client_fd, buffer, 1024, 0) < 0) {
      printf("Can't receive request: %s\n", strerror(errno));
      // return 1;
    }

    int bytes_sent;
    char *path = strtok(buffer, " ");
    path = strtok(NULL, " ");

    if (strcmp(path, "/") == 0) {
      char* res = "HTTP/1.1 200 OK\r\n\r\n";
      bytes_sent = send(client_fd, res, strlen(res), 0);
    } else if (strncmp("/echo/", path, 6) == 0) {
      char res[1024];
      char *msg = path + 6;
      sprintf(res, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
      bytes_sent = send(client_fd, res, strlen(res), 0);
    } else if (strncmp("/user-agent", path, 11) == 0) {
      char res[1042];

      char *header = strtok(NULL, "\r\n");
      char *msg = "";
      while (header != NULL) {
        // FIXME: headers are case-insensitive
        if (strncmp("User-Agent:", header, 11) == 0) {
          strtok(header, ":");
          msg = strtok(NULL, "\r\n") + 1;
          break;
        }
        header = strtok(NULL, "\r\n");
      }

      sprintf(res, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
      bytes_sent = send(client_fd, res, strlen(res), 0);
    } else {
      char* res =  "HTTP/1.1 404 Not Found\r\n\r\n";
      bytes_sent = send(client_fd, res, strlen(res), 0);
    }
    printf("Sent %d bytes.\n", bytes_sent);
    close(client_fd);
  }
  pthread_exit(NULL);
}

int main() {
	// Disable output buffering
	setbuf(stdout, NULL);
 	setbuf(stderr, NULL);

	// You can use print statements as follows for debugging, they'll be visible when running tests.
	printf("Logs from your program will appear here!\n");

	int server_fd, client_fd;
  struct sockaddr_in client_addr;
  socklen_t client_addr_len;

	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1) {
		printf("Socket creation failed: %s...\n", strerror(errno));
		return 1;
	}

	// Since the tester restarts your program quite often, setting SO_REUSEADDR
	// ensures that we don't run into 'Address already in use' errors
	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		printf("SO_REUSEADDR failed: %s \n", strerror(errno));
		return 1;
	}

	struct sockaddr_in serv_addr = {
    .sin_family = AF_INET ,
    .sin_port = htons(4221),
    .sin_addr = { htonl(INADDR_ANY) },
  };

	if (bind(server_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0) {
		printf("Bind failed: %s \n", strerror(errno));
		return 1;
	}

	int connection_backlog = 5;
	if (listen(server_fd, connection_backlog) != 0) {
		printf("Listen failed: %s \n", strerror(errno));
		return 1;
	}

  // Initialize thread pool
  pool = (tpool *)malloc(sizeof(tpool));
  if (pool == NULL) return 1;
  pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * WORKER_THREADS);
  if (pool->threads == NULL) {
    free(pool);
    return 1;
  }
  pool->count = WORKER_THREADS;
  pool->queue = (soc_queue *)malloc(sizeof(soc_queue));
  if (pool->queue == NULL) return 1;
  pool->queue->sockets = (int *)malloc(sizeof(int) * QUEUE_SIZE);
  if (pool->queue->sockets == NULL) {
    free(pool->queue);
    free(pool);
    return 1;
  }
  pool->queue->size = QUEUE_SIZE;
  pool->queue->first = 0;
  pool->queue->last = 0;
  if (pthread_mutex_init(&(pool->queue->mutex), NULL) != 0) {
    free(pool->queue->sockets);
    free(pool->queue);
    free(pool);
    return 1;
  }
  if (pthread_cond_init(&(pool->queue->cond), NULL) != 0) {
    pthread_mutex_destroy(&(pool->queue->mutex));
    free(pool->queue->sockets);
    free(pool->queue);
    free(pool);
    return 1;
  }

  for (int i = 0; i < WORKER_THREADS; ++i) {
    // worker_ids[i] = i;
    if (pthread_create(&pool->threads[i], NULL, worker, (void *)pool) != 0) {
      printf("Failed to create thread %d\n", i);
      return 1;
    }
  }

  // Listen for requests
  while (1) {
    // fd_set read_fds;
    // struct timeval timeout;

    // FD_ZERO(&read_fds);
    // FD_SET(server_fd, &read_fds);

    // Set the timeout to 1 second
    // timeout.tv_sec = 1;
    // timeout.tv_usec = 0;
    // int result;
    // result = select(server_fd+1, &read_fds, NULL, NULL, &timeout);
    // printf("Got new request. %d\n", result);
    // if (result == -1) {
    //   perror("select");
    //   break;
    // // timeout
    // } else if (result == 0) {
    //   break;
    // } else {
      client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
      if (client_fd < 0) break;
      // Enqueue job
      pthread_mutex_lock(&(pool->queue->mutex));

      pool->queue->sockets[pool->queue->last] = client_fd;
      pool->queue->last = (pool->queue->last + 1) % pool->queue->size;

      pthread_cond_signal(&pool->queue->cond);
      pthread_mutex_unlock(&pool->queue->mutex);
      printf("Enqueue job, %d\n", pool->queue->last);
    // }
  }

  // Cleanup queue
  pthread_mutex_destroy(&server_running_mutex);
  free(pool->queue->sockets);
  pthread_mutex_destroy(&(pool->queue->mutex));
  pthread_cond_destroy(&(pool->queue->cond));
  free(pool->queue);
  for (int i = 0; i < pool->count; ++i) {
      if (pthread_join(pool->threads[i], NULL) != 0) {
          fprintf(stderr, "Error joining thread. Continuing...\n");
      }
  }
  free(pool->threads);
  free(pool);

  close(server_fd);

  return 0;
}
