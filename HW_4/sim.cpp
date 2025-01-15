// simulator.cpp
#include <iostream>
#include <string>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <termios.h>
    #include <unistd.h>
    #include <errno.h>
#endif

using namespace std;

#ifdef _WIN32
// Функции для работы с последовательным портом на Windows
HANDLE openSerialPort(const char* portName, int baudRate) {
    HANDLE hSerial = CreateFileA(portName,
                                 GENERIC_READ | GENERIC_WRITE,
                                 0,
                                 NULL,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL,
                                 NULL);
    if (hSerial == INVALID_HANDLE_VALUE) {
        std::cerr << "Ошибка открытия последовательного порта " << portName << "\n";
        return INVALID_HANDLE_VALUE;
    }

    DCB dcbSerialParams = {0};
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);

    if (!GetCommState(hSerial, &dcbSerialParams)) {
        std::cerr << "Ошибка получения состояния порта.\n";
        CloseHandle(hSerial);
        return INVALID_HANDLE_VALUE;
    }

    dcbSerialParams.BaudRate = baudRate;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity   = NOPARITY;

    if(!SetCommState(hSerial, &dcbSerialParams)){
        std::cerr << "Ошибка установки параметров порта.\n";
        CloseHandle(hSerial);
        return INVALID_HANDLE_VALUE;
    }

    // Установка таймаутов
    COMMTIMEOUTS timeouts = {0};
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;

    if(!SetCommTimeouts(hSerial, &timeouts)){
        std::cerr << "Ошибка установки таймаутов.\n";
        CloseHandle(hSerial);
        return INVALID_HANDLE_VALUE;
    }

    return hSerial;
}

bool writeSerialPort(HANDLE hSerial, const char* data, DWORD dataSize, DWORD& bytesWritten) {
    return WriteFile(hSerial, data, dataSize, &bytesWritten, NULL);
}

void closeSerialPort(HANDLE hSerial) {
    CloseHandle(hSerial);
}

#else
// Функции для работы с последовательным портом на Unix
int openSerialPort(const char* portName, int baudRate) {
    int fd = open(portName, O_WRONLY | O_NOCTTY | O_NDELAY);
    if (fd == -1) {
        perror(("Ошибка открытия порта " + string(portName)).c_str());
        return -1;
    }

    struct termios options;
    if (tcgetattr(fd, &options) < 0) {
        perror("Ошибка получения атрибутов порта");
        close(fd);
        return -1;
    }

    // Установка скорости
    speed_t baud;
    switch (baudRate) {
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
    if (tcsetattr(fd, TCSANOW, &options) < 0) {
        perror("Ошибка установки атрибутов порта");
        close(fd);
        return -1;
    }

    return fd;
}

bool writeSerialPort(int fd, const char* data, size_t dataSize, ssize_t& bytesWritten) {
    bytesWritten = write(fd, data, dataSize);
    return bytesWritten >= 0;
}

void closeSerialPort(int fd) {
    close(fd);
}
#endif

int main(int argc, char* argv[]) {
    // Проверка аргументов
    if (argc < 2) {
        cout << "Использование: " << argv[0] << " <serial_port> [baud_rate]\n";
        cout << "Пример: " << argv[0] << " COM3 9600        (Windows)\n";
        cout << "        " << argv[0] << " /dev/ttys007 9600  (Linux/macOS)\n";
        return 1;
    }

    string port = argv[1];
    unsigned int baud_rate = 9600;
    if (argc >= 3) {
        baud_rate = stoi(argv[2]);
    }

    // Открытие последовательного порта
#ifdef _WIN32
    HANDLE hSerial = openSerialPort(port.c_str(), baud_rate);
    if (hSerial == INVALID_HANDLE_VALUE) {
        return 1;
    }
#else
    int fd = openSerialPort(port.c_str(), baud_rate);
    if (fd == -1) {
        return 1;
    }
#endif

    // Инициализация генератора случайных чисел
    srand(static_cast<unsigned int>(time(0)));

    cout << "Симулятор начал отправку данных на порт " << port << "...\n";

    while (true) {
        // Генерация случайной температуры от 20.00 до 30.00
        double temp = 20.0 + static_cast<double>(rand()) / RAND_MAX * 10.0;

        // Формирование строки для отправки: только число с двумя знаками после запятой и символ новой строки
        ostringstream oss;
        oss << fixed << setprecision(2) << temp << "\n";
        string data = oss.str();

#ifdef _WIN32
        DWORD bytesWritten;
        bool success = writeSerialPort(hSerial, data.c_str(), data.size(), bytesWritten);
        if (!success) {
            cerr << "Ошибка записи в последовательный порт.\n";
            break;
        }
#else
        ssize_t bytesWritten;
        bool success = writeSerialPort(fd, data.c_str(), data.size(), bytesWritten);
        if (!success) {
            perror("Ошибка записи в последовательный порт");
            break;
        }
#endif
        cout << "Отправлено: " << temp << "°C\n";

        // Задержка 1 секунда
        this_thread::sleep_for(chrono::seconds(1));
    }

    // Закрытие последовательного порта
#ifdef _WIN32
    closeSerialPort(hSerial);
#else
    closeSerialPort(fd);
#endif

    return 0;
}
