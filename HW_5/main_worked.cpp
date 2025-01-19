#include <iostream>
#include <sstream>
#include <vector>
#include <deque>
#include <string>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <thread>
#include <mutex>
#include <atomic>
#include <csignal>
#include <sqlite3.h>
#include "httplib.h" // Библиотека для HTTP-сервера (https://github.com/yhirose/cpp-httplib)

#ifdef _WIN32
    #include <windows.h>
#else
    #include <termios.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <errno.h>
#endif

std::mutex db_mutex; // Для синхронизации работы с БД
const std::string DB_NAME = "temperature_logs.db";
std::atomic<bool> running(true); // Флаг для завершения программы

// Инициализация базы данных
void init_database() {
    sqlite3* db;
    char* err_msg = nullptr;
    if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Ошибка открытия базы данных: " << sqlite3_errmsg(db) << std::endl;
        exit(1);
    }

    const char* create_table_query = R"(
        CREATE TABLE IF NOT EXISTS temperature_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp INTEGER NOT NULL,
            temperature REAL NOT NULL
        );
    )";

    if (sqlite3_exec(db, create_table_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка создания таблицы: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        sqlite3_close(db);
        exit(1);
    }

    sqlite3_close(db);
}

// Запись температуры в базу данных
void log_temperature(double temperature) {
    sqlite3* db;
    if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Ошибка открытия базы данных: " << sqlite3_errmsg(db) << std::endl;
        return;
    }

    const char* insert_query = "INSERT INTO temperature_logs (timestamp, temperature) VALUES (strftime('%s', 'now'), ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, insert_query, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Ошибка подготовки запроса: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return;
    }

    sqlite3_bind_double(stmt, 1, temperature);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "Ошибка выполнения запроса: " << sqlite3_errmsg(db) << std::endl;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

// Получение данных из базы за указанный период
std::vector<std::pair<std::time_t, double>> get_temperature_logs(std::time_t start, std::time_t end) {
    sqlite3* db;
    std::vector<std::pair<std::time_t, double>> logs;

    if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Ошибка открытия базы данных: " << sqlite3_errmsg(db) << std::endl;
        return logs;
    }

    const char* select_query = "SELECT timestamp, temperature FROM temperature_logs WHERE timestamp BETWEEN ? AND ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, select_query, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Ошибка подготовки запроса: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return logs;
    }

    sqlite3_bind_int64(stmt, 1, start);
    sqlite3_bind_int64(stmt, 2, end);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::time_t timestamp = sqlite3_column_int64(stmt, 0);
        double temperature = sqlite3_column_double(stmt, 1);
        logs.emplace_back(timestamp, temperature);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return logs;
}

// Запуск HTTP-сервера
void start_http_server() {
    httplib::Server svr;

    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"}
    });

    svr.Get("/temperature/current", [](const httplib::Request&, httplib::Response& res) {
        auto logs = get_temperature_logs(0, std::time(nullptr));
        if (!logs.empty()) {
            const auto& last = logs.back();
            res.set_content("{" + std::string("\"timestamp\":") + std::to_string(last.first) + ",\"temperature\":" + std::to_string(last.second) + "}", "application/json");
        } else {
            res.status = 404;
            res.set_content("{\"error\":\"No data available\"}", "application/json");
        }
    });

    svr.Get("/temperature/statistics", [](const httplib::Request& req, httplib::Response& res) {
        auto start = std::time(nullptr) - 24 * 60; // 24 минуты назад
        auto end = std::time(nullptr);
        auto logs = get_temperature_logs(start, end);

        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < logs.size(); ++i) {
            oss << "{\"timestamp\":" << logs[i].first << ",\"temperature\":" << logs[i].second << "}";
            if (i < logs.size() - 1) oss << ",";
        }
        oss << "]";
        res.set_content(oss.str(), "application/json");
    });

    std::cout << "HTTP сервер запущен на http://localhost:8080\n";
    svr.listen("0.0.0.0", 8080);
}

// Запуск потока для чтения данных
void data_reader_thread() {
    while (running) {
        // Симуляция получения данных температуры
        double simulated_temp = 20.0 + static_cast<double>(rand()) / RAND_MAX * 10.0;
        log_temperature(simulated_temp);

        // Каждую минуту (считаем среднюю температуру за 1 минуту)
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Очистка старых записей, если они старше 24 минут
        auto now = std::time(nullptr);
        auto oldest_valid_time = now - 24 * 60; // 24 минуты назад
        std::string delete_query = "DELETE FROM temperature_logs WHERE timestamp < " + std::to_string(oldest_valid_time);
        sqlite3* db;
        if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
            std::cerr << "Ошибка открытия базы данных: " << sqlite3_errmsg(db) << std::endl;
        }

        char* err_msg = nullptr;
        if (sqlite3_exec(db, delete_query.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
            std::cerr << "Ошибка очистки старых данных: " << err_msg << std::endl;
            sqlite3_free(err_msg);
        }

        sqlite3_close(db);
    }
}

// Обработчик сигналов
void signal_handler(int signal) {
    if (signal == SIGINT) {
        std::cout << "\nПолучен сигнал завершения. Остановка программы...\n";
        running = false;
    }
}

int main() {
    // Инициализация базы данных
    init_database();

    // Регистрация обработчика сигналов
    std::signal(SIGINT, signal_handler);

    // Запуск потоков
    std::thread reader_thread(data_reader_thread);
    start_http_server();

    // Ожидание завершения
    if (reader_thread.joinable()) {
        reader_thread.join();
    }

    return 0;
}
