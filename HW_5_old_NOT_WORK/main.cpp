// main.cpp

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <cstring>

#ifdef _WIN32
    #define _WIN32_WINNT 0x0601
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <termios.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <errno.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
#endif

extern "C" {
    #include "sqlite3.h"
}

// Названия лог-файлов
const std::string LOG_ALL = "log_all_measurements.log";
const std::string LOG_HOURLY = "log_hourly_averages.log";
const std::string LOG_DAILY = "log_daily_averages.log";

// Название базы данных
const std::string DB_NAME = "temperature_data.db";

// Порт для HTTP-сервера
const int HTTP_PORT = 18080;

// Мьютексы для синхронизации
std::mutex log_mutex;
std::mutex db_mutex;

// Структура для хранения измерений
struct Measurement {
    double temperature;
    std::chrono::system_clock::time_point timestamp;
};

// Класс для работы с базой данных SQLite
class Database {
public:
    Database(const std::string& db_name) : db(nullptr) {
        int rc = sqlite3_open(db_name.c_str(), &db);
        if (rc) {
            std::cerr << "Не удалось открыть базу данных: " << sqlite3_errmsg(db) << "\n";
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
            rc = sqlite3_exec(db, create_table_sql, nullptr, nullptr, &errMsg);
            if (rc != SQLITE_OK) {
                std::cerr << "Ошибка создания таблицы: " << errMsg << "\n";
                sqlite3_free(errMsg);
            }
        }
    }

    ~Database() {
        if (db) {
            sqlite3_close(db);
        }
    }

    bool insert_measurement(double temperature, std::chrono::system_clock::time_point timestamp) {
        if (!db) return false;
        const char* insert_sql = "INSERT INTO measurements (temperature, timestamp) VALUES (?, ?);";
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Ошибка подготовки вставки: " << sqlite3_errmsg(db) << "\n";
            return false;
        }

        std::time_t time_t_timestamp = std::chrono::system_clock::to_time_t(timestamp);
        sqlite3_bind_double(stmt, 1, temperature);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(time_t_timestamp));

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            std::cerr << "Ошибка вставки данных: " << sqlite3_errmsg(db) << "\n";
            sqlite3_finalize(stmt);
            return false;
        }

        sqlite3_finalize(stmt);
        return true;
    }

    double get_latest_temperature() {
        if (!db) return 0.0;
        const char* query_sql = "SELECT temperature FROM measurements ORDER BY timestamp DESC LIMIT 1;";
        sqlite3_stmt* stmt;
        double temp = 0.0;

        int rc = sqlite3_prepare_v2(db, query_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Ошибка подготовки запроса: " << sqlite3_errmsg(db) << "\n";
            return 0.0;
        }

        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            temp = sqlite3_column_double(stmt, 0);
        }

        sqlite3_finalize(stmt);
        return temp;
    }

    double get_average_temperature(std::chrono::system_clock::time_point since) {
        if (!db) return 0.0;
        const char* query_sql = "SELECT AVG(temperature) FROM measurements WHERE timestamp >= ?;";
        sqlite3_stmt* stmt;
        double avg_temp = 0.0;

        int rc = sqlite3_prepare_v2(db, query_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Ошибка подготовки запроса: " << sqlite3_errmsg(db) << "\n";
            return 0.0;
        }

        std::time_t time_since = std::chrono::system_clock::to_time_t(since);
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(time_since));

        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
                avg_temp = sqlite3_column_double(stmt, 0);
            }
        }

        sqlite3_finalize(stmt);
        return avg_temp;
    }

private:
    sqlite3* db;
};

// Функция записи в лог-файл
void write_log(const std::string& filename, const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex); // Защита от одновременной записи
    std::ofstream log_file(filename, std::ios::app);
    if (log_file) {
        log_file << message << std::endl;
    } else {
        std::cerr << "Не удалось открыть файл " << filename << " для записи.\n";
    }
}

// Очистка устаревших записей
void cleanup_log(const std::string& filename, std::chrono::system_clock::time_point cutoff_time) {
    std::lock_guard<std::mutex> lock(log_mutex); // Защита от одновременной записи
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        // Не выводим ошибку, если файл не существует
        std::cerr << "Файл " << filename << " не существует. Пропуск очистки.\n";
        return;
    }

    std::ofstream temp_file("temp.log", std::ios::out | std::ios::trunc);
    std::string line;
    while (std::getline(infile, line)) {
        std::istringstream iss(line);
        std::time_t timestamp;
        double temp;
        iss >> timestamp >> temp;
        auto entry_time = std::chrono::system_clock::from_time_t(timestamp);
        if (entry_time >= cutoff_time) {
            temp_file << line << std::endl;
        }
    }
    infile.close();
    temp_file.close();

    // Переименовываем временный файл обратно
