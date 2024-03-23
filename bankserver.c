#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "bankSystem.h"

static pid_t createChildProcess(void (func)(int), int opt);
static int allocateMEM();
static void deallocateMEM(int statusCode);
static int createSocket(const unsigned short port);
static void socketService(int clientID);
static void sessionSignalHandler(int SIGNAL);
static void sessionHandler(int port);
static void displayAccountStatus(int SIGNAL);
static void statusHandler(int timeout);
static void sig_handler(int SIGNAL);

static int sharedMemID = -1;
static int serverSocket = -1;
static pid_t childPID[MaxSessions+2];
static int serverPID[MaxSessions+2];
static struct bank *bank = NULL;

static pid_t createChildProcess(void (func)(int), int opt) {
    pid_t PID = fork();
    if (PID == 0) {
        func(opt);
    } else if (PID == -1) {
        perror("Server> Unable to create a process");
        exit(1);
    } else {
        printf("Server> Created child process PID %u\n", PID);
    }
    return PID;
}

static int allocateMEM() {
    key_t key = ftok("/", 6);
    if (key == -1) {
        perror("Unable to create identifier");
        return 1;
    }
    sharedMemID = shmget(key, sizeof(struct bank), IPC_CREAT | IPC_EXCL | S_IRWXU | S_IRWXG | S_IROTH);
    if (sharedMemID == -1) {
        perror("Unable to allocate shared memory (-1)");
        return 1;
    }
    bank = (struct bank*) shmat(sharedMemID, NULL, SHM_RND);
    if (bank == (void*) -1) {
        perror("Unable to attach share memory");
        return 1;
    }
    int idx1;
    for (idx1 = 0; idx1 < MaxAccounts; idx1++) {
        pthread_mutex_init(&bank->accounts[idx1].mutex, NULL);
    }
    bank->accountCounter = 0;
    if (pthread_mutex_init(&bank->mutex, NULL) != 0) {
        perror("Unable to initialize the thread");
        return 1;
    }
    memset(childPID, 0, sizeof(childPID));
    return 0;
}

static void deallocateMEM(int statusCode) {
    int idx1;
    if (bank == NULL)
        exit(statusCode);
    pthread_mutex_destroy(&bank->mutex);
    for (idx1 = 0; idx1 < MaxAccounts; idx1++)
        pthread_mutex_destroy(&bank->accounts[idx1].mutex);
    if (shmdt(bank) == -1)
        perror("Unable to detach shared memory");
    shmctl(sharedMemID, IPC_RMID, NULL);
    exit(statusCode);
}

static int createSocket(const unsigned short port) {
    struct sockaddr_in childSocket;
    int opt = 1;
    memset(&childSocket, 0, sizeof(childSocket));
    childSocket.sin_family = AF_INET;
    childSocket.sin_addr.s_addr = INADDR_ANY;
    childSocket.sin_port = htons(port);
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) == -1) {
        perror("setsockopt");
        return 1;
    }
    if (bind(serverSocket, (struct sockaddr *)&childSocket, sizeof childSocket) == -1) {
        perror("bind");
        return 1;
    }
    if (listen(serverSocket, 10) == -1) {
        perror("listen");
        return 1;
    }
    return 0;
}

