#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <libgen.h>

#define SERVER_ADDR "127.0.0.1"
#define SERVER_PORT 8083
#define FILE_PATH "/path/to/your/test_file.txt"

void send_post_request(const char *filepath, const char *server_addr, int server_port) {
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        perror("파일 열기 실패");
        return;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("소켓 생성 실패");
        fclose(file);
        return;
    }

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(server_port);
    server.sin_addr.s_addr = inet_addr(server_addr);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) == -1) {
        perror("서버 연결 실패");
        close(sock);
        fclose(file);
        return;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    char *filename = basename((char *)filepath);
    char request[1024];
    int header_len = snprintf(request, sizeof(request),
             "POST /upload HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Content-Type: application/octet-stream\r\n"
             "Content-Length: %ld\r\n"
             "X-Filename: %s\r\n"
             "\r\n", server_addr, server_port, file_size, filename);

    if (header_len >= sizeof(request)) {
        fprintf(stderr, "헤더가 버퍼 크기를 초과했습니다.\n");
        close(sock);
        fclose(file);
        return;
    }

    if (send(sock, request, header_len, 0) == -1) {
        perror("헤더 전송 실패");
        close(sock);
        fclose(file);
        return;
    }

    char buffer[1024];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (send(sock, buffer, bytes_read, 0) == -1) {
            perror("파일 데이터 전송 실패");
            close(sock);
            fclose(file);
            return;
        }
    }

    char response[1024];
    int bytes_received = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes_received > 0) {
        response[bytes_received] = '\0';
        printf("서버 응답:\n%s\n", response);
    } else {
        perror("서버 응답 수신 실패");
    }

    close(sock);
    fclose(file);
}

int main() {
    send_post_request(FILE_PATH, SERVER_ADDR, SERVER_PORT);
    return 0;
}