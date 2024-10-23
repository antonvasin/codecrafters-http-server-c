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

typedef struct Queue {
  int *sockets;
  int size;
  int first;
  int last;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
} Queue;

typedef struct ThreadPool {
  pthread_t *threads;
  int count;
} ThreadPool;

// typedef struct WorkerThread {
//   Queue *queue;
//   int id;
// } WorkerThread;

Queue *queue;

char file_dir[1024];

void* worker(void* arg) {
  // struct WorkerThread *params = (WorkerThread *) arg;
  // Queue *queue = params->queue;
  int id = *(int *) arg;

  printf("[worker %d] Starting new worker.\n", id);
  // Waiting for work
  while (1) {
    // Getting client socket
    pthread_mutex_lock(&queue->mutex);

    while (queue->first == queue->last) {
      pthread_cond_wait(&queue->cond, &queue->mutex);
    }
    int client_fd = queue->sockets[queue->first];
    queue->first = (queue->first + 1) % queue->size;
    pthread_mutex_unlock(&queue->mutex);

    // printf("[worker %d] Handling request.\n", id);
    char buffer[1024];

    if (recv(client_fd, buffer, 1024, 0) < 0) {
      printf("[worker %d] Can't receive request: %s\n", id, strerror(errno));
      // return 1;
    }

    int bytes_sent;
    char *path = strtok(buffer, " ");
    path = strtok(NULL, " ");

    if (strcmp(path, "/") == 0) {
      char *res = "HTTP/1.1 200 OK\r\n\r\n";
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
    } else if (strncmp("/files/", path, 7) == 0) {
      char filepath[1024];
      sprintf(filepath, "%s%s", file_dir, path + 6);
      FILE *file_d;
      if ((file_d = fopen(filepath, "r")) == NULL) {
        printf("Can't open file '%s'\n", filepath);
        continue;
      }
      printf("Sending file '%s'...\n", filepath);
      char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n";
      bytes_sent = send(client_fd, res, strlen(res), 0);
    } else {
      char* res =  "HTTP/1.1 404 Not Found\r\n\r\n";
      bytes_sent = send(client_fd, res, strlen(res), 0);
    }
    printf("[worker %d] Sent %d bytes.\n", id, bytes_sent);
    close(client_fd);
  }
  printf("[worker %d] Closing ...\n", id);
  pthread_exit(NULL);
}

int main(int argc, char **argv) {
  // Disable output buffering
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  if (argc > 1) {
    while (--argc > 0 && *(++argv)[0] == '-') {
      if (strstr(*argv, "--directory") != NULL) {
        // Save file_dir and advance current pointer one arg ahead
        strcpy(file_dir, *++argv);
        --argc;
        printf("Directory is set to '%s'\n", file_dir);
      }
    }
  }

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
  ThreadPool *pool = (ThreadPool *)malloc(sizeof(ThreadPool));
  if (pool == NULL) return 1;
  pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * WORKER_THREADS);
  if (pool->threads == NULL) return 1;
  pool->count = WORKER_THREADS;

  // Queue *queue = (Queue *)malloc(sizeof(Queue));
  queue = (Queue *)malloc(sizeof(Queue));
  if (queue == NULL) return 1;

  queue->sockets = (int *)malloc(sizeof(int) * QUEUE_SIZE);
  if (queue->sockets == NULL) return 1;

  queue->size = QUEUE_SIZE;
  queue->first = 0;
  queue->last = 0;
  if (pthread_mutex_init(&queue->mutex, NULL) != 0) return 1;

  if (pthread_cond_init(&queue->cond, NULL) != 0) {
    pthread_mutex_destroy(&queue->mutex);
    return 1;
  }

  int threads[WORKER_THREADS];
  for (int i = 0; i < WORKER_THREADS; ++i) {
    // struct WorkerThread worker_params = {
    //   .queue = queue,
    //   .id = i,
    // };
    threads[i] = i;
    if (pthread_create(&pool->threads[i], NULL, worker, &threads[i]) != 0) {
      printf("Failed to create thread %d\n", i);
      return 1;
    }
  }

  // Listen for requests
  while (1) {
    client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
    if (client_fd < 0) break;
    pthread_mutex_lock(&queue->mutex);

    queue->sockets[queue->last] = client_fd;
    queue->last = (queue->last + 1) % queue->size;

    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
  }

  // Cleanup queue
  // TODO: handle signals
  printf("Cleaning up...\n");
  free(queue->sockets);
  pthread_mutex_destroy(&queue->mutex);
  pthread_cond_destroy(&queue->cond);
  free(queue);
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