static void socketService(int clientID) {
    int inService = 1;
    char line[MaxInputSize+commandLen], *command, *value;
    int clientIdx = -1;
    int inputSize, idx1;
    
    while (inService) {
        inputSize = socketRead(clientID, line, sizeof(line));
        command = strtok(line, " ");
        value = strtok(NULL, "\n");
        
        if (strcmp(command, "open") == 0) {
            if (clientIdx != -1) {
                inputSize = snprintf(line, MaxInputSize, "Command Error: Session is already opened");
            } else if (value == NULL) {
                inputSize = snprintf(line, MaxInputSize, "Command Error: Please provide a name");
            } else {
                if (bank->accountCounter >= (MaxAccounts-1)) {
                    inputSize = snprintf(line, MaxInputSize, "Command Error: Unable to allocate resources");
                } else {
                    for (idx1 = 0; idx1 < bank->accountCounter; idx1++) {
                        if (strcmp(bank->accounts[idx1].name, value) == 0) {
                            inputSize = snprintf(line, MaxInputSize, "Command Error: Account name already exists. Try start command");
                            break;
                        }
                    }
                    if (idx1 == bank->accountCounter) {
                        if (pthread_mutex_lock(&bank->mutex) != 0) {
                            perror("pthread_mutex_lock ERROR");
                        }
                        clientIdx = bank->accountCounter++;
                        strcpy(bank->accounts[clientIdx].name, value);
                        bank->accounts[clientIdx].balance = 0.0;
                        bank->accounts[clientIdx].inSession = 1;
                        pthread_mutex_lock(&bank->accounts[clientIdx].mutex);
                        if (pthread_mutex_unlock(&bank->mutex) != 0) {
                            perror("pthread_mutex_unlock ERROR");
                        }
                        inputSize = snprintf(line, MaxInputSize, "Account successfully created - %s", bank->accounts[clientIdx].name);
                    }
                }
            }
        } else if (strcmp(command, "start") == 0) {
            if (clientIdx != -1) {
                inputSize = snprintf(line, MaxInputSize, "Command Error: Session is already opened");
            } else if (value == NULL) {
                inputSize = snprintf(line, MaxInputSize, "Command Error: Please provide an account name");
            } else {
                int accountIsLocked = 0;
                if (pthread_mutex_lock(&bank->mutex) != 0) {
                    perror("pthread_mutex_lock ERROR");
                }
                for (idx1 = 0; idx1 < bank->accountCounter; idx1++) {
                    if (strcmp(bank->accounts[idx1].name, value) == 0) {
                        clientIdx = idx1;
                        accountIsLocked = pthread_mutex_trylock(&bank->accounts[idx1].mutex); /* try to lock */
                        break;
                    }
                }
                if (pthread_mutex_unlock(&bank->mutex) != 0) {
                    perror("pthread_mutex_unlock ERROR");
                }
                if (clientIdx == -1) {
                    inputSize = snprintf(line, MaxInputSize, "Account not found");
                } else if (accountIsLocked) {
                    inputSize = snprintf(line, MaxInputSize, "Account is locked by another user");
                } else {
                    bank->accounts[clientIdx].inSession = 1;
                    inputSize = snprintf(line, MaxInputSize, "%s session reopened", bank->accounts[idx1].name);
                }
            }
        } else if (strcmp(command, "credit") == 0) {
            if (clientIdx == -1) {
                inputSize = snprintf(line, MaxInputSize, "Start or open an account before using credit function");
            } else if (value == NULL) {
                inputSize = snprintf(line, MaxInputSize, "Please enter an amount to credit");
            } else {
                bank->accounts[clientIdx].balance += atof(value);
                float balance = bank->accounts[clientIdx].balance;
                inputSize = snprintf(line, MaxInputSize, "%s new balance: %.2f", bank->accounts[clientIdx].name, balance);
            }
        } else if (strcmp(command, "debit") == 0) {
            if (clientIdx == -1) {
                inputSize = snprintf(line, MaxInputSize, "Start or open an account before using debit function");
            } else if (value == NULL) {
                inputSize = snprintf(line, MaxInputSize, "Please enter an amount to credit");
            } else {
                float amount_f = atof(value);
                if (amount_f > bank->accounts[clientIdx].balance) {
                    inputSize = snprintf(line, MaxInputSize, "Overdraft is not allowed for this account");
                } else {
                    bank->accounts[clientIdx].balance -= amount_f;
                    float balance = bank->accounts[clientIdx].balance;
                    inputSize = snprintf(line, MaxInputSize, "%s new balance: %.2f", bank->accounts[clientIdx].name, balance);
                }
            }
        } else if (strcmp(command, "balance") == 0) {
            if (clientIdx == -1) {
                inputSize = snprintf(line, MaxInputSize, "Start or open an account before using balance function");
            } else {
                inputSize = snprintf(line, MaxInputSize, "%s current balance: %.2f", bank->accounts[clientIdx].name, bank->accounts[clientIdx].balance);
            }
        } else if (strcmp(command, "finish") == 0) {
            if (clientIdx == -1) {
                inputSize = snprintf(line, MaxInputSize, "No open account to finish");
            } else {
                bank->accounts[clientIdx].inSession = 0;
                inputSize = snprintf(line, MaxInputSize, "%s transactions finished", bank->accounts[clientIdx].name);
                if (pthread_mutex_unlock(&bank->accounts[clientIdx].mutex) != 0) {
                    perror("pthread_mutex_unlock ERROR");
                }
                clientIdx = -1;
            }
        } else if (strcmp(command, "exit") == 0) {
            inputSize = snprintf(line, MaxInputSize, "Have a good day");
            inService = 0;
            bank->accounts[clientIdx].inSession = 0;
            if (pthread_mutex_unlock(&bank->accounts[clientIdx].mutex) != 0) {
                perror("pthread_mutex_unlock ERROR");
            }
            printf("User %i disconnect request granted\n", clientID);
        } else if (strcmp(command, "SIGINT") == 0) {
            int idx1, status;
            pid_t processID = getpid();
            printf("Abnormal Exit Signal From Client Process %d...\n", processID);
            for (idx1 = 0; idx1 < MaxSessions+2; idx1++) {
                if (childPID[idx1] == processID) {
                    printf("Freeing Session and Mutex Locks For Child Process...\n");
                    bank->accounts[idx1].inSession = 0;
                if(pthread_mutex_unlock(&bank->accounts[idx1].mutex) != 0)
                { perror("pthread_mutex_unlock ERROR"); }
                }
                }
                kill(childPID[idx1], SIGTERM);
                waitpid(childPID[idx1], &status, 0);
                childPID[idx1] = 0;
                deallocateMEM(0);
                }
                else
                {
                inputSize = snprintf(line, MaxInputSize, "I don't understand that
                command!!!");
                }
            if(socketWrite(clientID, line, inputSize) == 0) snprintf(line,
            MaxInputSize, "Do you need help?");
            }
            if(clientIdx == -1)
            { 
            printf ("In-SESSION = %i", bank->accounts[clientIdx].inSession);
            printf("Client disconnected for unknown reason\n");
            }
    shutdown(clientID, SHUT_RDWR);
    close(clientID);
}

static void sessionSignalHandler(int SIGNAL) {
    int idx1 = 0;
    char *end = "TERMINATE";
    puts("Terminating Session Handler Child Processes...");
    if (SIGNAL == SIGTERM) {
        for (; idx1 < MaxSessions+2; idx1++) {
            printf("Terminating Child Process %d\n", childPID[idx1]);
            kill(childPID[idx1], SIGTERM);
            waitpid(childPID[idx1], 0, 0);
        }
    }
}

static void sessionHandler(int port) {
    struct sockaddr_in childSocket;
    socklen_t socketLen = sizeof(childSocket);
    int idx1, clientIdx;
    signal(SIGCHLD, sessionSignalHandler);
    if (createSocket(port) == 1) {
        fprintf(stderr, "sessionHandler: Unable to setup socket\n");
        exit(1);
    }
    while ((clientIdx = accept(serverSocket, (struct sockaddr *)&childSocket, &socketLen)) > 0) {
        printf("sessionHandler: Client connection from %s, client index %d\n", inet_ntoa(childSocket.sin_addr), clientIdx);
        for (idx1 = 0; idx1 < MaxSessions; idx1++) {
            if (childPID[idx1] == 0) {
                serverPID[idx1] = clientIdx;
                childPID[idx1] = createChildProcess(socketService, clientIdx);
                break;
            }
        }
        if (idx1 == MaxSessions)
            fprintf(stderr, "sessionHandler: Max clients reached\n");
        socketLen = sizeof(childSocket);
    }
    shutdown(serverSocket, SHUT_RDWR);
    close(serverSocket);
}

static void displayAccountStatus(int SIGNAL) {
    int idx1;
    char *header1 = "Name", *header2 = "Balance";
    if (SIGNAL == SIGTERM) {
        puts("Ending Display....");
        return;
    }
    if (pthread_mutex_lock(&bank->mutex) != 0) { 
        perror("pthread_mutex_lock ERROR"); 
    }
    if (bank->accountCounter == 0) {
        puts("Bank Status: No accounts in bank");
    } else {
        printf("%-100s %-12s\n", header1, header2);
        printf("------------------------------------------------------------------------------------------------------------\n");
        for (idx1 = 0; idx1 < bank->accountCounter; idx1++) {
            if (bank->accounts[idx1].inSession) {
                printf("%-100s %.2f\tIN SERVICE\n", bank->accounts[idx1].name, bank->accounts[idx1].balance);
            } else {
                printf("%-100s\t%.2f\n", bank->accounts[idx1].name, bank->accounts[idx1].balance);
            }
        }
    }
    if (pthread_mutex_unlock(&bank->mutex) != 0) { 
        perror("pthread_mutex_unlock ERROR"); 
    }
}

static void statusHandler(int timeout) {
    signal(SIGALRM, displayAccountStatus);
    while (1) {
        alarm(timeout);
        sleep(timeout + 1);
    }
}

static void sig_handler(int SIGNAL) {
    if (SIGNAL == SIGINT) {
        int erre = -1;
        printf("Server> Exit signal detected Process %d\n", erre);
        printf("Server> Terminating Session Handler %d\n", childPID[MaxSessions+1]);
        printf("Server> Terminating Display Handler %d\n", childPID[MaxSessions]);
        kill(childPID[MaxSessions+1], SIGTERM);
        kill(childPID[MaxSessions], SIGTERM);
    }
}

int main(void) {
    int status;
    memset(childPID, 0, sizeof(childPID));
    if (allocateMEM() == 1) 
        deallocateMEM(1);
    childPID[MaxSessions] = createChildProcess(statusHandler, SessionTimeout);
    childPID[MaxSessions+1] = createChildProcess(sessionHandler, ServerPort);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    waitpid(childPID[MaxSessions], &status, 0);
    waitpid(childPID[MaxSessions+1], &status, 0);
    deallocateMEM(0);
    return 0;
}


