/**********************************************************

    * server.cpp by Eric Tatchell, 03-2024
    * For use in Tank Game Server

***********************************************************/


#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <WS2tcpip.h>
#include <vector>
#include <cstring>
#include <algorithm> 
#include <thread>    
#include <atomic>    
#include <mutex>
#include <queue>
#include <condition_variable>

using namespace std;
#pragma comment (lib, "ws2_32.lib")

struct Packet {

    // 0 = movement
    uint8_t packetType;

    // 1 to 3
    uint16_t playerId;

    // 0 = stationary
    // 1 = moving
    uint8_t movementState;

    // 0 = x, y
    // 1 = x, +y
    // 2 = x, -y
    // 3 = +x, y
    // 4 = -x, y
    // 5 = +x, +y
    // 6 = -x, +y
    // 7 = -x, +y
    // 8 = +x, -y
    uint8_t direction;

    uint32_t timestamp;

    string name;
};

struct PacketTimestampComparator {
    bool operator()(const Packet& lhs, const Packet& rhs) const {
        return lhs.timestamp > rhs.timestamp; // Invert comparison to turn max-heap into min-heap
    }
};

//struct Client {
//    sockaddr_in address;
//    string name;
//    string ip;
//    string player;
//    string health;
    //string x;
    //string y;
//
//    Client(sockaddr_in addr, string ipAddress, string clientName) : address(addr), ip(ipAddress), name(clientName) {}
//    Client() : address(), name(""), ip("") {}
//};

struct Client {
    sockaddr_in address;
    string ip;
    string name;

    uint16_t playerId;
    uint8_t movementState;
    uint8_t direction;
};


struct GameState {
    vector<Client> players;
    string datagram;
    std::atomic<bool> stopServer;
    std::atomic<bool> receiving;
    std::priority_queue<Packet, std::vector<Packet>, PacketTimestampComparator> msg_queue;
};

GameState gameState;

std::mutex mtx;
std::mutex receiveMutex;
std::condition_variable cv;
std::atomic<unsigned long long> globalSeqNum = 0;

void NotifyGameLoop() {
    cv.notify_one();
}

void WriteClient(Client client) {
    ofstream file("clients.txt", ios::app);
    if (file.is_open()) {
        file << client.name << " " << client.ip << endl;
        file.close();
    }
}

void ClientFileCleanup() {
    std::ofstream ofs;
    ofs.open("clients.txt", std::ofstream::trunc);
    ofs.close();
}

void RemoveClientFromFile(const string& ipAddress) {
    ifstream inFile("clients.txt");
    ofstream outFile("temp.txt");

    if (!inFile) {
        cerr << "Error opening input file!" << endl;
        return;
    }
    if (!outFile) {
        cerr << "Error creating temporary file!" << endl;
        return;
    }

    string line;
    bool found = false;
    while (getline(inFile, line)) {
        istringstream iss(line);
        string token;
        while (iss >> token);
        if (token == ipAddress) {
            found = true;
            continue;
        }
        outFile << line << endl;
    }
    inFile.close();
    outFile.close();

    if (remove("clients.txt") != 0) {
        cerr << "Error removing file!" << endl;
        return;
    }
    if (rename("temp.txt", "clients.txt") != 0) {
        cerr << "Error renaming file!" << endl;
        return;
    }

    if (!found) {
        cerr << "IP address not found in file!" << endl;
    }
}

Client FindClientByPlayerId(GameState& gameState, uint16_t playerId) {
    for (Client& client : gameState.players) {
        if (client.playerId == playerId) {
            return client; // Found the client
        }
    }
    return Client(); // No matching client found
}

void RegisterNew(Packet packet, sockaddr_in addr) {
    Client client;
    client.name = packet.name;
    client.movementState = packet.movementState;
    client.playerId = packet.playerId;
    client.direction = packet.direction;
    client.address = addr;
    gameState.players.emplace_back(client);
}

void ParsePacket(const char* buf, Packet& packet) {
    std::string data(buf);

    packet.packetType = static_cast<uint8_t>(data[0] - '0');
    packet.movementState = static_cast<uint8_t>(data[2] - '0');
    packet.direction = static_cast<uint8_t>(data[3] - '0');
    packet.playerId = static_cast<uint16_t>(std::stoi(data.substr(1, 1)));

    size_t timestampStart = 4;
    size_t nameStart = data.find_first_not_of("0123456789", timestampStart);
    if (nameStart == std::string::npos) {
        return;
    }
    packet.timestamp = static_cast<uint32_t>(std::stoul(data.substr(timestampStart, nameStart - timestampStart)));
    packet.name = data.substr(nameStart);
}


