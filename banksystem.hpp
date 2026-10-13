#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket InvalidSocket = INVALID_SOCKET;
inline void closeSocket(Socket s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket InvalidSocket = -1;
inline void closeSocket(Socket s) { close(s); }
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

constexpr unsigned short ServerPort = 1080;
constexpr std::size_t MaxAccounts = 20;
constexpr std::size_t MaxInputSize = 256;

inline bool sendLine(Socket socket, const std::string& line) {
    std::string data = line + "\n";
    std::size_t sent = 0;
    while (sent < data.size()) {
        int n = send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

inline bool receiveLine(Socket socket, std::string& line) {
    line.clear();
    char c;
    while (line.size() < MaxInputSize) {
        int n = recv(socket, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') return true;
        if (c != '\r') line.push_back(c);
    }
    return true;
}

inline bool startSockets() {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

inline void stopSockets() {
#ifdef _WIN32
    WSACleanup();
#endif
}