#ifdef _WIN32
    // Удаляем оригинальный файл
    if (!DeleteFileA(filename.c_str())) {
        std::cerr << "Не удалось удалить файл " << filename << ".\n";
    }
    // Переименовываем temp.log в оригинальное название
    if (!MoveFileA("temp.log", filename.c_str())) {
        std::cerr << "Не удалось переименовать temp.log в " << filename << ".\n";
    }
#else
    if (remove(filename.c_str()) != 0) {
        perror(("Ошибка удаления файла " + filename).c_str());
    }
    if (rename("temp.log", filename.c_str()) != 0) {
        perror(("Ошибка переименования файла temp.log в " + filename).c_str());
    }
#endif
}

// Функция для получения текущего времени в строковом формате
std::string get_current_time_str() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm_struct;
#ifdef _WIN32
    localtime_s(&tm_struct, &now_c);
#else
    localtime_r(&now_c, &tm_struct);
#endif
    char buffer[100];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_struct);
    return std::string(buffer);
}

// Функция для вычисления среднего значения
double calculate_average(const std::vector<double>& values) {
    double sum = 0.0;
    for (double v : values) {
        sum += v;
    }
    return values.empty() ? 0.0 : sum / values.size();
}

#ifdef _WIN32
// Функции для работы с последовательным портом на Windows
HANDLE open_serial_port(const std::string& port_name, int baud_rate) {
    HANDLE hSerial = CreateFileA(port_name.c_str(),
                                 GENERIC_READ | GENERIC_WRITE,
                                 0,
                                 NULL,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL,
                                 NULL);
    if (hSerial == INVALID_HANDLE_VALUE) {
        std::cerr << "Не удалось открыть порт " << port_name << ".\n";
        return INVALID_HANDLE_VALUE;
    }

    DCB dcbSerialParams = {0};
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);

    if (!GetCommState(hSerial, &dcbSerialParams)) {
        std::cerr << "Не удалось получить состояние порта.\n";
        CloseHandle(hSerial);
        return INVALID_HANDLE_VALUE;
    }

    dcbSerialParams.BaudRate = baud_rate;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity   = NOPARITY;

    if (!SetCommState(hSerial, &dcbSerialParams)) {
        std::cerr << "Не удалось установить параметры порта.\n";
        CloseHandle(hSerial);
        return INVALID_HANDLE_VALUE;
    }

    // Установка таймаутов
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;

    if (!SetCommTimeouts(hSerial, &timeouts)) {
        std::cerr << "Не удалось установить таймауты.\n";
        CloseHandle(hSerial);
        return INVALID_HANDLE_VALUE;
    }

    return hSerial;
}

bool read_serial_port(HANDLE hSerial, char* buffer, DWORD buf_size, DWORD& bytes_read) {
    return ReadFile(hSerial, buffer, buf_size, &bytes_read, NULL);
}

void close_serial_port(HANDLE hSerial) {
    CloseHandle(hSerial);
}

#else
// Функции для работы с последовательным портом на Unix
int open_serial_port(const std::string& port_name, int baud_rate) {
    int fd = open(port_name.c_str(), O_RDONLY | O_NOCTTY | O_NDELAY);
    if (fd == -1) {
        perror(("Не удалось открыть порт " + port_name).c_str());
        return -1;
    }

    // Установка блокирующего режима
    if (fcntl(fd, F_SETFL, 0) == -1) {
        perror("Не удалось установить блокирующий режим");
        close(fd);
        return -1;
    }

    struct termios options;
    if (tcgetattr(fd, &options) == -1) {
        perror("Не удалось получить атрибуты порта");
        close(fd);
        return -1;
    }

    // Установка скорости
    speed_t baud;
    switch (baud_rate) {
        case 9600: baud = B9600; break;
        case 19200: baud = B19200; break;
        case 38400: baud = B38400; break;
        case 57600: baud = B57600; break;
        case 115200: baud = B115200; break;
        default: baud = B9600; break;
    }

    cfsetispeed(&options, baud);
    cfsetospeed(&options, baud);

    // 8N1
    options.c_cflag &= ~PARENB; // Без четности
    options.c_cflag &= ~CSTOPB; // 1 стоп-бит
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;      // 8 бит данных

    // Без управления потоком
    options.c_cflag &= ~CRTSCTS;

    // Режимы ввода
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    // Режимы вывода
    options.c_oflag &= ~OPOST;

    // Применение настроек
    if (tcsetattr(fd, TCSANOW, &options) == -1) {
        perror("Не удалось установить атрибуты порта");
        close(fd);
        return -1;
    }

    return fd;
}

