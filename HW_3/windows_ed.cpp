#include <iostream>
#include <fstream>
#include <cstring>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <windows.h>

// Глобальные имена для разделяемой памяти и мьютекса
const char* SHM_NAME = "Global\\MySharedMemory";
const char* MUTEX_NAME = "Global\\MyMutex";

// Структура для разделяемых данных
struct SharedData {
    int counter;
};

// Функция получения текущего времени
std::string get_current_time_string() {
    char buffer[64];
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buffer);
}

// Захват мьютекса
bool acquire_mutex(HANDLE hMutex) {
    DWORD result = WaitForSingleObject(hMutex, INFINITE);
    return (result == WAIT_OBJECT_0);
}

// Освобождение мьютекса
void release_mutex(HANDLE hMutex) {
    ReleaseMutex(hMutex);
}

// Функция записи в лог
void write_log(SharedData* sharedData, const std::string& msg, HANDLE hMutex) {
    if (acquire_mutex(hMutex)) {
        std::ofstream logFile("log.txt", std::ios::app);
        if (logFile.is_open()) {
            logFile << msg << " | counter=" << sharedData->counter << std::endl;
            logFile.close();
        }
        release_mutex(hMutex);
    }
}

// Функция создания или открытия разделяемой памяти и мьютекса
bool initialize_shared_resources(SharedData** sharedData, bool& isMaster, HANDLE& hMutex, HANDLE& hMapFile) {
    hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SharedData), SHM_NAME);
    if (hMapFile == NULL) {
        std::cerr << "[ERROR] CreateFileMapping failed with error: " << GetLastError() << std::endl;
        return false;
    }

    isMaster = (GetLastError() != ERROR_ALREADY_EXISTS);

    *sharedData = static_cast<SharedData*>(MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedData)));
    if (*sharedData == NULL) {
        std::cerr << "[ERROR] MapViewOfFile failed with error: " << GetLastError() << std::endl;
        CloseHandle(hMapFile);
        return false;
    }

    hMutex = CreateMutexA(NULL, FALSE, MUTEX_NAME);
    if (hMutex == NULL) {
        std::cerr << "[ERROR] CreateMutex failed with error: " << GetLastError() << std::endl;
        UnmapViewOfFile(*sharedData);
        CloseHandle(hMapFile);
        return false;
    }

    if (isMaster) {
        (*sharedData)->counter = 0;
        std::cout << "[INFO] Мастер успешно инициализирован." << std::endl;
    } else {
        std::cout << "[INFO] Слейв подключился к разделяемой памяти." << std::endl;
    }

    return true;
}

// Очистка ресурсов
void cleanup_shared_resources(SharedData* sharedData, HANDLE hMutex, HANDLE hMapFile) {
    if (sharedData) UnmapViewOfFile(sharedData);
    if (hMutex) CloseHandle(hMutex);
    if (hMapFile) CloseHandle(hMapFile);
}

