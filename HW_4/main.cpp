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
#endif

const std::string LOG_ALL = "log_all_measurements.log";
const std::string LOG_HOURLY = "log_hourly_averages.log";
const std::string LOG_DAILY = "log_daily_averages.log";

std::mutex log_mutex;
std::atomic<bool> running(true);

struct Measurement {
    double temperature;
    std::chrono::system_clock::time_point timestamp;
};

void write_log(const std::string& filename, const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::ofstream log_file(filename, std::ios::app);
    if (log_file) {
        log_file << message << "\n";
    } else {
        std::cerr << "Ошибка открытия файла: " << filename << "\n";
    }
}

void cleanup_log(const std::string& filename, std::chrono::system_clock::time_point cutoff_time) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        std::cerr << "Файл не существует: " << filename << "\n";
        return;
    }

    std::string temp_filename = "temp_" + filename;
    std::ofstream temp_file(temp_filename, std::ios::trunc);
    if (!temp_file.is_open()) {
        std::cerr << "Ошибка создания временного файла для: " << filename << "\n";
        return;
    }

    std::string line;
    while (std::getline(infile, line)) {
        std::istringstream iss(line);
        std::time_t timestamp;
        double temp;
        if (iss >> timestamp >> temp) {
            auto entry_time = std::chrono::system_clock::from_time_t(timestamp);
            if (entry_time >= cutoff_time) {
                temp_file << line << "\n";
            }
        }
    }

    infile.close();
    temp_file.close();
#ifdef _WIN32
    if (!DeleteFileA(filename.c_str())) {
        std::cerr << "Не удалось удалить файл: " << filename << "\n";
    }
    if (!MoveFileA(temp_filename.c_str(), filename.c_str())) {
        std::cerr << "Не удалось переименовать временный файл: " << temp_filename << "\n";
    }
#else
    std::remove(filename.c_str());
    std::rename(temp_filename.c_str(), filename.c_str());
#endif
}

double calculate_average(const std::vector<double>& values) {
    double sum = 0.0;
    for (double v : values) sum += v;
    return values.empty() ? 0.0 : sum / values.size();
}

#ifdef _WIN32
HANDLE open_serial_port(const std::string& port_name, int baud_rate) {
    HANDLE hSerial = CreateFileA(port_name.c_str(),
                                 GENERIC_READ | GENERIC_WRITE,
                                 0,
                                 NULL,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL,
                                 NULL);
    if (hSerial == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    DCB dcbSerialParams = {0};
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(hSerial, &dcbSerialParams)) return INVALID_HANDLE_VALUE;

    dcbSerialParams.BaudRate = baud_rate;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    if (!SetCommState(hSerial, &dcbSerialParams)) return INVALID_HANDLE_VALUE;

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hSerial, &timeouts);

    return hSerial;
}

void close_serial_port(HANDLE hSerial) {
    CloseHandle(hSerial);
}

bool read_serial_port(HANDLE hSerial, char* buffer, DWORD buf_size, DWORD& bytes_read) {
    return ReadFile(hSerial, buffer, buf_size, &bytes_read, NULL);
}
#else
int open_serial_port(const std::string& port_name, int baud_rate) {
    int fd = open(port_name.c_str(), O_RDONLY | O_NOCTTY | O_NDELAY);
    if (fd == -1) return -1;

    struct termios options;
    tcgetattr(fd, &options);
    cfsetispeed(&options, B9600);
    cfsetospeed(&options, B9600);

    options.c_cflag |= (CLOCAL | CREAD);
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;

    tcsetattr(fd, TCSANOW, &options);
    return fd;
}

void close_serial_port(int fd) {
    close(fd);
}

bool read_serial_port(int fd, char* buffer, size_t buf_size, ssize_t& bytes_read) {
    bytes_read = read(fd, buffer, buf_size);
    return bytes_read > 0;
}
#endif

