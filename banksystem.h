#ifndef BankSystem
#define BankSystem

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

#define ServerPort 1080
#define AccountNameSize 100
#define MaxAccounts 20
#define commandThrottle 1
#define connectRetryInterval 3
#define commandLen 7
#define MaxSessions 20
#define SessionTimeout 20
#define MaxInputSize 256

char msgDelimiter = '\n';

struct account {
    char name[AccountNameSize];
    float balance;
    int inSession;
    pthread_mutex_t mutex;
};

struct bank {
    struct account accounts[MaxAccounts];
    int accountCounter;
    pthread_mutex_t mutex;
};

int socketWrite(int socket, char *socketBuffer, int bufferLen) {
    socketBuffer[bufferLen++] = msgDelimiter;
    if (send(socket, socketBuffer, bufferLen, 0) != bufferLen) {
        perror("socketWrite: Unable to send message to client");
        bufferLen = 0;
    }
    return bufferLen;
}

int socketRead(int socket, char *socketBuffer, int bufferLen) {
    int idx1 = 0;
    while ((recv(socket, &socketBuffer[idx1], 1, 0) == 1) && (idx1 < bufferLen)) {
        if (socketBuffer[idx1++] == msgDelimiter)
            break;
    }
    socketBuffer[--idx1] = '\0';
    return idx1;
}

#endif
