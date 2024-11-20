#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define PORTNUM 9100        // 서버 포트 번호
#define MAX_CLIENTS 100     // 최대 클라이언트 수
#define NUM_SERVERS 3       // 서버 개수

// 서버 정보를 저장하는 구조체
typedef struct {
    char ip[16];  // 서버 IP 주소
    int port;     // 서버 포트 번호
} server_info;

// 웹 서버 목록 정의
server_info web_servers[] = {
    {"127.0.0.1", PORTNUM}, // 첫 번째 서버
    {"127.0.0.1", PORTNUM + 1}, // 두 번째 서버
    {"127.0.0.1", PORTNUM + 2}  // 세 번째 서버
};

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

// 클라이언트 요청을 처리하는 함수
void* handle_client(void* client_sock) {
    int client_socket = *(int*)client_sock;        // 클라이언트 소켓 가져오기
    free(client_sock);                             // 동적 할당된 메모리 해제

    char client_ip[16];                            // 클라이언트 IP 주소 저장용
    struct sockaddr_in addr;                       // 클라이언트 주소 정보
    socklen_t addr_len = sizeof(addr);             // 클라이언트 주소 길이
    if (getpeername(client_socket, (struct sockaddr*)&addr, &addr_len) == 0) {
        strcpy(client_ip, inet_ntoa(addr.sin_addr)); // 클라이언트 IP 주소 추출
    } else {
        strcpy(client_ip, "Unknown");             // IP 주소를 가져오지 못한 경우
    }

    int server_index = load_balance(client_ip);    // 라운드 로빈으로 서버 선택
    server_info selected_server = web_servers[server_index]; // 선택된 서버 정보 가져오기

    int server_socket;                             // 서버 소켓
    struct sockaddr_in server_addr;                // 서버 주소 정보
    server_socket = socket(AF_INET, SOCK_STREAM, 0); // 서버와의 연결을 위한 소켓 생성
    if (server_socket == -1) {
        perror("Socket creation failed for server"); // 소켓 생성 실패 시 오류 출력
        close(client_socket);                      // 클라이언트 소켓 닫기
        return NULL;
    }

    memset(&server_addr, 0, sizeof(server_addr));  // 서버 주소 구조체 초기화
    server_addr.sin_family = AF_INET;             // 주소 체계: IPv4
    server_addr.sin_port = htons(selected_server.port); // 선택된 서버의 포트 설정
    inet_pton(AF_INET, selected_server.ip, &server_addr.sin_addr); // 선택된 서버의 IP 설정

    if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection to server failed");     // 서버 연결 실패 시 오류 출력
        close(client_socket);                      // 클라이언트 소켓 닫기
        close(server_socket);                      // 서버 소켓 닫기
        return NULL;
    }

    char buffer[1024];                             // 데이터 버퍼
    int bytes_received = recv(client_socket, buffer, sizeof(buffer), 0); // 클라이언트로부터 데이터 수신
    if (bytes_received > 0) {
        send(server_socket, buffer, bytes_received, 0); // 수신된 데이터를 서버로 전달
    } else if (bytes_received < 0) {
        perror("Error receiving from client");     // 데이터 수신 오류 출력
        close(client_socket);                      // 클라이언트 소켓 닫기
        close(server_socket);                      // 서버 소켓 닫기
        return NULL;
    }

    bytes_received = recv(server_socket, buffer, sizeof(buffer), 0); // 서버로부터 데이터 수신
    if (bytes_received > 0) {
        send(client_socket, buffer, bytes_received, 0); // 수신된 데이터를 클라이언트로 전달
    } else if (bytes_received < 0) {
        perror("Error receiving from server");      // 데이터 수신 오류 출력
    }

    close(client_socket);                           // 클라이언트 소켓 닫기
    close(server_socket);                           // 서버 소켓 닫기
    return NULL;
}

int main() {
    int server_socket, *new_sock;                   // 서버 소켓 및 클라이언트 소켓 저장용 포인터
    struct sockaddr_in server_addr, client_addr;    // 서버 및 클라이언트 주소 구조체
    socklen_t client_addr_len = sizeof(client_addr); // 클라이언트 주소 길이

    server_socket = socket(AF_INET, SOCK_STREAM, 0); // 서버 소켓 생성
    if (server_socket < 0) {
        perror("Socket creation failed");           // 소켓 생성 실패 시 오류 출력
        return -1;
    }

    server_addr.sin_family = AF_INET;               // 주소 체계: IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;       // 모든 네트워크 인터페이스에서 수신
    server_addr.sin_port = htons(8080);             // 포트 번호 설정

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");                      // 바인딩 실패 시 오류 출력
        return -1;
    }

    listen(server_socket, MAX_CLIENTS);             // 클라이언트 연결 대기
    printf("Load balancer listening on port 8080...\n"); // 로드 밸런서 시작 메시지 출력

    while (1) {
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len); // 클라이언트 연결 수락
        if (client_socket < 0) {
            perror("Accept failed");                // 연결 수락 실패 시 오류 출력
            continue;
        }

        pthread_t tid;                              // 새 스레드 식별자
        new_sock = malloc(sizeof(int));             // 새 클라이언트 소켓 저장용 메모리 할당
        *new_sock = client_socket;                  // 클라이언트 소켓 복사
        if (pthread_create(&tid, NULL, handle_client, (void*)new_sock) != 0) {
            perror("Thread creation failed");       // 스레드 생성 실패 시 오류 출력
            close(client_socket);                   // 클라이언트 소켓 닫기
            free(new_sock);                         // 메모리 해제
        }
    }

    close(server_socket);                           // 서버 소켓 닫기
    return 0;
}
