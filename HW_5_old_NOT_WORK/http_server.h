// http_server.h
#pragma once
#include "database.h"
#include <thread>
#include <atomic>

class HttpServer {
public:
    HttpServer(Database& db, int port);
    ~HttpServer();
    void start();
    void stop();
private:
    void run();
    void handle_client(int client_socket);
    std::string handle_request(const std::string& request);
    Database& database;
    int server_port;
    std::thread server_thread;
    std::atomic<bool> is_running;
};
