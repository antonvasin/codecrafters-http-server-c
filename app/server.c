#define _GNU_SOURCE
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
#include <ctype.h>
#include <zlib.h>

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

char* gzip_deflate(char *data, size_t data_len, size_t *gzip_len) {
  z_stream z;

  z.zalloc = Z_NULL;
  z.zfree = Z_NULL;
  z.opaque = Z_NULL;

  // Init zlib
  deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY);
  // Get max length of resulting compression and malloc the buffer with it
  size_t max_len = deflateBound(&z, data_len);
  char *output = malloc(max_len);

  z.avail_in = data_len;
  z.next_in = (Bytef *)data;
  z.avail_out = max_len;
  z.next_out = (Bytef *)output;

  deflate(&z, Z_FINISH);
  deflateEnd(&z);

  *gzip_len = z.total_out;
  printf("[z] Zipping '%s' (%ld) -> %02x (%ldb), in %d, out %d\n", data, data_len, *output, *gzip_len, z.avail_in, z.avail_out);
  return output;
}

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

    char request_buf[1024];

    if (recv(client_fd, request_buf, 1024, 0) < 0) {
      printf("[worker %d] Can't receive request: %s\n", id, strerror(errno));
      continue;
    }

    // --- Parsing request ---
    // GET /echo/hello HTTP/1.1\r\nHost: localhost:4221\r\n\r\n
    const char *path_start = strchr(request_buf, ' ') + 1;
    const char *protocol_start = strchr(path_start, ' ') + 1;
    const char *headers_start = strstr(protocol_start, "\r\n") + 1;
    const char *body_start = strstr(headers_start, "\r\n\r\n") + 4;

    char method[path_start - request_buf];
    char path[protocol_start-path_start];

    strncpy(method, request_buf, path_start-request_buf);
    strncpy(path, path_start, protocol_start-path_start);

    // FIXME: simpler pointer math
    method[sizeof(method)-1] = '\0';
    path[sizeof(path)-1] = '\0';

    int match = 0;
    char res[8192];
    char status[256];
    char headers[256];
    int headers_len = 0;
    char body[4096];
    size_t body_len;
    int bytes_sent;
    int needs_gzip = 0;

    const char *encoding_start = strcasestr(headers_start, "Accept-Encoding:");
    if (encoding_start != NULL) {
      encoding_start += 16;
      if (encoding_start[0] == ' ') ++encoding_start;
      // const char *encoding_end = strstr(encoding_start, "\r\n");
      // printf("encoding %s\n", encoding_start);
      if (strstr(encoding_start, "gzip") != NULL) {
        headers_len += sprintf(headers, "Content-Encoding: gzip\r\n");
        needs_gzip = 1;
      }
    }

    // --- Known paths ----
    if (strcmp(path, "/") == 0) {
      match = 1;
      strcpy(status, "200 OK");
    } else if (strncmp("/echo/", path, 6) == 0) {
      match = 1;
      char *msg = path + 6;
      body_len = strlen(msg);
      if (needs_gzip) {
        char *zipped = gzip_deflate(msg, strlen(msg), &body_len);
        memcpy(body, zipped, body_len);
        free(zipped);
      } else {
        sprintf(body, "%s", msg);
      }
      headers_len += sprintf(headers+headers_len, "Content-Type: text/plain\r\nContent-Length: %ld\r\n", body_len);
      strcpy(status, "200 OK");
    } else if (strncmp("/user-agent", path, 11) == 0) {
      match = 1;

      const char *uagent_start = strstr(headers_start, "User-Agent:")+11;
      if (uagent_start[0] == ' ')  ++uagent_start;
      const char *uagent_end = strstr(uagent_start, "\r\n");

      char agent[128];
      strncpy(agent, uagent_start, uagent_end - uagent_start);
      agent[uagent_end - uagent_start] = '\0';
      strcpy(status, "200 OK");
      body_len = strlen(agent);
      headers_len += sprintf(headers+headers_len, "Content-Type: text/plain\r\nContent-Length: %ld\r\n", body_len);
      sprintf(body, "%s", agent);
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
          strcpy(status, "200 OK");
          body_len = filesize;
          headers_len += sprintf(headers+headers_len, "Content-Type: application/octet-stream\r\nContent-Length: %ld\r\n", body_len);
          sprintf(body, "%s", filebuf);
        }
      } else if (strcmp(method, "POST") == 0) {
        printf("Attempting to write file to '%s'\n", filepath);
        int file_d = creat(filepath, 0666);
        if (file_d < 0) {
          printf("Can't write to file '%s'\n", filepath);
        } else {
          match = 1;
          int clength = 0;
          const char *clength_start = strcasestr(headers_start, "Content-Length:");
          if (clength_start != NULL) {
            clength = strtol(clength_start+16, NULL, 10);
          }
          write(file_d, body_start, clength);
          strcpy(status, "201 Created");
        }
      }
    }

    if (!match) {
      printf("Not found: %s %s\n", method, path);
      char* res = "HTTP/1.1 404 Not Found\r\n\r\n";
      bytes_sent = send(client_fd, res, strlen(res), 0);
    }

    if (needs_gzip) {
      int headers_len = sprintf(res, "HTTP/1.1 %s\r\n%s\r\n", status, headers);
      // Using memcpy for binary data, otherwise (strcpy, etc) will stop at first `\0`;
      memcpy(res + headers_len, body, body_len);
      bytes_sent = send(client_fd, res, headers_len + body_len, 0);
    } else {
      sprintf(res, "HTTP/1.1 %s\r\n%s\r\n%s\r\n", status, headers, body);
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
