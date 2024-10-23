#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>

#define WORKER_THREADS 5
#define QUEUE_SIZE 5
#define FILE_BUF_SIZE 1024

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

Queue *queue;

char files_dir_path[1024];

void* worker(void* arg) {
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
    char *path_start = strchr(buffer, ' ')+1;
    char *headers_start = strchr(path_start, ' ');

    char method[path_start - buffer];
    char path[headers_start-path_start];

    strncpy(method, buffer, path_start-buffer-1);
    strncpy(path, path_start, headers_start-path_start);

    int match = 0;

    if (strcmp(path, "/") == 0) {
      match = 1;
      char *res = "HTTP/1.1 200 OK\r\n\r\n";
      bytes_sent = send(client_fd, res, strlen(res), 0);
    } else if (strncmp("/echo/", path, 6) == 0) {
      match = 1;
      char res[1024];
      char *msg = path + 6;
      sprintf(res, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %ld\r\n\r\n%s", strlen(msg), msg);
      bytes_sent = send(client_fd, res, strlen(res), 0);
    } else if (strncmp("/user-agent", path, 11) == 0) {
      match = 1;
      char res[1042];

      // TODO: remove the use of strtok
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
      sprintf(filepath, "%s%s", files_dir_path, path + 6);

      if (strcmp(method, "GET") == 0) {
        int file_d = open(filepath, O_RDONLY, 0);
        if (file_d < 0) {
          printf("Can't open file '%s'\n", filepath);
        } else {
          match = 1;
          char filebuf[FILE_BUF_SIZE];
          int filesize = 0;
          int n;
          while ((n = read(file_d, filebuf, FILE_BUF_SIZE)) > 0) {
            filesize += n;
          }
          printf("Sending file '%s' (%d bytes)...\n", filepath, filesize);
          char res[2048];
          sprintf(res, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n%s", filesize, filebuf);
          bytes_sent = send(client_fd, res, strlen(res), 0);
        }
      } else if (strcmp(method, "POST") == 0) {
        int file_d = creat(filepath, 0666);
        if (file_d < 0) {
          printf("Can't write to file '%s'\n", filepath);
        } else {
          match = 1;
          char *body_start = strstr(headers_start, "\r\n\r\n")+4;
          char body[FILE_BUF_SIZE];
          strcpy(body, body_start);
          // printf("body: %s\n", body);
          write(file_d, body, sizeof(body));
          char *res = "HTTP/1.1 201 Created\r\n\r\n";
          bytes_sent = send(client_fd, res, strlen(res), 0);
        }
      }
    }

    if (!match) {
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
        strcpy(files_dir_path, *++argv);
        --argc;
      }
    }
  } else {
    getcwd(files_dir_path, sizeof((files_dir_path)));
  }
  printf("Directory is set to '%s'\n", files_dir_path);

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
