TCP/IP Lab Programs

Files

tcp_client.cpp  Windows client using Winsock
tcp_server.cpp  Linux/Raspberry Pi server

Raspberry Pi

Compile:
    g++ tcp_server.cpp -std=c++17 -O2 -Wall -Wextra -o tcp_server

Run:
    ./tcp_server

Windows with Visual Studio Developer Command Prompt

Compile:
    cl /EHsc /std:c++17 /O2 tcp_client.cpp ws2_32.lib

Run throughput:
    tcp_client.exe 192.168.1.222 throughput 1460 15

Run latency:
    tcp_client.exe 192.168.1.222 latency 512 5 1000

The server must be running before the client is started.
Both programs use TCP port 5000.
