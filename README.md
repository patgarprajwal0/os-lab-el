# Banking System

A small POSIX C client/server project demonstrating TCP sockets, process creation with `fork()`, System V shared memory, process-shared mutexes, and signal-based server shutdown. Account data is stored in shared memory while the server is running; it is cleared when the server stops.

## Build on Ubuntu

Install GCC and the POSIX development tools if needed:

```bash
sudo apt update
sudo apt install build-essential
```

From the project directory, compile both programs:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic bankserver.c -o bankserver -pthread
gcc -std=c11 -Wall -Wextra -Wpedantic bankclient.c -o bankclient
```

## Run

In the first terminal:

```bash
./bankserver
```

In a second terminal, change to the same project directory and run:

```bash
./bankclient localhost
```

Available client commands:

```text
open <name>
start <name>
credit <amount>
debit <amount>
balance
finish
exit
```

Press Ctrl+C in the server terminal to stop the server. The server listens on TCP port 1080. If that port is already in use, stop the other server or change `SERVER_PORT` in `banksystem.h`.