void ReceiveMessages(SOCKET serverSocket) {
    sockaddr_in client;
    int clientLength = sizeof(client);
    char buf[64]; // Adjust buffer size to match Packet size
    while (!gameState.stopServer) {
        ZeroMemory(buf, 64);
        int bytesIn = recvfrom(serverSocket, buf, sizeof(buf), 0, (sockaddr*)&client, &clientLength);
        if (bytesIn > 0) {
            Packet packet;
            ParsePacket(buf, packet);
            cout << "RECV: " << buf << "\n";
            if (packet.packetType == 1) {
                RegisterNew(packet, client);
            }
            
            gameState.msg_queue.push(packet);
            NotifyGameLoop();
        }
    }
}



void GameLoop(SOCKET serverSocket) {
    while (!gameState.stopServer) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [] { return !gameState.msg_queue.empty(); });

        while (!gameState.msg_queue.empty()) {
            Packet packet = gameState.msg_queue.top(); // Get the oldest packet
            gameState.msg_queue.pop(); // Remove it from the queue

            uint16_t id = packet.playerId;
            Client client = FindClientByPlayerId(gameState, id);

            std::string buf;
            buf.append(std::to_string(packet.packetType));
            buf.append(std::to_string(packet.playerId));
            buf.append(std::to_string(packet.movementState));
            buf.append(std::to_string(packet.direction));
            buf.append(std::to_string(packet.timestamp));
            buf.append(packet.name);

            // broadcast
            bool success = true;

            for (auto& connectedClient : gameState.players) {
                if (connectedClient.playerId != id) { // skip the sender
                    int sendOk = sendto(serverSocket, buf.c_str(), sizeof(Packet), 0, (sockaddr*)&connectedClient.address, sizeof(connectedClient.address));
                    if (sendOk == SOCKET_ERROR) {
                        std::cerr << "sendto failed: " << WSAGetLastError() << std::endl;
                        success = false;
                    }
                }
            }
            if (success) {
                cout << "SEND: " << buf << "\n";
            }
        }
    }
}



void command(vector<Client>& clients, SOCKET serverSocket) {
    string input;
    while (!gameState.stopServer) {
        cin >> input;
        if (input == "designation_eric") {
            //std::unique_lock<std::mutex> lock(mtx);


            /*ClientFileCleanup();
            std::string serverStoppedMessage = "Server stopped\n------END GAME LOG------";
            cout << serverStoppedMessage << endl;
            gameState.msg_queue.push(serverStoppedMessage);
            cout.flush();
            NotifyGameLoop();*/
        }
    }
}


int main() {
    ClientFileCleanup();
    WSADATA wsData;
    WORD ver = MAKEWORD(2, 2);

    int wsOK = WSAStartup(ver, &wsData);
    if (wsOK != 0) {
        cerr << "Winsock initialization error, quitting" << endl;
        return -1;
    }

    SOCKET serverSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (serverSocket == INVALID_SOCKET) {
        cerr << "Socket creation error, quitting" << endl;
        WSACleanup();
        return -1;
    }

    cout << "Waiting for port... (Enter a port, then press Start)" << endl;
    // string input;
    // cin >> input;
    // string portStr = input.substr(strlen("set_port:"));
    // int port = std::stoi(portStr);
    int port = 8000;
    sockaddr_in server;
    server.sin_addr.S_un.S_addr = ADDR_ANY;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if (bind(serverSocket, (sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) {
        cerr << "IP/Port binding error, quitting" << endl;
        closesocket(serverSocket);
        WSACleanup();
        return -1;
    }
    else {
        cout << "------ BEGIN GAME LOG ------" << endl;
        cout << "Successfully started the server on port " << port << endl;
    }

    gameState.receiving = true;
    thread receiveThread(ReceiveMessages, serverSocket);
    thread gameLoopThread(GameLoop, serverSocket);
    thread commandListener(command, ref(gameState.players), serverSocket);
    //thread frequentUpdates(UpdateFrequently);

    while (!gameState.stopServer) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [] { return !gameState.msg_queue.empty(); });
        NotifyGameLoop();
    }

    receiveThread.join();
    gameLoopThread.join();
    commandListener.join();
    cout.flush();
    closesocket(serverSocket);
    WSACleanup();
    return 0;
}