// Функция запуска дочернего процесса
bool spawn_child_process(int mode, const std::string& exePath) {
    std::string cmdLine = exePath + " --child " + std::to_string(mode);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(NULL, const_cast<char*>(cmdLine.c_str()), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        std::cerr << "[ERROR] CreateProcess failed with error: " << GetLastError() << std::endl;
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// Функция обработки пользовательского ввода
void user_input_thread(SharedData* sharedData, HANDLE hMutex, std::atomic<bool>& running) {
    while (running) {
        std::cout << "Введите новое значение счётчика: ";
        int new_value;
        std::cin >> new_value;

        if (std::cin.fail()) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << "Некорректный ввод. Попробуйте ещё раз." << std::endl;
            continue;
        }

        if (acquire_mutex(hMutex)) {
            sharedData->counter = new_value;
            release_mutex(hMutex);
            std::cout << "[INFO] Значение счётчика изменено на " << new_value << std::endl;
        }
    }
}

// Функция для дочернего процесса (моды 1 и 2)
void run_child_process(SharedData* sharedData, HANDLE hMutex, int mode) {
    int pid = GetCurrentProcessId();
    std::string start_time = get_current_time_string();

    std::string role = (mode == 1) ? "COPY1" : "COPY2";
    write_log(sharedData, "[" + role + "] Start: PID=" + std::to_string(pid) + ", time=" + start_time, hMutex);

    if (acquire_mutex(hMutex)) {
        if (mode == 1) {
            sharedData->counter += 10;
        } else if (mode == 2) {
            sharedData->counter *= 2;
            release_mutex(hMutex);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            acquire_mutex(hMutex);
            sharedData->counter /= 2;
        }
        release_mutex(hMutex);
    }

    std::string end_time = get_current_time_string();
    write_log(sharedData, "[" + role + "] End: PID=" + std::to_string(pid) + ", time=" + end_time, hMutex);
}

// Основная программа
int main(int argc, char* argv[]) {
    HANDLE hMapFile = NULL;
    HANDLE hMutex = NULL;
    SharedData* sharedData = nullptr;
    bool isMaster = false;

    if (!initialize_shared_resources(&sharedData, isMaster, hMutex, hMapFile)) {
        return 1;
    }

    if (argc >= 3 && std::strcmp(argv[1], "--child") == 0) {
        int mode = std::atoi(argv[2]);
        run_child_process(sharedData, hMutex, mode);
        cleanup_shared_resources(sharedData, hMutex, hMapFile);
        return 0;
    }

    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);

    std::atomic<bool> running(true);
    std::thread inputThread(user_input_thread, sharedData, hMutex, std::ref(running));

    auto last_300ms = std::chrono::steady_clock::now();
    auto last_1s = std::chrono::steady_clock::now();
    auto last_3s = std::chrono::steady_clock::now();

    while (true) {
        auto now = std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_300ms).count() >= 300) {
            last_300ms = now;
            if (isMaster && acquire_mutex(hMutex)) {
                sharedData->counter += 1;
                release_mutex(hMutex);
            }
        }

        if (isMaster && std::chrono::duration_cast<std::chrono::seconds>(now - last_1s).count() >= 1) {
            last_1s = now;
            write_log(sharedData, "[MASTER] " + get_current_time_string()
                       + " PID=" + std::to_string(GetCurrentProcessId())
                       + ", counter=" + std::to_string(sharedData->counter), hMutex);
        }

        if (isMaster && std::chrono::duration_cast<std::chrono::seconds>(now - last_3s).count() >= 3) {
            last_3s = now;
            spawn_child_process(1, exePath);
            spawn_child_process(2, exePath);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    running = false;
    inputThread.join();
    cleanup_shared_resources(sharedData, hMutex, hMapFile);
    return 0;
}





// Глобальные имена для разделяемой памяти и мьютекса
const char* SHM_NAME = "Global\MySharedMemory";
const char* MUTEX_NAME = "Global\MyMutex";


// Структура для разделяемых данных
struct SharedData {
int counter;
};


// Функция получения текущего времени
std::string get_current_time_string() {
char buffer[64];
SYSTEMTIME st;
GetLocalTime(&st);
snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
st.wYear, st.wMonth, st.wDay,
st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
return std::string(buffer);
}


// Захват мьютекса
bool acquire_mutex(HANDLE hMutex) {
DWORD result = WaitForSingleObject(hMutex, INFINITE);
return (result == WAIT_OBJECT_0);
}


// Освобождение мьютекса
void release_mutex(HANDLE hMutex) {
ReleaseMutex(hMutex);
}


// Функция записи в лог
void write_log(SharedData* sharedData, const std::string& msg, HANDLE hMutex) {
if (acquire_mutex(hMutex)) {
std::ofstream logFile("log.txt", std::ios::app);
if (logFile.is_open()) {
logFile << msg << " | counter=" << sharedData->counter << std::endl;
logFile.close();
}
release_mutex(hMutex);
}
}


// Функция создания или открытия разделяемой памяти и мьютекса
bool initialize_shared_resources(SharedData** sharedData, bool& isMaster, HANDLE& hMutex, HANDLE& hMapFile) {
hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SharedData), SHM_NAME);
if (hMapFile == NULL) {
std::cerr << "[ERROR] CreateFileMapping failed with error: " << GetLastError() << std::endl;
return false;
}


isMaster = (GetLastError() != ERROR_ALREADY_EXISTS);

*sharedData = static_cast<SharedData*>(MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedData)));
if (*sharedData == NULL) {
    std::cerr << "[ERROR] MapViewOfFile failed with error: " << GetLastError() << std::endl;
    CloseHandle(hMapFile);
    return false;
}

