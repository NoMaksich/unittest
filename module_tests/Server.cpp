#include "ErrorLog.h"
#include "Server.h"
#include <iostream>
#include <unistd.h>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <limits>
#include <random>
#include <cryptopp/md5.h>
#include <cryptopp/hex.h>
#include <sys/ioctl.h>
#include <cryptopp/filters.h>
#include <unordered_map>

using namespace std;

Server::Server(unsigned short port, int qlen, const string& db):
    sock(socket(AF_INET, SOCK_STREAM, 0)),
    self_addr(new sockaddr_in),
    foreign_addr(new sockaddr_in),
    queueLen(qlen)
{
    if (sock == -1) {
        ErrorLog::logError(true, "Socket error");
        throw std::system_error(errno, std::generic_category(), "Socket error");
    }

    int on = 1;
    int rc = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (rc == -1) {
        ErrorLog::logError(true, "Socket error");
        throw std::system_error(errno, std::generic_category(), "Socket error");
    }

    self_addr->sin_family = AF_INET;
    self_addr->sin_port = htons(port);
    self_addr->sin_addr.s_addr = INADDR_ANY;

    if (bind(sock,
             reinterpret_cast<const sockaddr*>(self_addr.get()),
             sizeof(sockaddr_in)) == -1) {
        ErrorLog::logError(true, "Socket error");
        throw std::system_error(errno, std::generic_category(), "Socket error");
    }
}

void Server::get_base(const string& db_file)
{
    try {
        ifstream database(db_file);
        if (!database.is_open()) {
            ErrorLog::logError(true, "Failed to open database file");
        }

        string line;
        while (getline(database, line)) {
            auto delimiterPos = line.find(':');
            if (delimiterPos != string::npos) {
                string identifier = line.substr(0, delimiterPos);
                string password = line.substr(delimiterPos + 1);
                userCredentials.emplace(identifier, password);
            } else {
                ErrorLog::logError(true, "Invalid database format");
            }
        }
        database.close();
    } catch (const exception& e) {
        cerr << "Error occurred in get_base(): " << e.what() << endl;
    }
}


bool Server::authentication(int work_sock, const unordered_map<string, string>& credentials)
{
    try {
        int rc;
        std::mt19937_64 gen(time(nullptr));

        string userLogin = str_read(work_sock);
        auto it = credentials.find(userLogin);
        if (it == credentials.end())
            throw std::system_error(errno, std::generic_category(), "Auth error: unknown user");

        std::clog << "log: username ok\n";


        string password = it->second;

        // Генерация соли
        uint64_t rnd = gen();
        std::string salt, message;
        CryptoPP::StringSource((const CryptoPP::byte*)&rnd,
                               8,
                               true,
                               new CryptoPP::HexEncoder(new CryptoPP::StringSink(salt)));

        rc = send(work_sock, salt.c_str(), salt.size(), 0);
        if (rc == -1)
            throw std::system_error(errno, std::generic_category(), "Send salt error");

        std::clog << "log: sending SALT " << salt << std::endl;

        CryptoPP::Weak1::MD5 hash;
        CryptoPP::StringSource(salt + password,
                               true,
                               new CryptoPP::HashFilter(hash, new CryptoPP::HexEncoder(new CryptoPP::StringSink(message))));

        std::clog << "log: waiting MESSAGE " << message << std::endl;

        string userMessage = str_read(work_sock);
        if (userMessage != message)
            throw std::system_error(errno, std::generic_category(), "Auth error: password mismatch");

        std::clog << "log: auth success, sending OK\n";

        const char* authSuccess = "OK";
        rc = send(work_sock, authSuccess, strlen(authSuccess), 0);
        if (rc == -1)
            throw std::system_error(errno, std::generic_category(), "Send OK error");

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error in authentication(): " << e.what() << std::endl;
        ErrorLog::logError(true, std::string("Error in authentication(): ") + e.what());
        return false;
    }
}

