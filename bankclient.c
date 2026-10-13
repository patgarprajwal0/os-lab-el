#define _POSIX_C_SOURCE 200809L
#include "banksystem.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int socket_read_line(int socket_fd, char *buffer, size_t capacity) {
    size_t used = 0;
    if (capacity < 2) return -1;
    for (;;) {
        char ch;
        ssize_t received = recv(socket_fd, &ch, 1, 0);
        if (received == 0) return 0;
        if (received < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ch == '\n') {
            if (used > 0 && buffer[used - 1] == '\r') --used;
            buffer[used] = '\0';
            return 1;
        }
        if (used + 1 >= capacity) return -1;
        buffer[used++] = ch;
    }
}

int socket_write_line(int socket_fd, const char *message) {
    size_t length = strlen(message);
    size_t sent = 0;
    char buffer[MAX_INPUT_SIZE + 2];
    if (length > MAX_INPUT_SIZE) return -1;
    memcpy(buffer, message, length);
    buffer[length++] = '\n';
    while (sent < length) {
        ssize_t result = send(socket_fd, buffer + sent, length - sent, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) return -1;
        sent += (size_t)result;
    }
    return 0;
}

static int connect_to_server(const char *hostname) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    char port[16];
    int socket_fd = -1;
    int result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%d", SERVER_PORT);
    result = getaddrinfo(hostname, port, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "Could not resolve host %s: %s\n", hostname, gai_strerror(result));
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd < 0) continue;
        if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) break;
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(addresses);
    if (socket_fd < 0) perror("Could not connect to bank server");
    return socket_fd;
}

int main(int argc, char **argv) {
    char *line = NULL;
    size_t capacity = 0;
    char response[MAX_INPUT_SIZE + 1];
    int socket_fd;
    int done = 0;
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <hostname>\nExample: %s localhost\n", argv[0], argv[0]);
        return 1;
    }
    socket_fd = connect_to_server(argv[1]);
    if (socket_fd < 0) return 1;
    printf("Connected to %s on port %d.\n", argv[1], SERVER_PORT);
    puts("Commands: open <name>, start <name>, credit <amount>, debit <amount>, balance, finish, exit");
    while (!done) {
        ssize_t length;
        printf("client> ");
        fflush(stdout);
        length = getline(&line, &capacity, stdin);
        if (length < 0) {
            if (socket_write_line(socket_fd, "exit") == 0 && socket_read_line(socket_fd, response, sizeof(response)) == 1)
                printf("server> %s\n", response);
            break;
        }
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
        if (length == 0) continue;
        if (length > MAX_INPUT_SIZE) {
            fprintf(stderr, "Command is too long (maximum %d characters).\n", MAX_INPUT_SIZE);
            continue;
        }
        if (socket_write_line(socket_fd, line) != 0) {
            fprintf(stderr, "Could not send command; server may have disconnected.\n");
            break;
        }
        if (socket_read_line(socket_fd, response, sizeof(response)) != 1) {
            fprintf(stderr, "Server disconnected.\n");
            break;
        }
        printf("server> %s\n", response);
        if (strcmp(line, "exit") == 0) done = 1;
    }
    free(line);
    close(socket_fd);
    return 0;
}
