#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace
{
constexpr unsigned short SERVER_PORT = 5000;
constexpr std::size_t MAX_PAYLOAD_SIZE = 1024 * 1024; // 1 MiB

class WinsockSession
{
public:
    WinsockSession()
    {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0)
        {
            throw std::runtime_error("WSAStartup failed with error " + std::to_string(result));
        }
    }

    ~WinsockSession()
    {
        WSACleanup();
    }

    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;
};

void closeSocket(SOCKET socketHandle)
{
    if (socketHandle != INVALID_SOCKET)
    {
        closesocket(socketHandle);
    }
}

bool sendAll(SOCKET socketHandle, const char* data, std::size_t length)
{
    std::size_t totalSent = 0;

    while (totalSent < length)
    {
        const std::size_t remaining = length - totalSent;
        const int chunkLength = static_cast<int>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));

        const int sent = send(socketHandle, data + totalSent, chunkLength, 0);
        if (sent == SOCKET_ERROR || sent == 0)
        {
            return false;
        }

        totalSent += static_cast<std::size_t>(sent);
    }

    return true;
}

bool receiveAll(SOCKET socketHandle, char* data, std::size_t length)
{
    std::size_t totalReceived = 0;

    while (totalReceived < length)
    {
        const std::size_t remaining = length - totalReceived;
        const int chunkLength = static_cast<int>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));

        const int received = recv(socketHandle, data + totalReceived, chunkLength, 0);
        if (received == SOCKET_ERROR || received == 0)
        {
            return false;
        }

        totalReceived += static_cast<std::size_t>(received);
    }

    return true;
}

std::size_t parsePayloadSize(const char* text)
{
    const unsigned long long value = std::stoull(text);

    if (value == 0 || value > MAX_PAYLOAD_SIZE)
    {
        throw std::runtime_error("Payload size must be between 1 and 1048576 bytes.");
    }

    return static_cast<std::size_t>(value);
}

int parsePositiveInt(const char* text, const std::string& fieldName)
{
    const long value = std::stol(text);

    if (value <= 0 || value > std::numeric_limits<int>::max())
    {
        throw std::runtime_error(fieldName + " must be a positive integer.");
    }

    return static_cast<int>(value);
}

SOCKET connectToServer(const std::string& serverIp)
{
    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET)
    {
        throw std::runtime_error("socket() failed with error " + std::to_string(WSAGetLastError()));
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, serverIp.c_str(), &serverAddress.sin_addr) != 1)
    {
        closeSocket(socketHandle);
        throw std::runtime_error("Invalid IPv4 address: " + serverIp);
    }

    if (connect(socketHandle, reinterpret_cast<sockaddr*>(&serverAddress), sizeof(serverAddress)) ==
        SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        closeSocket(socketHandle);
        throw std::runtime_error("connect() failed with error " + std::to_string(error));
    }

    return socketHandle;
}

void runThroughputTest(
    SOCKET socketHandle,
    std::size_t payloadSize,
    int durationSeconds)
{
    const char mode = 'T';
    if (!sendAll(socketHandle, &mode, 1))
    {
        throw std::runtime_error("Could not send throughput mode to the server.");
    }

    std::vector<char> payload(payloadSize);
    for (std::size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<char>(i & 0xFF);
    }

    std::uint64_t totalBytes = 0;
    const auto start = std::chrono::steady_clock::now();
    const auto stopTime = start + std::chrono::seconds(durationSeconds);

    while (std::chrono::steady_clock::now() < stopTime)
    {
        if (!sendAll(socketHandle, payload.data(), payload.size()))
        {
            throw std::runtime_error("The connection was lost while sending data.");
        }

        totalBytes += static_cast<std::uint64_t>(payload.size());
    }

    shutdown(socketHandle, SD_SEND);

    const auto end = std::chrono::steady_clock::now();
    const double elapsedSeconds = std::chrono::duration<double>(end - start).count();
    const double throughputMbps =
        (static_cast<double>(totalBytes) * 8.0) / (elapsedSeconds * 1'000'000.0);

    std::cout << "\nThroughput test completed\n"
              << "Payload size: " << payloadSize << " bytes\n"
              << "Duration:     " << std::fixed << std::setprecision(3)
              << elapsedSeconds << " seconds\n"
              << "Sent data:    " << totalBytes << " bytes\n"
              << "Throughput:   " << std::setprecision(2)
              << throughputMbps << " Mbps\n";
}

