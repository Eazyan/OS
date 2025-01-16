// main_program.cpp
#include <iostream>
#include <fstream>
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

#ifdef _WIN32
    #include <windows.h>
#else
    #include <termios.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <errno.h>
#endif

// Названия лог-файлов
const std::string LOG_ALL = "log_all_measurements.log";
const std::string LOG_HOURLY = "log_hourly_averages.log";
const std::string LOG_DAILY = "log_daily_averages.log";

// Мьютекс для синхронизации
std::mutex log_mutex;

// Структура для хранения измерений
struct Measurement {
    double temperature;
    std::chrono::system_clock::time_point timestamp;
};

// Атомарный флаг для управления завершением программы
std::atomic<bool> running(true);

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
        // Не выводить ошибку, если файл не существует
        std::cerr << "Файл " << filename << " не существует. Пропуск очистки.\n";
        return;
    }

    // Создаём уникальное имя временного файла для каждого лог-файла
    std::string temp_filename = "temp_" + filename;

    std::ofstream temp_file(temp_filename, std::ios::out | std::ios::trunc);
    if (!temp_file.is_open()) {
        std::cerr << "Не удалось создать временный файл " << temp_filename << " для очистки.\n";
        infile.close();
        return;
    }

    std::string line;
    while (std::getline(infile, line)) {
        std::istringstream iss(line);
        double temp;
        std::time_t timestamp;
        iss >> timestamp >> temp;
        auto entry_time = std::chrono::system_clock::from_time_t(timestamp);
        if (entry_time >= cutoff_time) {
            temp_file << line << std::endl;
        }
    }
    infile.close();
    temp_file.close();

    // Переименовываем временный файл обратно на оригинальное имя
#ifdef _WIN32
    // Удаляем оригинальный файл
    if (!DeleteFileA(filename.c_str())) {
        std::cerr << "Не удалось удалить файл " << filename << ".\n";
    }
    // Переименовываем temp_<filename> в оригинальное название
    if (!MoveFileA(temp_filename.c_str(), filename.c_str())) {
        std::cerr << "Не удалось переименовать " << temp_filename << " в " << filename << ".\n";
    }
#else
    if (remove(filename.c_str()) != 0) {
        perror(("Ошибка удаления файла " + filename).c_str());
    }
    if (rename(temp_filename.c_str(), filename.c_str()) != 0) {
        perror(("Ошибка переименования файла " + temp_filename + " в " + filename).c_str());
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

// Функция обработки измерений и записи средних значений
void process_measurements(std::deque<Measurement>& measurements_deque) {
    int counter = 0; // Счётчик для определения, когда выполнять расчёт дневного среднего

    while (running) {
        // Выполняем раз в минуту (симулирует 1 час)
        std::this_thread::sleep_for(std::chrono::minutes(1));

        auto now = std::chrono::system_clock::now();
        // Определяем cutoff_times в соответствии с тестовыми временными интервалами
        auto cutoff_hour = now - std::chrono::minutes(1);       // симулирует 1 час
        auto cutoff_day = now - std::chrono::minutes(2);        // симулирует 1 день
        auto cutoff_month = now - std::chrono::minutes(3);      // симулирует 1 месяц
        auto cutoff_year = now - std::chrono::minutes(4);       // симулирует 1 год

        // Сбор данных за последний "час" (1 минута)
        std::vector<double> hourly_temps;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            for (const auto& m : measurements_deque) {
                if (m.timestamp >= cutoff_hour) {
                    hourly_temps.push_back(m.temperature);
                }
            }
        }

        // Вычисление среднего за час
        double hourly_avg = calculate_average(hourly_temps);
        std::ostringstream oss_hourly;
        oss_hourly << std::chrono::system_clock::to_time_t(now) << " " << hourly_avg;
        write_log(LOG_HOURLY, oss_hourly.str());
        std::cout << "Среднее за час записано: " << hourly_avg << "°C\n";

        counter++;

        // Проверяем, пора ли обновлять дневное среднее (каждые 2 минуты)
        if (counter % 2 == 0) { // Каждые 2 минуты симулирует 1 день
            // Сбор данных за последние "2 минуты" (1 день)
            std::vector<double> daily_temps;
            {
                std::lock_guard<std::mutex> lock(log_mutex);
                for (const auto& m : measurements_deque) {
                    if (m.timestamp >= cutoff_day) {
                        daily_temps.push_back(m.temperature);
                    }
                }
            }

            // Вычисление среднего за день
            double daily_avg = calculate_average(daily_temps);
            std::ostringstream oss_daily;
            oss_daily << std::chrono::system_clock::to_time_t(now) << " " << daily_avg;
            write_log(LOG_DAILY, oss_daily.str());
            std::cout << "Среднее за день записано: " << daily_avg << "°C\n";
        }

        // Очистка LOG_ALL (хранение последних 24 часов = 24 минуты)
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            while (!measurements_deque.empty() && measurements_deque.front().timestamp < (now - std::chrono::minutes(24))) {
                measurements_deque.pop_front();
            }
        }

        // Очистка LOG_HOURLY (хранение последних 1 месяц = 3 минуты)
        cleanup_log(LOG_HOURLY, cutoff_month);

        // Очистка LOG_DAILY (хранение последних 1 год = 4 минуты)
        cleanup_log(LOG_DAILY, cutoff_year);
    }
}

// Обработчик сигналов для корректного завершения программы
void signal_handler(int signal) {
    if (signal == SIGINT) {
        std::cout << "\nПолучен сигнал прерывания. Завершение работы...\n";
        running = false;
    }
}

#ifdef _WIN32
// Обработчик сигналов для Windows
BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT) {
        signal_handler(SIGINT);
        return TRUE;
    }
    return FALSE;
}
#endif

int main(int argc, char* argv[]) {
    // Регистрация обработчика сигналов
#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
#endif

    // Настройка последовательного порта
    std::string port_name;
    int baud_rate = 9600;

    // Получение параметров из аргументов командной строки, если необходимо
    if (argc >= 2) {
        port_name = argv[1];
    }
    else {
#ifdef _WIN32
        // Пример порта для Windows: "COM4"
        port_name = "COM4";
#else
        // Пример порта для Unix: "/dev/ttyS7"
        port_name = "/dev/ttyS7";
#endif
    }

    if (argc >= 3) {
        try {
            baud_rate = std::stoi(argv[2]);
        }
        catch (...) {
            std::cerr << "Некорректное значение baud_rate. Используется 9600.\n";
            baud_rate = 9600;
        }
    }

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

    // Очередь для хранения всех измерений (хранит только последние 24 часа = 24 минуты)
    std::deque<Measurement> measurements_deque;

    // Запуск потока для обработки измерений
    std::thread processor_thread(process_measurements, std::ref(measurements_deque));

    // Основной цикл чтения данных
    while (running) {
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

                    std::cout << "Измерение записано: " << temp << "°C\n";
                }
                catch (const std::exception& e) {
                    std::cerr << "Ошибка парсинга строки: " << line << "\n";
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Пауза для снижения загрузки CPU
    }

    // Закрытие последовательного порта и завершение потока
#ifdef _WIN32
    close_serial_port(hSerial);
#else
    close_serial_port(fd);
#endif
    if (processor_thread.joinable()) {
        processor_thread.join();
    }

    std::cout << "Программа завершена.\n";
    return 0;
}