bool read_serial_port(int fd, char* buffer, size_t buf_size, ssize_t& bytes_read) {
    bytes_read = read(fd, buffer, buf_size);
    return bytes_read > 0;
}

void close_serial_port(int fd) {
    close(fd);
}
#endif

// Функция для получения текущего времени в формате time_t
std::time_t get_current_time_t() {
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

// Функция для запуска HTTP-сервера
void run_http_server(Database& db, std::atomic<bool>& server_running) {
#ifdef _WIN32
    // Инициализация Winsock
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (iResult != 0) {
        std::cerr << "WSAStartup failed: " << iResult << "\n";
        server_running = false;
        return;
    }
#endif

    // Создание сокета
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Ошибка создания сокета.\n";
        server_running = false;
        return;
    }

    // Привязка сокета к порту
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(HTTP_PORT); // Порт 18080

    // Установка опции SO_REUSEADDR
    int opt = 1;
#ifdef _WIN32
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    if (
#ifdef _WIN32
        bind(server_fd, (struct sockaddr*)&address, sizeof(address)) == SOCKET_ERROR
#else
        bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0
#endif
    ) {
        std::cerr << "Не удалось привязать сокет к порту.\n";
#ifdef _WIN32
        closesocket(server_fd);
        WSACleanup();
#else
        close(server_fd);
#endif
        server_running = false;
        return;
    }

    // Прослушивание входящих соединений
    if (
#ifdef _WIN32
        listen(server_fd, SOMAXCONN) == SOCKET_ERROR
#else
        listen(server_fd, 10) < 0
#endif
    ) {
        std::cerr << "Ошибка прослушивания сокета.\n";
#ifdef _WIN32
        closesocket(server_fd);
        WSACleanup();
#else
        close(server_fd);
#endif
        server_running = false;
        return;
    }

    std::cout << "HTTP-сервер запущен на порту " << HTTP_PORT << "\n";

    while (server_running) {
        struct sockaddr_in client_addr;
#ifdef _WIN32
        int client_len = sizeof(client_addr);
        SOCKET client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket == INVALID_SOCKET) {
            if (server_running) {
                std::cerr << "Ошибка принятия соединения.\n";
            }
            continue;
        }
#else
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (server_running) {
                std::cerr << "Ошибка принятия соединения.\n";
            }
            continue;
        }