float Server::quad(int work_sock)
{
    try {
        const int floatSize = sizeof(float);
        int numbersOfVectors = 0;

        int rc = recv(work_sock, reinterpret_cast<char*>(&numbersOfVectors), sizeof(int), 0);
        if (rc != sizeof(int)) {
            throw std::runtime_error("Failed to receive the number of vectors");
        }

        for (int i = 0; i < numbersOfVectors; ++i) {
            int sizeOfVector = 0;
            rc = recv(work_sock, reinterpret_cast<char*>(&sizeOfVector), sizeof(int), 0);
            if (rc != sizeof(int)) {
                throw std::runtime_error("Failed to receive the size of vector");
            }

            std::vector<float> vectorData(sizeOfVector);

            rc = recv(work_sock, reinterpret_cast<char*>(vectorData.data()), sizeOfVector * floatSize, 0);
            if (rc != sizeOfVector * floatSize) {
                throw std::runtime_error("Failed to receive vector data");
            }

            float sum = 0.0f;
            for (float num : vectorData) {
                sum += num * num;
            }

            const float maxFloat = std::numeric_limits<float>::max();
            const float minFloat = std::numeric_limits<float>::lowest();

            if (std::isinf(sum)) { // Проверка на бесконечность (переполнение)
                if (sum > 0.0f) {
                    sum = maxFloat - 1.0f;
                } else {
                    sum = minFloat;
                }
            }


            rc = send(work_sock, reinterpret_cast<char*>(&sum), sizeof(float), 0);
            if (rc != sizeof(float)) {
                throw std::runtime_error("Failed to send the result of calculation");
            }
        }

        return 0.0f;
    } catch (const std::exception& e) {
        std::cerr << "Error occurred in quad(): " << e.what() << std::endl;
        ErrorLog::logError(true, std::string("Error occurred in quad(): ") + e.what());
        return 0.0f;
    }
}





void Server::operator()(int work_sock)
{
    bool authenticated = authentication(work_sock, userCredentials);
    if (authenticated) {
        float result = quad(work_sock);

        std::ostringstream oss;
        oss << result;
        std::string resultStr = oss.str();

        int rc = send(work_sock, resultStr.c_str(), resultStr.size(), 0);
        if (rc == -1)
            throw std::system_error(errno, std::generic_category(), "Send result error");
    }
    close(work_sock);
}


void Server::startListening(Server & server)
{
    try {
        if (listen(sock, queueLen) == -1)
            throw std::system_error(errno, std::generic_category(), "Listen error");

        socklen_t socklen = sizeof(sockaddr_in);
        while (true) {
            int work_sock = accept(sock, reinterpret_cast<sockaddr*>(foreign_addr.get()), &socklen);
            if (work_sock == -1)
                throw std::system_error(errno, std::generic_category(), "Accept error");

            std::string ip_addr(inet_ntoa(foreign_addr->sin_addr));
            std::clog << "log: Connection established with " << ip_addr << std::endl;

            try {
                server(work_sock);
            } catch (std::system_error &e) {
                std::cerr << e.what() << "\nConnection with " << ip_addr << " aborted\n";
            } catch (std::bad_alloc &e) {
                std::cerr << e.what() << ": vector size too big\nConnection with " << ip_addr << " aborted\n";
            }
            close(work_sock);
            std::clog << "log: Connection closed\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Error occurred in startListening(): " << e.what() << std::endl;
        ErrorLog::logError(true, std::string("Error occurred in startListening(): ") + e.what());
    }
}

std::string Server::str_read(int work_sock)
{
    int rc;
    int buflen = BUFLEN;
    std::unique_ptr<char[]> buf(new char[buflen]);
    rc = recv(work_sock, buf.get(), buflen, 0);
    if (rc == -1)
        throw std::system_error(errno, std::generic_category(), "Recv string error");
    std::string res(buf.get(), rc);
    if (rc == buflen) {
        int tail_size;
        rc = ioctl(work_sock, FIONREAD, &tail_size);
        if (rc == -1)
            throw std::system_error(errno, std::generic_category(), "IOCTL error");
        if (tail_size > 0) {
            if (tail_size > buflen)
                buf = std::unique_ptr<char[]>(new char[tail_size]);
            rc = recv(work_sock, buf.get(), tail_size, 0);
            if (rc == -1)
                throw std::system_error(errno, std::generic_category(), "Recv string error");
            res.append(buf.get(), rc);
        }
    }
    res.resize(res.find_last_not_of("\n\r") + 1);
    return res;
}
Server::~Server()
{
    close(sock);
    self_addr.reset();
    foreign_addr.reset();
}
