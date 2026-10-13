#include "banksystem.hpp"

int main(int argc, char* argv[]) {
    if (argc != 2) { std::cerr << "Usage: bankclient <hostname>\nExample: bankclient localhost\n"; return 1; }
    if (!startSockets()) { std::cerr << "Could not initialize sockets\n"; return 1; }
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    int lookup = getaddrinfo(argv[1], "1080", &hints, &result);
    if (lookup != 0) { std::cerr << "Could not resolve host " << argv[1] << '\n'; stopSockets(); return 1; }
    Socket connection = InvalidSocket;
    for (addrinfo* p = result; p; p = p->ai_next) {
        connection = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (connection != InvalidSocket && connect(connection, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
        if (connection != InvalidSocket) closeSocket(connection);
        connection = InvalidSocket;
    }
    freeaddrinfo(result);
    if (connection == InvalidSocket) { std::cerr << "Could not connect to " << argv[1] << ":1080. Is the server running?\n"; stopSockets(); return 1; }
    std::cout << "Connected to " << argv[1] << " on port 1080.\nCommands: open <name>, start <name>, credit <amount>, debit <amount>, balance, finish, exit\n";
    std::string line, response;
    while (std::cout << "client> " && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (!sendLine(connection, line)) { std::cerr << "Connection lost.\n"; break; }
        if (!receiveLine(connection, response)) { std::cerr << "Server disconnected.\n"; break; }
        std::cout << "server> " << response << '\n';
        if (line == "exit") break;
    }
    closeSocket(connection); stopSockets();
    return 0;
}
