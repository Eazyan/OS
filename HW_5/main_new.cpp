#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <ctime>
#include <mutex>
#include <thread>
#include <atomic>
#include <sqlite3.h>
#include "httplib.h" // HTTP-библиотека

std::mutex db_mutex;
const std::string DB_NAME = "temperature_logs.db";
std::atomic<bool> running(true);

// Функция для инициализации базы данных
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

        CREATE TABLE IF NOT EXISTS hourly_averages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            hour_start INTEGER NOT NULL,
            avg_temperature REAL NOT NULL
        );

        CREATE TABLE IF NOT EXISTS daily_averages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            day_start INTEGER NOT NULL,
            avg_temperature REAL NOT NULL
        );
    )";

    if (sqlite3_exec(db, create_table_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка создания таблиц: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        sqlite3_close(db);
        exit(1);
    }

    sqlite3_close(db);
}

// Запись температуры в общий лог
void log_temperature(double temperature) {
    std::lock_guard<std::mutex> lock(db_mutex);
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

// Рассчитать средние температуры за последний час и день, и записать в соответствующие таблицы
void calculate_averages() {
    std::lock_guard<std::mutex> lock(db_mutex);
    sqlite3* db;

    if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Ошибка открытия базы данных: " << sqlite3_errmsg(db) << std::endl;
        return;
    }

    // Рассчитываем среднюю температуру за последние 24 минуты (1 "час" по времени)
    const char* hourly_query = R"(
        INSERT INTO hourly_averages (hour_start, avg_temperature)
        SELECT strftime('%s', 'now', '-24 minutes'), COALESCE(AVG(temperature), 0)
        FROM temperature_logs
        WHERE timestamp >= strftime('%s', 'now', '-24 minutes')
        AND timestamp < strftime('%s', 'now');
    )";

    // Рассчитываем среднюю температуру за последние 24 "часа" (1 сутки, 1440 минут)
    const char* daily_query = R"(
        INSERT INTO daily_averages (day_start, avg_temperature)
        SELECT strftime('%s', 'now', '-1440 minutes'), COALESCE(AVG(avg_temperature), 0)
        FROM hourly_averages
        WHERE hour_start >= strftime('%s', 'now', '-1440 minutes')
        AND hour_start < strftime('%s', 'now');
    )";

    char* err_msg = nullptr;

    // Записываем средние за последний час
    if (sqlite3_exec(db, hourly_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка подсчета hourly_averages: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    // Записываем средние за последний день
    if (sqlite3_exec(db, daily_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка подсчета daily_averages: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    sqlite3_close(db);
}

// Очистка старых записей из базы данных
void cleanup_old_records() {
    std::lock_guard<std::mutex> lock(db_mutex);
    sqlite3* db;

    if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Ошибка открытия базы данных: " << sqlite3_errmsg(db) << std::endl;
        return;
    }

    // Удаление старых данных (старше 24 минут)
    const char* cleanup_logs = "DELETE FROM temperature_logs WHERE timestamp < strftime('%s', 'now', '-24 minutes');";
    const char* cleanup_hourly = "DELETE FROM hourly_averages WHERE hour_start < strftime('%s', 'now', '-720 minutes');";
    const char* cleanup_daily = "DELETE FROM daily_averages WHERE day_start < strftime('%s', 'now', '-8760 minutes');";

    char* err_msg = nullptr;

    // Удаляем старые записи из temperature_logs (старше 24 минут)
    if (sqlite3_exec(db, cleanup_logs, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка очистки temperature_logs: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    // Удаляем старые записи из hourly_averages (старше 30 дней)
    if (sqlite3_exec(db, cleanup_hourly, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка очистки hourly_averages: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    // Удаляем старые записи из daily_averages (старше 1 года)
    if (sqlite3_exec(db, cleanup_daily, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка очистки daily_averages: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    sqlite3_close(db);
}


// Запуск HTTP-сервера
void start_http_server() {
    httplib::Server svr;

    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"}
    });

    svr.Get("/temperature/all", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        sqlite3* db;
        if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
            res.status = 500;
            res.set_content("{\"error\":\"Ошибка открытия базы данных\"}", "application/json");
            return;
        }

        const char* query = "SELECT timestamp, temperature FROM temperature_logs ORDER BY timestamp ASC;";
        sqlite3_stmt* stmt;
        std::ostringstream oss;
        oss << "[";
        bool first = true;

        if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) {
                    oss << ",";
                }
                first = false;
                oss << "{\"timestamp\":" << sqlite3_column_int64(stmt, 0) << ",\"temperature\":" << sqlite3_column_double(stmt, 1) << "}";
            }
            sqlite3_finalize(stmt);
        }

        sqlite3_close(db);
        oss << "]";
        res.set_content(oss.str(), "application/json");
    });

    svr.Get("/temperature/hourly", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        sqlite3* db;
        if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
            res.status = 500;
            res.set_content("{\"error\":\"Ошибка открытия базы данных\"}", "application/json");
            return;
        }

        const char* query = "SELECT hour_start, avg_temperature FROM hourly_averages ORDER BY hour_start ASC;";
        sqlite3_stmt* stmt;
        std::ostringstream oss;
        oss << "[";
        bool first = true;

        if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) {
                    oss << ",";
                }
                first = false;
                oss << "{\"timestamp\":" << sqlite3_column_int64(stmt, 0) << ",\"avg_temperature\":" << sqlite3_column_double(stmt, 1) << "}";
            }
            sqlite3_finalize(stmt);
        }

        sqlite3_close(db);
        oss << "]";
        res.set_content(oss.str(), "application/json");
    });

    svr.Get("/temperature/daily", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        sqlite3* db;
        if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
            res.status = 500;
            res.set_content("{\"error\":\"Ошибка открытия базы данных\"}", "application/json");
            return;
        }

        const char* query = "SELECT day_start, avg_temperature FROM daily_averages ORDER BY day_start ASC;";
        sqlite3_stmt* stmt;
        std::ostringstream oss;
        oss << "[";
        bool first = true;

        if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) {
                    oss << ",";
                }
                first = false;
                oss << "{\"timestamp\":" << sqlite3_column_int64(stmt, 0) << ",\"avg_temperature\":" << sqlite3_column_double(stmt, 1) << "}";
            }
            sqlite3_finalize(stmt);
        }

        sqlite3_close(db);
        oss << "]";
        res.set_content(oss.str(), "application/json");
    });

    svr.listen("0.0.0.0", 8080);
}

// Поток обработки данных
void data_reader_thread() {
    while (running) {
        // Симулируем температуру для тестирования
        double simulated_temp = 20.0 + static_cast<double>(rand()) / RAND_MAX * 10.0;
        log_temperature(simulated_temp);  // Записываем температуру

        cleanup_old_records();  // Очищаем старые записи
        calculate_averages();   // Рассчитываем средние

        std::this_thread::sleep_for(std::chrono::seconds(1));  // Пауза в 1 секунду
    }
}

void signal_handler(int signal) {
    if (signal == SIGINT) {
        running = false;
    }
}

int main() {
    init_database();
    std::signal(SIGINT, signal_handler);

    std::thread reader_thread(data_reader_thread);
    start_http_server();

    if (reader_thread.joinable()) {
        reader_thread.join();
    }

    return 0;
}
