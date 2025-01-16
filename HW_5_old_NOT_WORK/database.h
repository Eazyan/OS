// database.h
#pragma once
#include <string>
#include <chrono>

// Объявление структур SQLite
extern "C" {
    #include "sqlite3.h"
}

class Database {
public:
    Database(const std::string& db_name);
    ~Database();
    bool insert_measurement(double temperature, std::chrono::system_clock::time_point timestamp);
    double get_latest_temperature();
    double get_average_temperature(std::chrono::system_clock::time_point since);
private:
    sqlite3* db;
};
