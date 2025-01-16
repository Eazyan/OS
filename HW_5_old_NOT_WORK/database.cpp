// database.cpp
#include "database.h"
#include <iostream>

Database::Database(const std::string& db_name) : db(nullptr) {
    if (sqlite3_open(db_name.c_str(), &db)) {
        std::cerr << "Не удалось открыть базу данных: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        db = nullptr;
    } else {
        // Создание таблицы measurements, если она не существует
        const char* create_table_sql = 
            "CREATE TABLE IF NOT EXISTS measurements ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "temperature REAL NOT NULL, "
            "timestamp INTEGER NOT NULL);";
        char* errMsg = nullptr;
        if (sqlite3_exec(db, create_table_sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::cerr << "Ошибка создания таблицы: " << errMsg << std::endl;
            sqlite3_free(errMsg);
        }
    }
}

Database::~Database() {
    if (db) {
        sqlite3_close(db);
    }
}

bool Database::insert_measurement(double temperature, std::chrono::system_clock::time_point timestamp) {
    if (!db) return false;
    const char* insert_sql = "INSERT INTO measurements (temperature, timestamp) VALUES (?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Ошибка подготовки вставки: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    std::time_t time_t_timestamp = std::chrono::system_clock::to_time_t(timestamp);
    sqlite3_bind_double(stmt, 1, temperature);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(time_t_timestamp));

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "Ошибка вставки данных: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

double Database::get_latest_temperature() {
    if (!db) return 0.0;
    const char* query_sql = "SELECT temperature FROM measurements ORDER BY timestamp DESC LIMIT 1;";
    sqlite3_stmt* stmt;
    double temp = 0.0;

    if (sqlite3_prepare_v2(db, query_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Ошибка подготовки запроса: " << sqlite3_errmsg(db) << std::endl;
        return 0.0;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        temp = sqlite3_column_double(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return temp;
}

double Database::get_average_temperature(std::chrono::system_clock::time_point since) {
    if (!db) return 0.0;
    const char* query_sql = "SELECT AVG(temperature) FROM measurements WHERE timestamp >= ?;";
    sqlite3_stmt* stmt;
    double avg_temp = 0.0;

    if (sqlite3_prepare_v2(db, query_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Ошибка подготовки запроса: " << sqlite3_errmsg(db) << std::endl;
        return 0.0;
    }

    std::time_t time_since = std::chrono::system_clock::to_time_t(since);
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(time_since));

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!sqlite3_column_type(stmt, 0) == SQLITE_NULL) {
            avg_temp = sqlite3_column_double(stmt, 0);
        }
    }

    sqlite3_finalize(stmt);
    return avg_temp;
}
