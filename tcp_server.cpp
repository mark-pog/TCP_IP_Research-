#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
constexpr unsigned short SERVER_PORT = 5000;
constexpr int LISTEN_BACKLOG = 5;
constexpr std::size_t RECEIVE_BUFFER_SIZE = 64 * 1024;

bool sendAll(int socketHandle, const char* data, std::size_t length)
{
    std::size_t totalSent = 0;

    while (totalSent < length)
    {
        const ssize_t sent = send(
            socketHandle,
            data + totalSent,
            length - totalSent,
            MSG_NOSIGNAL);

        if (sent <= 0)
        {
            return false;
        }

        totalSent += static_cast<std::size_t>(sent);
    }

    return true;
}

void runThroughputReceiver(int clientSocket)
{
    std::vector<char> buffer(RECEIVE_BUFFER_SIZE);
    std::uint64_t totalBytes = 0;
    const auto start = std::chrono::steady_clock::now();

    while (true)
    {
        const ssize_t received = recv(clientSocket, buffer.data(), buffer.size(), 0);

        if (received == 0)
        {
            break;
        }

        if (received < 0)
        {
            std::perror("recv");
            return;
        }

        totalBytes += static_cast<std::uint64_t>(received);
    }

    const auto end = std::chrono::steady_clock::now();
    const double elapsedSeconds = std::chrono::duration<double>(end - start).count();
    const double throughputMbps =
        elapsedSeconds > 0.0
            ? (static_cast<double>(totalBytes) * 8.0) /
                  (elapsedSeconds * 1'000'000.0)
            : 0.0;

    std::cout << "\nThroughput connection closed\n"
              << "Received data: " << totalBytes << " bytes\n"
              << "Duration:      " << std::fixed << std::setprecision(3)
              << elapsedSeconds << " seconds\n"
              << "Throughput:    " << std::setprecision(2)
              << throughputMbps << " Mbps\n\n";
}

void runEchoServer(int clientSocket)
{
    std::vector<char> buffer(RECEIVE_BUFFER_SIZE);
    std::uint64_t totalBytes = 0;

    while (true)
    {
        const ssize_t received = recv(clientSocket, buffer.data(), buffer.size(), 0);

        if (received == 0)
        {
            break;
        }

        if (received < 0)
        {
            std::perror("recv");
            return;
        }

        if (!sendAll(clientSocket, buffer.data(), static_cast<std::size_t>(received)))
        {
            std::cerr << "The client disconnected while echo data was being sent.\n";
            return;
        }

        totalBytes += static_cast<std::uint64_t>(received);
    }

    std::cout << "\nLatency connection closed\n"
              << "Echoed data: " << totalBytes << " bytes\n\n";
}

std::string clientAddressToString(const sockaddr_in& address)
{
    char buffer[INET_ADDRSTRLEN]{};

    if (inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) == nullptr)
    {
        return "unknown";
    }

    return buffer;
}
} // namespace

int main()
{
    const int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0)
    {
        std::perror("socket");
        return 1;
    }

    int reuseAddress = 1;
    if (setsockopt(
            serverSocket,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuseAddress,
            sizeof(reuseAddress)) < 0)
    {
        std::perror("setsockopt");
        close(serverSocket);
        return 1;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddress.sin_port = htons(SERVER_PORT);

    if (bind(
            serverSocket,
            reinterpret_cast<sockaddr*>(&serverAddress),
            sizeof(serverAddress)) < 0)
    {
        std::perror("bind");
        close(serverSocket);
        return 1;
    }

    if (listen(serverSocket, LISTEN_BACKLOG) < 0)
    {
        std::perror("listen");
        close(serverSocket);
        return 1;
    }

    std::cout << "TCP server is listening on port " << SERVER_PORT << ".\n"
              << "Press Ctrl+C to stop it.\n\n";

    while (true)
    {
        sockaddr_in clientAddress{};
        socklen_t clientAddressLength = sizeof(clientAddress);

        const int clientSocket = accept(
            serverSocket,
            reinterpret_cast<sockaddr*>(&clientAddress),
            &clientAddressLength);

        if (clientSocket < 0)
        {
            std::perror("accept");
            continue;
        }

        std::cout << "Client connected from "
                  << clientAddressToString(clientAddress) << ':'
                  << ntohs(clientAddress.sin_port) << '\n';

        char mode = '\0';
        const ssize_t modeBytes = recv(clientSocket, &mode, 1, MSG_WAITALL);

        if (modeBytes != 1)
        {
            std::cerr << "The client disconnected before sending a valid mode.\n";
            close(clientSocket);
            continue;
        }

        if (mode == 'T')
        {
            std::cout << "Running throughput receiver.\n";
            runThroughputReceiver(clientSocket);
        }
        else if (mode == 'L')
        {
            std::cout << "Running latency echo server.\n";
            runEchoServer(clientSocket);
        }
        else
        {
            std::cerr << "Unknown mode received: " << mode << '\n';
        }

        close(clientSocket);
    }

    close(serverSocket);
    return 0;
}