#endif

        // Обработка клиента в отдельном потоке
        std::thread([client_socket, &db]() {
            char buffer[1024];
            std::memset(buffer, 0, sizeof(buffer));
#ifdef _WIN32
            int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
            if (bytes_received > 0) {
#else
            ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
            if (bytes_received > 0) {
#endif
                std::string request(buffer);
                std::istringstream request_stream(request);
                std::string method, path;
                request_stream >> method >> path;

                std::string response;

                if (method != "GET") {
                    response = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
                }
                else {
                    if (path == "/current") {
                        // Получение последней температуры
                        double current_temp = 0.0;
                        {
                            std::lock_guard<std::mutex> lock(db_mutex);
                            current_temp = db.get_latest_temperature();
                        }

                        // Формирование JSON-ответа
                        std::ostringstream oss;
                        oss << "{ \"current_temperature\": " << current_temp << " }";

                        std::string body = oss.str();
                        response = "HTTP/1.1 200 OK\r\n";
                        response += "Content-Type: application/json\r\n";
                        response += "Content-Length: " + std::to_string(body.length()) + "\r\n";
                        response += "Access-Control-Allow-Origin: *\r\n";
                        response += "\r\n";
                        response += body;
                    }
                    else if (path.find("/average") == 0) {
                        // Обработка запроса /average?period=1 или /average?period=2
                        size_t pos = path.find("?");
                        if (pos == std::string::npos) {
                            response = "HTTP/1.1 400 Bad Request\r\n\r\n";
                        }
                        else {
                            std::string query = path.substr(pos + 1);
                            std::istringstream query_stream(query);
                            std::string key, value;
                            int period = 1; // По умолчанию 1 минута

                            if (std::getline(query_stream, key, '=') && std::getline(query_stream, value, '&')) {
                                if (key == "period") {
                                    try {
                                        period = std::stoi(value);
                                    }
                                    catch (...) {
                                        period = 1;
                                    }
                                }
                            }

                            if (period != 1 && period != 2) {
                                response = "HTTP/1.1 400 Bad Request\r\n\r\n";
                            }
                            else {
                                auto since = std::chrono::system_clock::now() - std::chrono::minutes(period);
                                double avg_temp = 0.0;
                                {
                                    std::lock_guard<std::mutex> lock(db_mutex);
                                    avg_temp = db.get_average_temperature(since);
                                }

                                // Формирование JSON-ответа
                                std::ostringstream oss;
                                oss << "{ \"average_temperature\": " << avg_temp 
                                    << ", \"period_minutes\": " << period << " }";

                                std::string body = oss.str();
                                response = "HTTP/1.1 200 OK\r\n";
                                response += "Content-Type: application/json\r\n";
                                response += "Content-Length: " + std::to_string(body.length()) + "\r\n";
                                response += "Access-Control-Allow-Origin: *\r\n";
                                response += "\r\n";
                                response += body;
                            }
                        }
                    }
                    else {
                        response = "HTTP/1.1 404 Not Found\r\n\r\n";
                    }
                }

                // Отправка ответа клиенту
#ifdef _WIN32
                send(client_socket, response.c_str(), static_cast<int>(response.length()), 0);
                closesocket(client_socket);
#else
                send(client_socket, response.c_str(), response.length(), 0);
                close(client_socket);
#endif
            }
#ifdef _WIN32
            else {
                closesocket(client_socket);
            }
#else
            else {
                close(client_socket);
            }
#endif
        }).detach();
    }

#ifdef _WIN32
    // Функция для запуска HTTP-сервера
    void start_http_server(Database& db, std::atomic<bool>& server_running) {
        std::thread server_thread(run_http_server, std::ref(db), std::ref(server_running));
        server_thread.detach();
    }
#else
    void start_http_server(Database& db, std::atomic<bool>& server_running) {
        std::thread server_thread(run_http_server, std::ref(db), std::ref(server_running));
        server_thread.detach();
    }
#endif

// Функция обработки измерений и записи средних значений
void process_measurements(std::deque<Measurement>& measurements_deque, Database& db, std::atomic<bool>& server_running) {
    int counter = 0; // Счётчик для определения, когда выполнять расчёт среднего

    while (server_running) {
        // Выполняем раз в минуту
        std::this_thread::sleep_for(std::chrono::minutes(1));

        auto now = std::chrono::system_clock::now();
        auto cutoff_minute = now - std::chrono::minutes(1);  // за последнюю минуту
        auto cutoff_2minutes = now - std::chrono::minutes(2); // за последние 2 минуты

        // Вычисление среднего за 1 минуту
        double avg_1 = 0.0;
        {
            std::lock_guard<std::mutex> lock(db_mutex);
            avg_1 = db.get_average_temperature(cutoff_minute);
        }

        // Запись среднего в базу данных
        {
            std::lock_guard<std::mutex> lock(db_mutex);
            db.insert_measurement(avg_1, now);
        }

        // Запись в LOG_HOURLY
        {
            std::ostringstream oss_hourly;
            oss_hourly << std::chrono::system_clock::to_time_t(now) << " " << avg_1;
            write_log(LOG_HOURLY, oss_hourly.str());
        }

        std::cout << "Среднее за 1 минуту записано: " << avg_1 << "°C\n";

        counter++;

        // Каждые 2 минуты вычисляем среднее за 2 минуты
        if (counter % 2 == 0) {
            double avg_2 = 0.0;
            {
                std::lock_guard<std::mutex> lock(db_mutex);
                avg_2 = db.get_average_temperature(cutoff_2minutes);
            }

            // Запись среднего в базу данных
            {
                std::lock_guard<std::mutex> lock(db_mutex);
                db.insert_measurement(avg_2, now);
            }

            // Запись в LOG_DAILY
            {
                std::ostringstream oss_daily;
                oss_daily << std::chrono::system_clock::to_time_t(now) << " " << avg_2;
                write_log(LOG_DAILY, oss_daily.str());
            }

            std::cout << "Среднее за 2 минуты записано: " << avg_2 << "°C\n";
        }

        // Очистка LOG_ALL (хранение последних 2 минут)
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            while (!measurements_deque.empty() && measurements_deque.front().timestamp < cutoff_2minutes) {
                measurements_deque.pop_front();
            }
        }

        // Очистка LOG_HOURLY (хранение последних 30 минут) - для дебага
        auto cutoff_30minutes = now - std::chrono::minutes(30);
        cleanup_log(LOG_HOURLY, cutoff_30minutes);

        // Очистка LOG_DAILY (хранение последних 60 минут) - для дебага
        auto cutoff_60minutes = now - std::chrono::minutes(60);
        cleanup_log(LOG_DAILY, cutoff_60minutes);
    }
}

