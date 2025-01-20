#include <iostream>
#include <sqlite3.h>
#include <string>

// Имя базы данных
const std::string DB_NAME = "temperature_logs.db";

void recalculate_averages() {
    sqlite3* db;
    char* err_msg = nullptr;

    // Открытие базы данных
    if (sqlite3_open(DB_NAME.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Ошибка открытия базы данных: " << sqlite3_errmsg(db) << std::endl;
        return;
    }

    // Очистка таблиц `hourly_averages` и `daily_averages`
    const char* clear_tables_query = R"(
        DELETE FROM hourly_averages;
        DELETE FROM daily_averages;
    )";

    if (sqlite3_exec(db, clear_tables_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка очистки таблиц: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return;
    }

    std::cout << "Таблицы hourly_averages и daily_averages очищены.\n";

    // Перерасчет средних значений за час
    const char* recalculate_hourly_query = R"(
        INSERT INTO hourly_averages (timestamp, avg_temperature)
        SELECT strftime('%s', datetime(timestamp, 'unixepoch', 'start of hour')) AS hour_start,
               AVG(temperature) AS avg_temp
        FROM measurements
        GROUP BY hour_start;
    )";

    if (sqlite3_exec(db, recalculate_hourly_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка перерасчета hourly_averages: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return;
    }

    std::cout << "Средние значения за час перерасчитаны.\n";

    // Перерасчет средних значений за день
    const char* recalculate_daily_query = R"(
        INSERT INTO daily_averages (timestamp, avg_temperature)
        SELECT strftime('%s', datetime(timestamp, 'unixepoch', 'start of day')) AS day_start,
               AVG(temperature) AS avg_temp
        FROM measurements
        GROUP BY day_start;
    )";

    if (sqlite3_exec(db, recalculate_daily_query, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка перерасчета daily_averages: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return;
    }

    std::cout << "Средние значения за день перерасчитаны.\n";

    // Закрытие базы данных
    sqlite3_close(db);
    std::cout << "Перерасчет завершен успешно.\n";
}

int main() {
    recalculate_averages();
    return 0;
}
