#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <vector>
#include <set>
#include <mutex>
#include <atomic>
#include <chrono>
#include <fstream>
#include <random>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

const int PORT = 25565;
const int TIMEOUT_MS = 2000;   // 2s timeout
const int MAX_THREADS = 100;

std::mutex listMutex;
std::mutex printMutex;
std::mutex fileMutex;

std::vector<std::string> ipList;      // L1 – IP do sprawdzenia
std::set<std::string> ipInProgress;   // L2 – IP aktualnie sprawdzane

std::chrono::steady_clock::time_point startTime;

std::atomic<int> checkedCount(0);
std::vector<std::string> foundServers;

std::atomic<bool> runningWorkers(false);
std::atomic<bool> killAllWorkers(false);

void printWithTime(const std::string& message) {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
    std::lock_guard<std::mutex> lock(printMutex);
    std::cout << "[" << ms << "ms] " << message << "\n";
}

// Zwraca: czy dostępny, opis/komunikat, czas odpowiedzi (ms)
std::tuple<bool, std::string, int> isMinecraftServerWithPing(const std::string& ip) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return {false, "Błąd socket()", -1};

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);

    if (inet_pton(AF_INET, ip.c_str(), &server.sin_addr) <= 0) {
        closesocket(sock);
        return {false, "Błąd inet_pton", -1};
    }

    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    auto start = std::chrono::steady_clock::now();

    int res = connect(sock, (sockaddr*)&server, sizeof(server));
    if (res == SOCKET_ERROR) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(sock, &writeSet);
        timeval tv{};
        tv.tv_sec = 2;  // 2s timeout
        tv.tv_usec = 0;

        res = select(0, nullptr, &writeSet, nullptr, &tv);
        if (res <= 0) {
            closesocket(sock);
            return {false, "Timeout", -1};
        }

        int err;
        int len = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err != 0) {
            closesocket(sock);
            return {false, "Połączenie odrzucone", -1};
        }
    }

    auto end = std::chrono::steady_clock::now();
    int pingMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    closesocket(sock);
    return {true, "OK", pingMs};
}

void worker() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 3000);

    while (runningWorkers && !killAllWorkers) {
        std::string ip;
        {
            std::lock_guard<std::mutex> lock(listMutex);
            if (ipList.empty() || killAllWorkers) return;

            int idx = rand() % ipList.size();
            ip = ipList[idx];

            if (ipInProgress.count(ip)) continue;
            ipInProgress.insert(ip);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(dist(gen)));
        if (killAllWorkers) return;

        auto [ok, msg, ping] = isMinecraftServerWithPing(ip);

        {
            std::lock_guard<std::mutex> lock(printMutex);
            checkedCount++;
            double progress = (double)checkedCount / (checkedCount + ipList.size()) * 100.0;
            std::cout << "Progres: " << int(progress) << "% "
                      << checkedCount << "/" << checkedCount + ipList.size()
                      << " | Serwery: ";
            for (const auto& s : foundServers) std::cout << s << " ";
            std::cout << "\n";
        }

        if (ok) {
            printWithTime(ip + " - Jest serwer Minecraft!");
            std::lock_guard<std::mutex> fileLock(fileMutex);
            foundServers.push_back(ip);
            std::ofstream outFile("serwery.txt", std::ios::app);
            outFile << ip << "\n";
        }

        {
            std::lock_guard<std::mutex> lock(listMutex);
            ipList.erase(std::remove(ipList.begin(), ipList.end(), ip), ipList.end());
            ipInProgress.erase(ip);
        }
    }
}

std::vector<std::thread> startWorkers() {
    killAllWorkers = false;
    runningWorkers = true;
    std::vector<std::thread> threads;
    for (int i=0; i<MAX_THREADS; i++) {
        threads.emplace_back(worker);
    }
    return threads;
}

void stopWorkers(std::vector<std::thread>& threads) {
    killAllWorkers = true;
    runningWorkers = false;
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    threads.clear();
}

void hypixelMonitor() {
    const std::string hypixelIP = "172.65.198.170";
    std::vector<std::thread> workers;

    while (true) {
        auto [ok, msg, ping] = isMinecraftServerWithPing(hypixelIP);

        if (!ok) {
            printWithTime("Hypixel NIE odpowiada – zabijam wszystkie workery!");
            {
                std::lock_guard<std::mutex> lock(listMutex);
                ipInProgress.clear();
            }
            stopWorkers(workers);
            {
                std::lock_guard<std::mutex> lock(printMutex);
                std::cout << "Hypixel nie odpowiada\n";
            }
        } else {
            {
                std::lock_guard<std::mutex> lock(printMutex);
                std::cout << "Hypixel odpowiedział: " << ping << "ms\n";
            }
            if (!runningWorkers) {
                printWithTime("Hypixel wrócił – uruchamiam workery!");
                workers = startWorkers();
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(2)); // ping co 2 sekundy
    }
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);

    startTime = std::chrono::steady_clock::now();

    std::string startIP, endIP;
    std::cout << "Podaj początkowe IP: ";
    std::cin >> startIP;
    std::cout << "Podaj końcowe IP: ";
    std::cin >> endIP;

    int start[4], end[4];
    sscanf(startIP.c_str(), "%d.%d.%d.%d", &start[0], &start[1], &start[2], &start[3]);
    sscanf(endIP.c_str(), "%d.%d.%d.%d", &end[0], &end[1], &end[2], &end[3]);

    auto ipToInt = [](int ip[4]) -> uint32_t {
        return (ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3];
    };
    auto intToIp = [](uint32_t val) -> std::string {
        return std::to_string((val >> 24) & 0xFF) + "." +
               std::to_string((val >> 16) & 0xFF) + "." +
               std::to_string((val >> 8) & 0xFF) + "." +
               std::to_string(val & 0xFF);
    };

    uint32_t startInt = ipToInt(start);
    uint32_t endInt   = ipToInt(end);

    for (uint32_t cur = startInt; cur <= endInt; ++cur) {
        ipList.push_back(intToIp(cur));
    }

    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(ipList.begin(), ipList.end(), g);

    std::thread monitor(hypixelMonitor);
    monitor.join();

    WSACleanup();
    return 0;
}