void process_measurements(std::deque<Measurement>& measurements_deque) {
    int hourly_counter = 0;

    while (running) {
        std::this_thread::sleep_for(std::chrono::minutes(1));
        auto now = std::chrono::system_clock::now();

        auto cutoff_24h = now - std::chrono::minutes(24);
        auto cutoff_month = now - std::chrono::minutes(720);
        auto cutoff_year = now - std::chrono::minutes(8760);

        cleanup_log(LOG_ALL, cutoff_24h);
        cleanup_log(LOG_HOURLY, cutoff_month);
        cleanup_log(LOG_DAILY, cutoff_year);

        std::vector<double> hourly_temps;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            for (const auto& m : measurements_deque) {
                if (m.timestamp >= now - std::chrono::minutes(1)) {
                    hourly_temps.push_back(m.temperature);
                }
            }
        }

        if (!hourly_temps.empty()) {
            double hourly_avg = calculate_average(hourly_temps);
            std::ostringstream oss_hourly;
            oss_hourly << std::chrono::system_clock::to_time_t(now) << " " << hourly_avg;
            write_log(LOG_HOURLY, oss_hourly.str());
        }

        hourly_counter++;
        if (hourly_counter == 24) {
            hourly_counter = 0;

            std::vector<double> daily_temps;
            {
                std::lock_guard<std::mutex> lock(log_mutex);
                for (const auto& m : measurements_deque) {
                    if (m.timestamp >= now - std::chrono::minutes(24)) {
                        daily_temps.push_back(m.temperature);
                    }
                }
            }

            if (!daily_temps.empty()) {
                double daily_avg = calculate_average(daily_temps);
                std::ostringstream oss_daily;
                oss_daily << std::chrono::system_clock::to_time_t(now) << " " << daily_avg;
                write_log(LOG_DAILY, oss_daily.str());
            }
        }
    }
}

void signal_handler(int signal) {
    if (signal == SIGINT) {
        running = false;
    }
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);

    std::string port_name;
#ifdef _WIN32
    port_name = "COM4"; // Задайте свой COM-порт
#else
    port_name = "/dev/ttyS7";
#endif
    int baud_rate = 9600;

#ifdef _WIN32
    HANDLE hSerial = open_serial_port(port_name, baud_rate);
    if (hSerial == INVALID_HANDLE_VALUE) {
        std::cerr << "Ошибка открытия порта: " << port_name << "\n";
        return 1;
    }
#else
    int fd = open_serial_port(port_name, baud_rate);
    if (fd == -1) {
        std::cerr << "Ошибка открытия порта: " << port_name << "\n";
        return 1;
    }
#endif

    std::deque<Measurement> measurements_deque;

    std::thread processor_thread([&]() {
        process_measurements(measurements_deque);
    });

    while (running) {
        char buffer[256];
#ifdef _WIN32
        DWORD bytes_read = 0;
        if (read_serial_port(hSerial, buffer, sizeof(buffer) - 1, bytes_read)) {
#else
        ssize_t bytes_read = 0;
        if (read_serial_port(fd, buffer, sizeof(buffer) - 1, bytes_read)) {
#endif
            buffer[bytes_read] = '\0';
            std::istringstream iss(buffer);
            std::string line;
            while (std::getline(iss, line)) {
                try {
                    double temp = std::stod(line);
                    auto now = std::chrono::system_clock::now();
                    Measurement m{temp, now};
                    {
                        std::lock_guard<std::mutex> lock(log_mutex);
                        measurements_deque.emplace_back(m);
                    }

                    std::ostringstream oss;
                    oss << std::chrono::system_clock::to_time_t(now) << " " << temp;
                    write_log(LOG_ALL, oss.str());
                } catch (...) {
                    std::cerr << "Ошибка парсинга данных\n";
                }
            }
        }
    }

#ifdef _WIN32
    close_serial_port(hSerial);
#else
    close_serial_port(fd);
#endif
    if (processor_thread.joinable()) processor_thread.join();

    return 0;
}
