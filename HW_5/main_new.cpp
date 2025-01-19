#include <iostream>
#include <vector>
#include <deque>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <csignal>
#include <sqlite3.h>
#include <sstream>
#include <iomanip>

std::atomic<bool> running(true);
std::mutex db_mutex;

const std::string DB_NAME = "temperature.db";

struct Measurement {
    double temperature;
    std::chrono::system_clock::time_point timestamp;
};

// Инициализация базы данных
void init_database() {
    sqlite3* db;
    char* err_msg = nullptr;
    if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Ошибка открытия базы данных: " << sqlite3_errmsg(db) << "\n";
        exit(1);
    }

    const char* create_tables = R"(
        CREATE TABLE IF NOT EXISTS measurements (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp INTEGER NOT NULL,
            temperature REAL NOT NULL
        );

        CREATE TABLE IF NOT EXISTS hourly_averages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp INTEGER NOT NULL,
            avg_temperature REAL NOT NULL
        );

        CREATE TABLE IF NOT EXISTS daily_averages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp INTEGER NOT NULL,
            avg_temperature REAL NOT NULL
        );
    )";

    if (sqlite3_exec(db, create_tables, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка создания таблиц: " << err_msg << "\n";
        sqlite3_free(err_msg);
        sqlite3_close(db);
        exit(1);
    }

    sqlite3_close(db);
}

// Запись данных в базу
void log_measurement(double temperature) {
    sqlite3* db;
    if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Ошибка открытия базы данных: " << sqlite3_errmsg(db) << "\n";
        return;
    }

    const char* insert_query = "INSERT INTO measurements (timestamp, temperature) VALUES (strftime('%s', 'now'), ?);";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db, insert_query, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Ошибка подготовки запроса: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        return;
    }

    sqlite3_bind_double(stmt, 1, temperature);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "Ошибка выполнения запроса: " << sqlite3_errmsg(db) << "\n";
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

// Очистка устаревших данных
void cleanup_database() {
    sqlite3* db;
    if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Ошибка открытия базы данных: " << sqlite3_errmsg(db) << "\n";
        return;
    }

    const char* delete_measurements = "DELETE FROM measurements WHERE timestamp < strftime('%s', 'now', '-1 day');";
    const char* delete_hourly = "DELETE FROM hourly_averages WHERE timestamp < strftime('%s', 'now', '-1 month');";
    const char* delete_daily = "DELETE FROM daily_averages WHERE timestamp < strftime('%s', 'now', '-1 year');";

    char* err_msg = nullptr;

    if (sqlite3_exec(db, delete_measurements, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка очистки measurements: " << err_msg << "\n";
        sqlite3_free(err_msg);
    }

    if (sqlite3_exec(db, delete_hourly, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка очистки hourly_averages: " << err_msg << "\n";
        sqlite3_free(err_msg);
    }

    if (sqlite3_exec(db, delete_daily, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка очистки daily_averages: " << err_msg << "\n";
        sqlite3_free(err_msg);
    }

    sqlite3_close(db);
}

// Подсчёт и запись средних значений
void calculate_averages() {
    sqlite3* db;
    if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Ошибка открытия базы данных: " << sqlite3_errmsg(db) << "\n";
        return;
    }

    const char* hourly_query = R"(
        INSERT INTO hourly_averages (timestamp, avg_temperature)
        SELECT strftime('%s', 'now'), AVG(temperature)
        FROM measurements
        WHERE timestamp >= strftime('%s', 'now', '-1 hour');
    )";

    const char* daily_query = R"(
        INSERT INTO daily_averages (timestamp, avg_temperature)
        SELECT strftime('%s', 'now'), AVG(temperature)
        FROM measurements
        WHERE timestamp >= strftime('%s', 'now', '-1 day');
    )";

    char* err_msg = nullptr;

    if (sqlite3_exec(db, hourly_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка подсчёта hourly_averages: " << err_msg << "\n";
        sqlite3_free(err_msg);
    }

    if (sqlite3_exec(db, daily_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка подсчёта daily_averages: " << err_msg << "\n";
        sqlite3_free(err_msg);
    }

    sqlite3_close(db);
}

// Поток обработки данных
void process_data() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::minutes(1));
        cleanup_database();
        calculate_averages();
    }
}

void signal_handler(int signal) {
    if (signal == SIGINT) {
        running = false;
    }
}

int main() {
    std::signal(SIGINT, signal_handler);
    init_database();

    std::thread processing_thread(process_data);

    while (running) {
        // Симуляция данных температуры
        double simulated_temp = 20.0 + static_cast<double>(rand()) / RAND_MAX * 10.0;
        log_measurement(simulated_temp);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (processing_thread.joinable()) {
        processing_thread.join();
    }

    return 0;
}
