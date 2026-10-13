#define _POSIX_C_SOURCE 200809L
#include "banksystem.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stopping = 0;
static int listen_fd = -1;
static int shared_memory_id = -1;
static Bank *bank = NULL;
static int bank_mutex_initialized = 0;
static pid_t workers[MAX_SESSIONS];

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

static void handle_signal(int signal_number) {
    (void)signal_number;
    stopping = 1;
}

static int lock_bank(void) {
    int result = pthread_mutex_lock(&bank->mutex);
    if (result != 0) {
        errno = result;
        perror("pthread_mutex_lock");
        return -1;
    }
    return 0;
}

static void unlock_bank(void) {
    int result = pthread_mutex_unlock(&bank->mutex);
    if (result != 0) {
        errno = result;
        perror("pthread_mutex_unlock");
    }
}

static void release_account(int account_index, pid_t owner) {
    if (account_index < 0 || lock_bank() != 0) return;
    if (bank->accounts[account_index].owner_pid == owner)
        bank->accounts[account_index].owner_pid = 0;
    unlock_bank();
}

static int find_account(const char *name) {
    size_t i;
    for (i = 0; i < bank->account_count; ++i)
        if (strcmp(bank->accounts[i].name, name) == 0) return (int)i;
    return -1;
}

static int parse_amount(const char *text, double *amount) {
    char *end = NULL;
    double value;
    if (text == NULL || *text == '\0') return 0;
    errno = 0;
    value = strtod(text, &end);
    while (end != NULL && (*end == ' ' || *end == '\t')) ++end;
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value) || value < 0)
        return 0;
    *amount = value;
    return 1;
}

static int process_command(const char *line, int *active_account, char *reply, size_t reply_size) {
    char command[32];
    char argument[MAX_INPUT_SIZE + 1];
    const char *cursor = line;
    size_t command_length = 0;
    pid_t self = getpid();
    double amount;

    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    while (cursor[command_length] && cursor[command_length] != ' ' && cursor[command_length] != '\t') {
        if (command_length + 1 >= sizeof(command)) {
            snprintf(reply, reply_size, "Command is too long");
            return 0;
        }
        command[command_length] = cursor[command_length];
        ++command_length;
    }
    command[command_length] = '\0';
    cursor += command_length;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    snprintf(argument, sizeof(argument), "%s", cursor);
    {
        size_t n = strlen(argument);
        while (n > 0 && (argument[n - 1] == ' ' || argument[n - 1] == '\t')) argument[--n] = '\0';
    }

    if (strcmp(command, "exit") == 0) {
        if (*active_account >= 0) release_account(*active_account, self);
        *active_account = -1;
        snprintf(reply, reply_size, "Have a good day");
        return 1;
    }
    if (strcmp(command, "open") == 0) {
        if (*active_account >= 0) snprintf(reply, reply_size, "Command Error: Session is already open");
        else if (argument[0] == '\0') snprintf(reply, reply_size, "Command Error: Please provide a name");
        else if (strlen(argument) >= ACCOUNT_NAME_SIZE) snprintf(reply, reply_size, "Command Error: Name is too long (maximum %d characters)", ACCOUNT_NAME_SIZE - 1);
        else if (lock_bank() != 0) snprintf(reply, reply_size, "Server error: Could not lock bank data");
        else {
            int existing = find_account(argument);
            if (existing >= 0) snprintf(reply, reply_size, "Command Error: Account name already exists; use start");
            else if (bank->account_count >= MAX_ACCOUNTS) snprintf(reply, reply_size, "Command Error: Account limit reached");
            else {
                int index = (int)bank->account_count++;
                snprintf(bank->accounts[index].name, sizeof(bank->accounts[index].name), "%s", argument);
                bank->accounts[index].balance = 0.0;
                bank->accounts[index].owner_pid = self;
                *active_account = index;
                snprintf(reply, reply_size, "Account successfully created - %s", argument);
            }
            unlock_bank();
        }
    } else if (strcmp(command, "start") == 0) {
        if (*active_account >= 0) snprintf(reply, reply_size, "Command Error: Session is already open");
        else if (argument[0] == '\0') snprintf(reply, reply_size, "Command Error: Please provide an account name");
        else if (lock_bank() != 0) snprintf(reply, reply_size, "Server error: Could not lock bank data");
        else {
            int index = find_account(argument);
            if (index < 0) snprintf(reply, reply_size, "Account not found");
            else if (bank->accounts[index].owner_pid != 0 &&
                     (kill(bank->accounts[index].owner_pid, 0) == 0 || errno != ESRCH))
                snprintf(reply, reply_size, "Account is locked by another user");
            else {
                bank->accounts[index].owner_pid = self;
                *active_account = index;
                snprintf(reply, reply_size, "%s session reopened", bank->accounts[index].name);
            }
            unlock_bank();
        }
    } else if (strcmp(command, "credit") == 0 || strcmp(command, "debit") == 0) {
        int is_credit = strcmp(command, "credit") == 0;
        if (*active_account < 0) snprintf(reply, reply_size, "Start or open an account before using this function");
        else if (!parse_amount(argument, &amount)) snprintf(reply, reply_size, "Please enter a valid non-negative amount");
        else if (lock_bank() != 0) snprintf(reply, reply_size, "Server error: Could not lock bank data");
        else {
            Account *account = &bank->accounts[*active_account];
            if (account->owner_pid != self) snprintf(reply, reply_size, "This session no longer owns the account");
            else if (!is_credit && amount > account->balance) snprintf(reply, reply_size, "Overdraft is not allowed for this account");
            else {
                account->balance += is_credit ? amount : -amount;
                snprintf(reply, reply_size, "%s new balance: %.2f", account->name, account->balance);
            }
            unlock_bank();
        }
    } else if (strcmp(command, "balance") == 0) {
        if (*active_account < 0) snprintf(reply, reply_size, "Start or open an account before using balance function");
        else if (lock_bank() != 0) snprintf(reply, reply_size, "Server error: Could not lock bank data");
        else {
            Account *account = &bank->accounts[*active_account];
            if (account->owner_pid != self) snprintf(reply, reply_size, "This session no longer owns the account");
            else snprintf(reply, reply_size, "%s current balance: %.2f", account->name, account->balance);
            unlock_bank();
        }
    } else if (strcmp(command, "finish") == 0) {
        if (*active_account < 0) snprintf(reply, reply_size, "No open account to finish");
        else if (lock_bank() != 0) snprintf(reply, reply_size, "Server error: Could not lock bank data");
        else {
            Account *account = &bank->accounts[*active_account];
            snprintf(reply, reply_size, "%s transactions finished", account->name);
            if (account->owner_pid == self) account->owner_pid = 0;
            *active_account = -1;
            unlock_bank();
        }
    } else {
        snprintf(reply, reply_size, "Unknown command. Use open, start, credit, debit, balance, finish, or exit");
    }
    return 0;
}

