// http_server.cpp
#include "http_server.h"
#include <iostream>
#include <cstring>
#include <sstream>
#include <vector>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#ifdef _WIN32
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netdb.h>
#endif

HttpServer::HttpServer(Database& db, int port) : database(db), server_port(port), is_running(false) {}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    is_running = true;
    server_thread = std::thread(&HttpServer::run, this);
}

void HttpServer::stop() {
    if (is_running) {
        is_running = false;
#ifdef _WIN32
        // На Windows необходимо закрыть сокеты иначе, это может привести к зависанию
        // Дополнительно можно использовать WSAAsyncSelect или другие методы
#endif
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }
}

void HttpServer::run() {
#ifdef _WIN32
    // Инициализация Winsock
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (iResult != 0) {
        std::cerr << "WSAStartup failed: " << iResult << "\n";
        return;
    }
#endif

    // Создание сокета
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        std::cerr << "Не удалось создать сокет.\n";
        return;
    }

    // Привязка сокета к порту
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(server_port);

    int opt = 1;
    // Установка опции SO_REUSEADDR
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt)) < 0) {
        std::cerr << "Ошибка setsockopt.\n";
        close(server_fd);
        return;
    }

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Не удалось привязать сокет к порту.\n";
        close(server_fd);
        return;
    }

    // Прослушивание входящих соединений
    if (listen(server_fd, 10) < 0) {
        std::cerr << "Ошибка listen.\n";
        close(server_fd);
        return;
    }

    std::cout << "HTTP-сервер запущен на порту " << server_port << "\n";

    while (is_running) {
        struct sockaddr_in client_address;
        socklen_t client_len = sizeof(client_address);
        int client_socket = accept(server_fd, (struct sockaddr*)&client_address, &client_len);
        if (client_socket < 0) {
            if (is_running) {
                std::cerr << "Ошибка accept.\n";
            }
            continue;
        }

        // Обработка клиента в отдельном потоке
        std::thread(&HttpServer::handle_client, this, client_socket).detach();
    }

    close(server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
}

void HttpServer::handle_client(int client_socket) {
    char buffer[1024];
    std::memset(buffer, 0, sizeof(buffer));
    int bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        close(client_socket);
        return;
    }

    std::string request(buffer);
    std::string response = handle_request(request);

    send(client_socket, response.c_str(), response.length(), 0);
    close(client_socket);
}

std::string HttpServer::handle_request(const std::string& request) {
    std::istringstream request_stream(request);
    std::string method;
    std::string path;
    request_stream >> method >> path;

    if (method != "GET") {
        return "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
    }

    // Обработка разных путей
    if (path == "/current") {
        double current_temp = 0.0;
        {
            std::lock_guard<std::mutex> lock(db_mutex);
            current_temp = database.get_latest_temperature();
        }

        std::ostringstream oss;
        oss << "{ \"current_temperature\": " << current_temp << " }";

        std::string body = oss.str();
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Content-Length: " << body.length() << "\r\n"
                 << "Access-Control-Allow-Origin: *\r\n"
                 << "\r\n"
                 << body;
        return response.str();
    }
    else if (path.find("/average") == 0) {
        // Пример пути: /average?period=1 или /average?period=2
        size_t pos = path.find("?");
        if (pos == std::string::npos) {
            return "HTTP/1.1 400 Bad Request\r\n\r\n";
        }

        std::string query = path.substr(pos + 1);
        std::istringstream query_stream(query);
        std::string key, value;
        int period = 1; // По умолчанию 1 минута
        while (std::getline(query_stream, key, '=') && std::getline(query_stream, value, '&')) {
            if (key == "period") {
                period = std::stoi(value);
            }
        }

        if (period != 1 && period != 2) {
            return "HTTP/1.1 400 Bad Request\r\n\r\n";
        }

        auto since = std::chrono::system_clock::now() - std::chrono::minutes(period);
        double avg_temp = 0.0;
        {
            std::lock_guard<std::mutex> lock(db_mutex);
            avg_temp = database.get_average_temperature(since);
        }

        std::ostringstream oss;
        oss << "{ \"average_temperature\": " << avg_temp << ", \"period_minutes\": " << period << " }";

        std::string body = oss.str();
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Content-Length: " << body.length() << "\r\n"
                 << "Access-Control-Allow-Origin: *\r\n"
                 << "\r\n"
                 << body;
        return response.str();
    }
    else {
        return "HTTP/1.1 404 Not Found\r\n\r\n";
    }
}