hMutex = CreateMutexA(NULL, FALSE, MUTEX_NAME);
if (hMutex == NULL) {
    std::cerr << "[ERROR] CreateMutex failed with error: " << GetLastError() << std::endl;
    UnmapViewOfFile(*sharedData);
    CloseHandle(hMapFile);
    return false;
}

if (isMaster) {
    (*sharedData)->counter = 0;
    std::cout << "[INFO] Мастер успешно инициализирован." << std::endl;
} else {
    std::cout << "[INFO] Слейв подключился к разделяемой памяти." << std::endl;
}

return true;
}


// Очистка ресурсов
void cleanup_shared_resources(SharedData* sharedData, HANDLE hMutex, HANDLE hMapFile) {
if (sharedData) UnmapViewOfFile(sharedData);
if (hMutex) CloseHandle(hMutex);
if (hMapFile) CloseHandle(hMapFile);
}


// Функция запуска дочернего процесса
bool spawn_child_process(int mode, const std::string& exePath) {
std::string cmdLine = exePath + " --child " + std::to_string(mode);


STARTUPINFOA si = {};
si.cb = sizeof(si);
PROCESS_INFORMATION pi = {};

if (!CreateProcessA(NULL, const_cast<char*>(cmdLine.c_str()), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    std::cerr << "[ERROR] CreateProcess failed with error: " << GetLastError() << std::endl;
    return false;
}

CloseHandle(pi.hProcess);
CloseHandle(pi.hThread);
return true;
}


// Функция для дочернего процесса (моды 1 и 2)
void run_child_process(SharedData* sharedData, HANDLE hMutex, int mode) {
int pid = GetCurrentProcessId();
std::string start_time = get_current_time_string();


write_log(sharedData, "[CHILD " + std::to_string(mode) + "] Start: PID=" + std::to_string(pid) + ", time=" + start_time, hMutex);

if (acquire_mutex(hMutex)) {
    if (mode == 1) {
        sharedData->counter += 10;
    } else if (mode == 2) {
        sharedData->counter *= 2;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        sharedData->counter /= 2;
    }
    release_mutex(hMutex);
}

std::string end_time = get_current_time_string();
write_log(sharedData, "[CHILD " + std::to_string(mode) + "] End: PID=" + std::to_string(pid) + ", time=" + end_time, hMutex);
}


// Основная программа
int main(int argc, char* argv[]) {
HANDLE hMapFile = NULL;
HANDLE hMutex = NULL;
SharedData* sharedData = nullptr;
bool isMaster = false;


if (!initialize_shared_resources(&sharedData, isMaster, hMutex, hMapFile)) {
    return 1;
}

// Проверяем, является ли процесс дочерним
if (argc >= 3 && std::strcmp(argv[1], "--child") == 0) {
    int mode = std::atoi(argv[2]);
    run_child_process(sharedData, hMutex, mode);
    cleanup_shared_resources(sharedData, hMutex, hMapFile);
    return 0;
}

// Путь к исполняемому файлу
char exePath[MAX_PATH];
GetModuleFileNameA(NULL, exePath, MAX_PATH);

// Основной цикл мастера
auto last_300ms = std::chrono::steady_clock::now();
auto last_1s = std::chrono::steady_clock::now();
auto last_3s = std::chrono::steady_clock::now();

while (true) {
    auto now = std::chrono::steady_clock::now();

    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_300ms).count() >= 300) {
        last_300ms = now;
        if (isMaster && acquire_mutex(hMutex)) {
            sharedData->counter += 1;
            release_mutex(hMutex);
        }
    }

    if (isMaster && std::chrono::duration_cast<std::chrono::seconds>(now - last_1s).count() >= 1) {
        last_1s = now;
        write_log(sharedData, "[MASTER] Current counter: " + std::to_string(sharedData->counter), hMutex);
    }

    if (isMaster && std::chrono::duration_cast<std::chrono::seconds>(now - last_3s).count() >= 3) {
        last_3s = now;
        spawn_child_process(1, exePath);
        spawn_child_process(2, exePath);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

cleanup_shared_resources(sharedData, hMutex, hMapFile);
return 0;
}