static void serve_client(int client_fd) {
    char line[MAX_INPUT_SIZE + 1];
    char reply[MAX_INPUT_SIZE + 1];
    int active_account = -1;
    int read_result;
    pid_t self = getpid();

    for (;;) {
        read_result = socket_read_line(client_fd, line, sizeof(line));
        if (read_result != 1) break;
        if (process_command(line, &active_account, reply, sizeof(reply))) {
            socket_write_line(client_fd, reply);
            break;
        }
        if (socket_write_line(client_fd, reply) != 0) break;
    }
    release_account(active_account, self);
    close(client_fd);
    _exit(0);
}

static void reap_workers(void) {
    pid_t ended;
    while ((ended = waitpid(-1, NULL, WNOHANG)) > 0) {
        size_t i;
        for (i = 0; i < MAX_SESSIONS; ++i)
            if (workers[i] == ended) workers[i] = 0;
    }
}

static int create_shared_bank(void) {
    pthread_mutexattr_t attributes;
    int attributes_initialized = 0;
    int result;
    shared_memory_id = shmget(IPC_PRIVATE, sizeof(Bank), IPC_CREAT | 0600);
    if (shared_memory_id < 0) { perror("shmget"); return -1; }
    bank = (Bank *)shmat(shared_memory_id, NULL, 0);
    if (bank == (void *)-1) { bank = NULL; perror("shmat"); return -1; }
    memset(bank, 0, sizeof(*bank));
    result = pthread_mutexattr_init(&attributes);
    if (result == 0) attributes_initialized = 1;
    if (result == 0) result = pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED);
    if (result == 0) result = pthread_mutex_init(&bank->mutex, &attributes);
    if (result == 0) bank_mutex_initialized = 1;
    if (attributes_initialized) pthread_mutexattr_destroy(&attributes);
    if (result != 0) { errno = result; perror("initializing process-shared mutex"); return -1; }
    return 0;
}

static void destroy_shared_bank(void) {
    if (bank != NULL) {
        if (bank_mutex_initialized) pthread_mutex_destroy(&bank->mutex);
        shmdt(bank);
        bank = NULL;
    }
    if (shared_memory_id >= 0) {
        shmctl(shared_memory_id, IPC_RMID, NULL);
        shared_memory_id = -1;
    }
}

static int open_server_socket(void) {
    struct sockaddr_in address;
    int yes = 1;
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return -1; }
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("setsockopt"); close(listen_fd); listen_fd = -1; return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(SERVER_PORT);
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind (port may already be in use)"); close(listen_fd); listen_fd = -1; return -1;
    }
    if (listen(listen_fd, MAX_SESSIONS) < 0) {
        perror("listen"); close(listen_fd); listen_fd = -1; return -1;
    }
    return 0;
}

int main(void) {
    struct sigaction action;
    size_t i;
    memset(workers, 0, sizeof(workers));
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    if (create_shared_bank() != 0) { destroy_shared_bank(); return 1; }
    if (open_server_socket() != 0) { destroy_shared_bank(); return 1; }
    printf("Bank server listening on port %d. Press Ctrl+C to stop.\n", SERVER_PORT);

    while (!stopping) {
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_length);
        pid_t child;
        int slot = -1;
        if (client_fd < 0) {
            if (errno == EINTR) { reap_workers(); continue; }
            if (stopping) break;
            perror("accept");
            continue;
        }
        reap_workers();
        for (i = 0; i < MAX_SESSIONS; ++i) if (workers[i] == 0) { slot = (int)i; break; }
        if (slot < 0) {
            socket_write_line(client_fd, "Server is at its client limit; try again later");
            close(client_fd);
            continue;
        }
        child = fork();
        if (child < 0) {
            perror("fork"); close(client_fd); continue;
        }
        if (child == 0) {
            close(listen_fd);
            serve_client(client_fd);
        }
        workers[slot] = child;
        close(client_fd);
        printf("Accepted client from %s (worker PID %ld)\n", inet_ntoa(peer.sin_addr), (long)child);
    }

    if (listen_fd >= 0) close(listen_fd);
    for (i = 0; i < MAX_SESSIONS; ++i) if (workers[i] > 0) kill(workers[i], SIGTERM);
    for (i = 0; i < MAX_SESSIONS; ++i) if (workers[i] > 0) waitpid(workers[i], NULL, 0);
    destroy_shared_bank();
    puts("Bank server stopped.");
    return 0;
}
