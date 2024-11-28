#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>

#define PORT 8083
#define BUFFER_SIZE 1024
#define THREAD_POOL_SIZE 4
#define QUEUE_SIZE 10
#define ROOT_DIR "./www"
#define UPLOAD_DIR "./upload"

typedef struct {
    int client_socket;
} Task;

Task task_queue[QUEUE_SIZE];
int queue_front = 0, queue_rear = 0, queue_count = 0;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t queue_not_full = PTHREAD_COND_INITIALIZER;
FILE *log_file;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;
long total_requests = 0;
double total_response_time = 0;
long active_connections = 0;

void log_message(const char *message) {
    pthread_mutex_lock(&log_mutex);
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(log_file, "[%02d-%02d-%04d %02d:%02d:%02d] %s\n",
            t->tm_mday, t->tm_mon + 1, t->tm_year + 1900,
            t->tm_hour, t->tm_min, t->tm_sec, message);
    fflush(log_file);
    pthread_mutex_unlock(&log_mutex);
}

void enqueue_task(Task task) {
    pthread_mutex_lock(&queue_mutex);
    while (queue_count == QUEUE_SIZE) {
        pthread_cond_wait(&queue_not_full, &queue_mutex);
    }
    task_queue[queue_rear] = task;
    queue_rear = (queue_rear + 1) % QUEUE_SIZE;
    queue_count++;
    pthread_cond_signal(&queue_not_empty);
    pthread_mutex_unlock(&queue_mutex);
}

Task dequeue_task() {
    pthread_mutex_lock(&queue_mutex);
    while (queue_count == 0) {
        pthread_cond_wait(&queue_not_empty, &queue_mutex);
    }
    Task task = task_queue[queue_front];
    queue_front = (queue_front + 1) % QUEUE_SIZE;
    queue_count--;
    pthread_cond_signal(&queue_not_full);
    pthread_mutex_unlock(&queue_mutex);
    return task;
}

void get_http_method(const char *request, char *method) {
    sscanf(request, "%s", method);
}

void handle_post_request(const char *request, int client_socket) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        log_message("400 Bad Request: POST request missing body.");
        const char *response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        write(client_socket, response, strlen(response));
        return;
    }
    body += 4;

    const char *content_length_str = strstr(request, "Content-Length: ");
    if (!content_length_str) {
        log_message("411 Length Required: POST request missing Content-Length header.");
        const char *response = "HTTP/1.1 411 Length Required\r\nContent-Length: 0\r\n\r\n";
        write(client_socket, response, strlen(response));
        return;
    }
    size_t content_length = atoi(content_length_str + 16);

    const char *filename_str = strstr(request, "X-Filename: ");
    char filename[256] = "uploaded_file";
    if (filename_str) {
        sscanf(filename_str + 12, "%255[^\r\n]", filename);
    }

    char filepath[BUFFER_SIZE];
    snprintf(filepath, sizeof(filepath), "%s/%s", UPLOAD_DIR, filename);

    FILE *file = fopen(filepath, "wb");
if (!file) {
    log_message("500 Internal Server Error: Failed to create upload file.");
    const char *response = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
    write(client_socket, response, strlen(response));
    return;
}

size_t total_written = 0;
size_t bytes_to_write = content_length;
while (total_written < content_length) {
    size_t bytes_written = fwrite(body + total_written, 1, bytes_to_write, file);
    if (bytes_written == 0) {
        if (ferror(file)) {
            log_message("500 Internal Server Error: Failed to write file.");
            fclose(file);
            const char *response = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
            write(client_socket, response, strlen(response));
            return;
        }
        break;
    }
    total_written += bytes_written;
    bytes_to_write -= bytes_written;
}

fclose(file);

if (total_written != content_length) {
    log_message("500 Internal Server Error: Failed to write complete file.");
    const char *response = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
    write(client_socket, response, strlen(response));
    return;
}
}

void get_requested_file(const char *request, char *filepath) {
    char url[BUFFER_SIZE];
    sscanf(request, "GET %s HTTP/1.1", url);
    snprintf(filepath, BUFFER_SIZE, "%s%s", ROOT_DIR, url);

    char *real_path = realpath(filepath, NULL);
    if (real_path) {
        if (strncmp(real_path, ROOT_DIR, strlen(ROOT_DIR)) != 0) {
            snprintf(filepath, BUFFER_SIZE, "%s/404.html", ROOT_DIR);
        }
        free(real_path);
    } else {
        snprintf(filepath, BUFFER_SIZE, "%s/404.html", ROOT_DIR);
    }
}

void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];
    char filepath[BUFFER_SIZE];
    struct stat file_stat;

    pthread_mutex_lock(&stats_mutex);
    active_connections++;
    pthread_mutex_unlock(&stats_mutex);

    int bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        close(client_socket);
        pthread_mutex_lock(&stats_mutex);
        active_connections--;
        pthread_mutex_unlock(&stats_mutex);
        return;
    }
    buffer[bytes_read] = '\0';

    char method[16];
    get_http_method(buffer, method);

    if (strcmp(method, "POST") == 0) {
        handle_post_request(buffer, client_socket);
    } else {
        get_requested_file(buffer, filepath);
        if (stat(filepath, &file_stat) == -1 || S_ISDIR(file_stat.st_mode)) {
            const char *response = "HTTP/1.1 404 Not Found\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: 13\r\n"
                                   "\r\n"
                                   "404 Not Found";
            write(client_socket, response, strlen(response));
            log_message("404 Not Found: File not found");
        } else {
            FILE *file = fopen(filepath, "rb");
            if (!file) {
                const char *response = "HTTP/1.1 500 Internal Server Error\r\n"
                                       "Content-Type: text/plain\r\n"
                                       "Content-Length: 25\r\n"
                                       "\r\n"
                                       "500 Internal Server Error";
                write(client_socket, response, strlen(response));
                log_message("500 Internal Server Error: File open failed");
            } else {
                const char *header = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: text/html\r\n"
                                     "\r\n";
                write(client_socket, header, strlen(header));
                char file_buffer[BUFFER_SIZE];
                size_t bytes;
                while ((bytes = fread(file_buffer, 1, sizeof(file_buffer), file)) > 0) {
                    write(client_socket, file_buffer, bytes);
                }
                fclose(file);
                log_message("200 OK: File served successfully");
            }
        }
    }

    close(client_socket);
    pthread_mutex_lock(&stats_mutex);
    active_connections--;
    pthread_mutex_unlock(&stats_mutex);
}

void *worker_thread(void *arg) {
    while (1) {
        Task task = dequeue_task();
        handle_client(task.client_socket);
    }
    return NULL;
}

int main() {
    mkdir(ROOT_DIR, 0755);
    mkdir(UPLOAD_DIR, 0755);

    int server_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    log_file = fopen("server.log", "a");
    if (!log_file) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Socket creation failed");
        log_message("Error: Failed to create socket.");
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("Bind failed");
        log_message("Error: Failed to bind socket.");
        close(server_socket);
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, 5) == -1) {
        perror("Listen failed");
        log_message("Error: Failed to listen on socket.");
        close(server_socket);
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    printf("Server is running. Waiting for connections on port %d...\n", PORT);

    pthread_t thread_pool[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_create(&thread_pool[i], NULL, worker_thread, NULL);
    }

    while (1) {
        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket == -1) {
            perror("Accept failed");
        log_message("Error: Failed to accept connection.");
        continue;
    }
    Task task = {client_socket};
    enqueue_task(task);
}

close(server_socket);
fclose(log_file);
return 0;
}