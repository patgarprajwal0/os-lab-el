#ifndef BANKSYSTEM_H
#define BANKSYSTEM_H

#include <pthread.h>
#include <stddef.h>
#include <sys/types.h>

#define SERVER_PORT 1080
#define MAX_ACCOUNTS 20
#define MAX_SESSIONS 20
#define ACCOUNT_NAME_SIZE 100
#define MAX_INPUT_SIZE 256

typedef struct {
    char name[ACCOUNT_NAME_SIZE];
    double balance;
    pid_t owner_pid; /* 0 means no client currently owns the account session. */
} Account;

typedef struct {
    Account accounts[MAX_ACCOUNTS];
    size_t account_count;
    pthread_mutex_t mutex; /* Initialized as process-shared by the server. */
} Bank;

/* Read one newline-terminated message. Returns 1 on success, 0 on disconnect, -1 on error. */
int socket_read_line(int socket_fd, char *buffer, size_t capacity);

/* Send an entire newline-terminated message. Returns 0 on success, -1 on error. */
int socket_write_line(int socket_fd, const char *message);

#endif