int main(int argc, char* argv[]) {
    // Проверка аргументов
    if (argc < 2) {
        std::cout << "Использование: " << argv[0] << " <serial_port> [baud_rate]\n";
        std::cout << "Пример: " << argv[0] << " COM4 9600        (Windows)\n";
        std::cout << "        " << argv[0] << " /dev/ttyS7 9600  (Linux/macOS)\n";
        return 1;
    }

    std::string port_name = argv[1];
    int baud_rate = 9600;
    if (argc >= 3) {
        try {
            baud_rate = std::stoi(argv[2]);
        }
        catch (...) {
            std::cerr << "Некорректное значение baud_rate. Используется 9600.\n";
            baud_rate = 9600;
        }
    }

    // Открытие последовательного порта
#ifdef _WIN32
    HANDLE hSerial = open_serial_port(port_name, baud_rate);
    if (hSerial == INVALID_HANDLE_VALUE) {
        return 1;
    }
#else
    int fd = open_serial_port(port_name, baud_rate);
    if (fd == -1) {
        return 1;
    }
#endif

    // Инициализация базы данных
    Database db(DB_NAME);

    // Очередь для хранения всех измерений
    std::deque<Measurement> measurements_deque;

    // Флаг для управления работой сервера и потоков
    std::atomic<bool> server_running(true);

    // Запуск HTTP-сервера
    start_http_server(db, server_running);

    // Запуск потока для обработки измерений
    std::thread processor_thread(process_measurements, std::ref(measurements_deque), std::ref(db), std::ref(server_running));

    // Основной цикл чтения данных
    while (server_running) {
        char buffer[256];
#ifdef _WIN32
        DWORD bytes_read = 0;
        bool success = read_serial_port(hSerial, buffer, sizeof(buffer) - 1, bytes_read);
#else
        ssize_t bytes_read = 0;
        bool success = read_serial_port(fd, buffer, sizeof(buffer) - 1, bytes_read);
#endif
        if (success && bytes_read > 0) {
            buffer[bytes_read] = '\0'; // Завершаем строку
            std::string data(buffer);

            // Разделение данных на строки
            std::istringstream iss(data);
            std::string line;
            while (std::getline(iss, line)) {
                if (line.empty()) continue;
                try {
                    double temp = std::stod(line);
                    auto now = std::chrono::system_clock::now();
                    Measurement m;
                    m.temperature = temp;
                    m.timestamp = now;

                    {
                        std::lock_guard<std::mutex> lock(log_mutex);
                        measurements_deque.emplace_back(m);
                    }

                    // Запись в LOG_ALL
                    std::ostringstream oss;
                    oss << std::chrono::system_clock::to_time_t(now) << " " << temp;
                    write_log(LOG_ALL, oss.str());

                    // Запись в базу данных
                    {
                        std::lock_guard<std::mutex> lock(db_mutex);
                        db.insert_measurement(temp, now);
                    }

                    std::cout << "Измерение записано: " << temp << "°C\n";
                }
                catch (const std::exception& e) {
                    std::cerr << "Ошибка парсинга строки: " << line << "\n";
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Пауза для снижения загрузки CPU
    }

    // Закрытие последовательного порта и завершение потоков
#ifdef _WIN32
    close_serial_port(hSerial);
#else
    close_serial_port(fd);
#endif
    processor_thread.join();

    // Остановка HTTP-сервера
    server_running = false;
#ifdef _WIN32
    // Для завершения accept на Windows, можно использовать закрытие сокета или другой механизм
    // В данном случае сервер завершится после завершения основной программы
#endif

    std::cout << "Программа завершена.\n";
    return 0;
}