void runLatencyTest(
    SOCKET socketHandle,
    std::size_t payloadSize,
    int intervalMilliseconds,
    int measurementCount)
{
    const char mode = 'L';
    if (!sendAll(socketHandle, &mode, 1))
    {
        throw std::runtime_error("Could not send latency mode to the server.");
    }

    std::vector<char> transmitted(payloadSize);
    std::vector<char> received(payloadSize);

    for (std::size_t i = 0; i < transmitted.size(); ++i)
    {
        transmitted[i] = static_cast<char>((i * 7) & 0xFF);
    }

    double minimumRttMs = std::numeric_limits<double>::max();
    double maximumRttMs = 0.0;
    double totalRttMs = 0.0;

    for (int measurement = 0; measurement < measurementCount; ++measurement)
    {
        const auto start = std::chrono::steady_clock::now();

        if (!sendAll(socketHandle, transmitted.data(), transmitted.size()))
        {
            throw std::runtime_error("The connection was lost while sending a latency message.");
        }

        if (!receiveAll(socketHandle, received.data(), received.size()))
        {
            throw std::runtime_error("The connection was lost while receiving an echoed message.");
        }

        const auto end = std::chrono::steady_clock::now();

        if (received != transmitted)
        {
            throw std::runtime_error("Received data did not match the transmitted data.");
        }

        const double rttMs =
            std::chrono::duration<double, std::milli>(end - start).count();

        minimumRttMs = std::min(minimumRttMs, rttMs);
        maximumRttMs = std::max(maximumRttMs, rttMs);
        totalRttMs += rttMs;

        if (measurement + 1 < measurementCount)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMilliseconds));
        }
    }

    shutdown(socketHandle, SD_SEND);

    const double averageRttMs = totalRttMs / static_cast<double>(measurementCount);

    std::cout << "\nLatency test completed\n"
              << "Payload size: " << payloadSize << " bytes\n"
              << "Interval:     " << intervalMilliseconds << " ms\n"
              << "Measurements: " << measurementCount << "\n"
              << std::fixed << std::setprecision(3)
              << "Minimum RTT:  " << minimumRttMs << " ms\n"
              << "Maximum RTT:  " << maximumRttMs << " ms\n"
              << "Average RTT:  " << averageRttMs << " ms\n";
}

void printUsage(const char* programName)
{
    std::cerr
        << "Usage:\n"
        << "  " << programName
        << " <server-ip> throughput <payload-bytes> <duration-seconds>\n"
        << "  " << programName
        << " <server-ip> latency <payload-bytes> <interval-ms> <measurements>\n\n"
        << "Examples:\n"
        << "  " << programName << " 192.168.1.222 throughput 1460 15\n"
        << "  " << programName << " 192.168.1.222 latency 512 5 1000\n";
}
} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    try
    {
        WinsockSession winsock;
        const std::string serverIp = argv[1];

        if (argc == 5 && std::strcmp(argv[2], "throughput") == 0)
        {
            const std::size_t payloadSize = parsePayloadSize(argv[3]);
            const int durationSeconds = parsePositiveInt(argv[4], "Duration");

            SOCKET socketHandle = connectToServer(serverIp);
            std::cout << "Connected to " << serverIp << ':' << SERVER_PORT << '\n';

            try
            {
                runThroughputTest(socketHandle, payloadSize, durationSeconds);
            }
            catch (...)
            {
                closeSocket(socketHandle);
                throw;
            }

            closeSocket(socketHandle);
            return EXIT_SUCCESS;
        }

        if (argc == 6 && std::strcmp(argv[2], "latency") == 0)
        {
            const std::size_t payloadSize = parsePayloadSize(argv[3]);
            const int intervalMs = parsePositiveInt(argv[4], "Interval");
            const int measurementCount = parsePositiveInt(argv[5], "Measurement count");

            SOCKET socketHandle = connectToServer(serverIp);
            std::cout << "Connected to " << serverIp << ':' << SERVER_PORT << '\n';

            try
            {
                runLatencyTest(socketHandle, payloadSize, intervalMs, measurementCount);
            }
            catch (...)
            {
                closeSocket(socketHandle);
                throw;
            }

            closeSocket(socketHandle);
            return EXIT_SUCCESS;
        }

        printUsage(argv[0]);
        return EXIT_FAILURE;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
