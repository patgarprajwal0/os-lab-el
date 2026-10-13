#include "banksystem.hpp"
#include <csignal>
#include <iomanip>
#include <sstream>

struct Account { std::string name; double balance = 0; bool inSession = false; };
static std::array<Account, MaxAccounts> accounts;
static std::size_t accountCount = 0;
static std::mutex bankMutex, consoleMutex;
static std::atomic<bool> running{true};
static Socket listenSocket = InvalidSocket;

static void printStatus() {
    std::lock_guard<std::mutex> lock(bankMutex);
    std::lock_guard<std::mutex> out(consoleMutex);
    std::cout << "\n\nBank status\n";
    if (!accountCount) std::cout << "No accounts in bank\n";
    for (std::size_t i = 0; i < accountCount; ++i)
        std::cout << std::left << std::setw(24) << accounts[i].name << std::fixed << std::setprecision(2)
                  << accounts[i].balance << (accounts[i].inSession ? "  IN SERVICE" : "") << '\n';
    std::cout << std::flush;
}

static void serveClient(Socket client) {
    int active = -1;
    std::string line;
    while (receiveLine(client, line)) {
        std::istringstream input(line);
        std::string command, value;
        input >> command;
        std::getline(input >> std::ws, value);
        std::string response;
        bool done = false;
        {
            std::lock_guard<std::mutex> lock(bankMutex);
            if (command == "open") {
                if (active != -1) response = "Command Error: Session is already opened";
                else if (value.empty()) response = "Command Error: Please provide a name";
                else if (accountCount >= MaxAccounts) response = "Command Error: Unable to allocate resources";
                else if (std::any_of(accounts.begin(), accounts.begin() + accountCount,
                                     [&](const Account& a) { return a.name == value; }))
                    response = "Command Error: Account name already exists. Try start command";
                else {
                    active = static_cast<int>(accountCount++);
                    accounts[active] = {value, 0, true};
                    response = "Account successfully created - " + value;
                }
            } else if (command == "start") {
                if (active != -1) response = "Command Error: Session is already opened";
                else if (value.empty()) response = "Command Error: Please provide an account name";
                else {
                    auto it = std::find_if(accounts.begin(), accounts.begin() + accountCount,
                                           [&](const Account& a) { return a.name == value; });
                    if (it == accounts.begin() + accountCount) response = "Account not found";
                    else if (it->inSession) response = "Account is locked by another user";
                    else { active = static_cast<int>(it - accounts.begin()); it->inSession = true; response = value + " session reopened"; }
                }
            } else if (command == "credit" || command == "debit") {
                if (active < 0) response = "Start or open an account before using this function";
                else {
                    try {
                        std::size_t used = 0; double amount = std::stod(value, &used);
                        if (value.empty() || used != value.size() || amount < 0) throw std::invalid_argument("amount");
                        Account& a = accounts[active];
                        if (command == "debit" && amount > a.balance) response = "Overdraft is not allowed for this account";
                        else {
                            a.balance += command == "credit" ? amount : -amount;
                            std::ostringstream out; out << a.name << " new balance: " << std::fixed << std::setprecision(2) << a.balance; response = out.str();
                        }
                    } catch (...) { response = "Please enter a valid non-negative amount"; }
                }
            } else if (command == "balance") {
                if (active < 0) response = "Start or open an account before using balance function";
                else { std::ostringstream out; out << accounts[active].name << " current balance: " << std::fixed << std::setprecision(2) << accounts[active].balance; response = out.str(); }
            } else if (command == "finish") {
                if (active < 0) response = "No open account to finish";
                else { accounts[active].inSession = false; response = accounts[active].name + " transactions finished"; active = -1; }
            } else if (command == "exit") {
                response = "Have a good day";
                if (active >= 0) accounts[active].inSession = false;
                active = -1; done = true;
            } else response = "I don't understand that command. Use open, start, credit, debit, balance, finish, or exit.";
        }
        if (!sendLine(client, response) || done) break;
    }
    if (active >= 0) { std::lock_guard<std::mutex> lock(bankMutex); accounts[active].inSession = false; }
    closeSocket(client);
}

int main() {
    if (!startSockets()) { std::cerr << "Could not initialize sockets\n"; return 1; }
    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == InvalidSocket) { std::cerr << "Could not create socket\n"; stopSockets(); return 1; }
    int yes = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY); address.sin_port = htons(ServerPort);
    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listenSocket, 20) != 0) {
        std::cerr << "Could not listen on port " << ServerPort << ". Is another server using it?\n";
        closeSocket(listenSocket); stopSockets(); return 1;
    }
    std::cout << "Bank server listening on port " << ServerPort << ". Press Ctrl+C to stop.\n";
    std::thread statusThread([] { while (running) { for (int i = 0; i < 20 && running; ++i) std::this_thread::sleep_for(std::chrono::seconds(1)); if (running) printStatus(); } });
    std::vector<std::thread> clients;
    while (running) {
        Socket client = accept(listenSocket, nullptr, nullptr);
        if (client == InvalidSocket) break;
        clients.emplace_back(serveClient, client);
    }
    running = false;
    for (auto& t : clients) if (t.joinable()) t.join();
    if (statusThread.joinable()) statusThread.join();
    closeSocket(listenSocket); stopSockets();
    return 0;
}
