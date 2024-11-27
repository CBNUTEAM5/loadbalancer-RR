#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define LISTENPORT 8080
#define PORTNUM 9100
#define MAX_CLIENTS 100
#define NUM_SERVERS 2
#define QUEUE_SIZE 20
#define BUFFER_SIZE 4096

typedef struct {
    char ip[16];
    int port;
} server_info;

typedef struct {
    int client_socket;
} client_request;

typedef struct {
    client_request requests[QUEUE_SIZE];
    int front, rear, count;
    pthread_mutex_t mutex;
    pthread_cond_t cond_non_empty;
    pthread_cond_t cond_non_full;
} request_queue;

typedef struct {
    char key[256];
    char value[BUFFER_SIZE];
} CacheEntry;

server_info web_servers[] = {
    {"10.198.138.212", PORTNUM},
    {"10.198.138.213", PORTNUM}
};

request_queue queue = {
    .front = 0,
    .rear = 0,
    .count = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond_non_empty = PTHREAD_COND_INITIALIZER,
    .cond_non_full = PTHREAD_COND_INITIALIZER
};


CacheEntry cache[5]; 
int cache_count = 0;
pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

// 라운드 로빈을 위한 전역 변수
int current_server_index = 0;  // 현재 선택된 서버의 인덱스
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; // 동기화를 위한 mutex

// 라운드 로빈 방식으로 서버 선택
int load_balance(char* client_ip) {
    pthread_mutex_lock(&lock);                     // 임계 구역 시작: mutex 잠금
    int server_index = current_server_index;       // 현재 서버 인덱스를 저장
    current_server_index = (current_server_index + 1) % NUM_SERVERS; // 다음 서버로 이동
    pthread_mutex_unlock(&lock);                   // 임계 구역 종료: mutex 해제
    return server_index;                           // 선택된 서버 인덱스 반환
}

void enqueue(int client_socket) {
    pthread_mutex_lock(&queue.mutex);
    while (queue.count == QUEUE_SIZE) {
        pthread_cond_wait(&queue.cond_non_full, &queue.mutex);
    }
    queue.requests[queue.rear].client_socket = client_socket;
    queue.rear = (queue.rear + 1) % QUEUE_SIZE;
    queue.count++;
    pthread_cond_signal(&queue.cond_non_empty);
    pthread_mutex_unlock(&queue.mutex);
}

int dequeue() {
    pthread_mutex_lock(&queue.mutex);
    while (queue.count == 0) {
        pthread_cond_wait(&queue.cond_non_empty, &queue.mutex);
    }
    int client_socket = queue.requests[queue.front].client_socket;
    queue.front = (queue.front + 1) % QUEUE_SIZE;
    queue.count--;
    pthread_cond_signal(&queue.cond_non_full);
    pthread_mutex_unlock(&queue.mutex);
    return client_socket;
}

int check_cache(char* key, char* value) {
    pthread_mutex_lock(&cache_lock);
    for (int i = 0; i < cache_count; i++) {
        if (strcmp(cache[i].key, key) == 0) {
            strcpy(value, cache[i].value);
            pthread_mutex_unlock(&cache_lock);
            return 1;         
        }
    }
    pthread_mutex_unlock(&cache_lock);
    return 0; 
}

void update_cache(char* key, char* value) {
    pthread_mutex_lock(&cache_lock);
    if (cache_count < 10) {
        strcpy(cache[cache_count].key, key);
        strcpy(cache[cache_count].value, value);
        cache_count++;
    }
    else {

        //처음 캐시 대체

        strcpy(cache[0].key, key);
        strcpy(cache[0].value, value);
    }
    pthread_mutex_unlock(&cache_lock);
}

void* handle_client(void* arg) {
    while (1) {
        int client_socket = dequeue();

        char client_ip[16];
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        if (getpeername(client_socket, (struct sockaddr*)&addr, &addr_len) == 0) {
            strcpy(client_ip, inet_ntoa(addr.sin_addr));
        }
        else {
            strcpy(client_ip, "Unknown");
        }

        char buffer[BUFFER_SIZE];
        memset(buffer, 0, BUFFER_SIZE);

        // 요청 읽기
        int bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) {
            close(client_socket);
            continue;
        }

        char cache_key[256];
        sscanf(buffer, "GET %s HTTP/1.1", cache_key);

        char cache_value[BUFFER_SIZE];
        if (check_cache(cache_key, cache_value)) {
            //hit

            send(client_socket, cache_value, strlen(cache_value), 0);
        }
        else {
            //miss 

            int server_index = load_balance(client_ip);
            server_info selected_server = web_servers[server_index];

            int server_socket;
            struct sockaddr_in server_addr;
            server_socket = socket(AF_INET, SOCK_STREAM, 0);
            if (server_socket == -1) {
                perror("Socket creation failed for server");
                close(client_socket);
                continue;
            }
            memset(&server_addr, 0, sizeof(server_addr));
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(selected_server.port);
            inet_pton(AF_INET, selected_server.ip, &server_addr.sin_addr);

            if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                perror("connect");
                close(client_socket);
                close(server_socket);
                continue;
            }

            send(server_socket, buffer, bytes_received, 0);
            int server_response = recv(server_socket, buffer, sizeof(buffer), 0);
            if (server_response > 0) {
                send(client_socket, buffer, server_response, 0);
                update_cache(cache_key, buffer); 
            }
            close(server_socket);
        }
        close(client_socket);
    }
    return NULL;
}

int main() {
    int server_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Socket creation failed");
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(LISTENPORT);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        return -1;
    }

    listen(server_socket, MAX_CLIENTS);

    pthread_t tids[5];
    for (int i = 0; i < 5; i++) {
        pthread_create(&tids[i], NULL, handle_client, NULL);
    }

    while (1) {
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            perror("accept");
            continue;
        }
        enqueue(client_socket);
    }

    close(server_socket);
    return 0;
}
