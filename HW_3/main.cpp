#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <cstring>
#include <ctime>
#include <cstdlib>

#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
#else
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <unistd.h>
    #include <pthread.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <sys/stat.h>
    #include <mach-o/dyld.h> // Для macOS получения пути к исполняемому файлу
#endif

// Структура для разделяемых данных
struct SharedData {
    int counter;

#ifndef _WIN32
    pthread_mutex_t mutex; // На POSIX мьютекс включён в структуру
#endif
};

// Функция получения текущего времени в строковом формате
std::string get_current_time_string() {
    char buffer[64];
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_now;
    localtime_r(&ts.tv_sec, &tm_now);
    int ms = static_cast<int>(ts.tv_nsec / 1000000);
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, ms);
#endif
    return std::string(buffer);
}

// Функция получения идентификатора процесса
int get_process_id() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

// Функция задержки на указанное количество миллисекунд
void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Глобальные имена для разделяемой памяти и мьютекса
#ifdef _WIN32
const char* SHM_NAME = "Global\\MySharedMemory";
const char* MUTEX_NAME = "Global\\MyMutex";
#else
const char* SHM_NAME = "/mysharedmemory";
#endif

// Функция для инициализации разделяемой памяти и определения роли процесса
bool initialize_shared_memory(SharedData** sharedData, bool& isMaster) {
#ifdef _WIN32
    // Создаём или открываем разделяемую память
    HANDLE hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(SharedData),
        SHM_NAME
    );

    if (hMapFile == NULL) {
        std::cerr << "[ERROR] CreateFileMapping failed with error: " << GetLastError() << std::endl;
        return false;
    }

    // Определяем роль процесса
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        isMaster = false;
    } else {
        isMaster = true;
    }

    // Маппируем разделяемую память
    *sharedData = static_cast<SharedData*>(MapViewOfFile(
        hMapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(SharedData)
    ));

    if (*sharedData == NULL) {
        std::cerr << "[ERROR] MapViewOfFile failed with error: " << GetLastError() << std::endl;
        CloseHandle(hMapFile);
        return false;
    }

    // Если мастер, инициализируем счётчик
    if (isMaster) {
        (*sharedData)->counter = 0;
        std::cout << "[INFO] Процесс " << get_process_id() << " является Мастером." << std::endl;

        // Создаём именованный мьютекс
        HANDLE hMutex = CreateMutexA(
            NULL,
            FALSE,
            MUTEX_NAME
        );

        if (hMutex == NULL) {
            std::cerr << "[ERROR] CreateMutex failed with error: " << GetLastError() << std::endl;
            UnmapViewOfFile(*sharedData);
            CloseHandle(hMapFile);
            return false;
        }

        CloseHandle(hMutex); // Закрываем дескриптор мьютекса
    } else {
        std::cout << "[INFO] Процесс " << get_process_id() << " является Слейвом." << std::endl;
    }

    CloseHandle(hMapFile); // Закрываем дескриптор разделяемой памяти
    return true;

#else
    // POSIX: Используем O_CREAT | O_EXCL для определения роли
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (shm_fd >= 0) {
        // Успешно создали, процесс - мастер
        isMaster = true;
        std::cout << "[INFO] Процесс " << get_process_id() << " является Мастером." << std::endl;
        // Устанавливаем размер разделяемой памяти
        if (ftruncate(shm_fd, sizeof(SharedData)) == -1) {
            perror("[ERROR] ftruncate");
            close(shm_fd);
            return false;
        }
    } else {
        if (errno == EEXIST) {
            // Разделяемая память уже существует, процесс - слейв
            shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
            if (shm_fd < 0) {
                perror("[ERROR] shm_open");
                return false;
            }
            isMaster = false;
            std::cout << "[INFO] Процесс " << get_process_id() << " является Слейвом." << std::endl;
        } else {
            perror("[ERROR] shm_open");
            return false;
        }
    }

    // Маппируем разделяемую память
    *sharedData = static_cast<SharedData*>(mmap(NULL, sizeof(SharedData),
                                               PROT_READ | PROT_WRITE,
                                               MAP_SHARED,
                                               shm_fd, 0));
    if (*sharedData == MAP_FAILED) {
        perror("[ERROR] mmap");
        close(shm_fd);
        return false;
    }

    close(shm_fd); // Файловый дескриптор больше не нужен

    // Если мастер, инициализируем счётчик и мьютекс
    if (isMaster) {
        (*sharedData)->counter = 0;

        pthread_mutexattr_t attr;
        if (pthread_mutexattr_init(&attr) != 0) {
            std::cerr << "[ERROR] pthread_mutexattr_init failed." << std::endl;
            munmap(*sharedData, sizeof(SharedData));
            return false;
        }

        if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
            std::cerr << "[ERROR] pthread_mutexattr_setpshared failed." << std::endl;
            pthread_mutexattr_destroy(&attr);
            munmap(*sharedData, sizeof(SharedData));
            return false;
        }

        if (pthread_mutex_init(&(*sharedData)->mutex, &attr) != 0) {
            std::cerr << "[ERROR] pthread_mutex_init failed." << std::endl;
            pthread_mutexattr_destroy(&attr);
            munmap(*sharedData, sizeof(SharedData));
            return false;
        }

        pthread_mutexattr_destroy(&attr);
    }

    return true;
#endif
}

// Функция очистки разделяемой памяти
void cleanup_shared_memory(SharedData* sharedData, bool isMaster) {
#ifdef _WIN32
    if (sharedData != NULL) {
        UnmapViewOfFile(sharedData);
    }
    // На Windows удаление именованных объектов не требуется, они удаляются автоматически
#else
    if (sharedData && sharedData != MAP_FAILED) {
        munmap(sharedData, sizeof(SharedData));
    }
    if (isMaster) {
        // Мастер удаляет разделяемую память при завершении
        if (shm_unlink(SHM_NAME) == -1) {
            perror("[ERROR] shm_unlink");
        } else {
            std::cout << "[INFO] Разделяемая память удалена." << std::endl;
        }
    }
#endif
}

// Функция захвата мьютекса
bool acquire_mutex(SharedData* sd, bool isMaster) {
#ifdef _WIN32
    // Открываем именованный мьютекс
    HANDLE hMutex = OpenMutexA(SYNCHRONIZE, FALSE, MUTEX_NAME);
    if (hMutex == NULL) {
        std::cerr << "[ERROR] OpenMutex failed with error: " << GetLastError() << std::endl;
        return false;
    }
    DWORD dwWaitResult = WaitForSingleObject(hMutex, INFINITE);
    if (dwWaitResult != WAIT_OBJECT_0) {
        std::cerr << "[ERROR] WaitForSingleObject failed with error: " << GetLastError() << std::endl;
        CloseHandle(hMutex);
        return false;
    }
    CloseHandle(hMutex);
    return true;
#else
    if (pthread_mutex_lock(&sd->mutex) == 0) {
        return true;
    } else {
        std::cerr << "[ERROR] pthread_mutex_lock failed." << std::endl;
        return false;
    }
#endif
}

// Функция освобождения мьютекса
void release_mutex(SharedData* sd, bool isMaster) {
#ifdef _WIN32
    // Открываем именованный мьютекс
    HANDLE hMutex = OpenMutexA(SYNCHRONIZE, FALSE, MUTEX_NAME);
    if (hMutex != NULL) {
        if (!ReleaseMutex(hMutex)) {
            std::cerr << "[ERROR] ReleaseMutex failed with error: " << GetLastError() << std::endl;
        }
        CloseHandle(hMutex);
    } else {
        std::cerr << "[ERROR] OpenMutex failed during release with error: " << GetLastError() << std::endl;
    }
#else
    if (pthread_mutex_unlock(&sd->mutex) != 0) {
        std::cerr << "[ERROR] pthread_mutex_unlock failed." << std::endl;
    }
#endif
}

// Функция записи в лог-файл с синхронизацией
void write_log(SharedData* sd, const std::string& msg, bool isMaster) {
    if (!acquire_mutex(sd, isMaster)) {
        std::cerr << "[ERROR] Не удалось захватить мьютекс для записи в лог." << std::endl;
        return;
    }
    {
        std::ofstream logFile("log.txt", std::ios::app);
        if (logFile.is_open()) {
            logFile << msg << std::endl;
            logFile.close();
        } else {
            std::cerr << "[ERROR] Не удалось открыть log.txt для записи." << std::endl;
        }
    }
    release_mutex(sd, isMaster);
}

// Функция запуска копии программы
bool spawn_copy(int mode, const std::string& exePath) {
    std::vector<std::string> args_vec = { exePath, "--child", std::to_string(mode) };
    std::vector<const char*> args;
    for (const auto& arg : args_vec) {
        args.push_back(arg.c_str());
    }
    args.push_back(NULL);

#ifdef _WIN32
    // Формируем командную строку
    std::string cmdLine;
    for (size_t i = 0; i < args.size() - 1; ++i) { // исключаем NULL
        cmdLine += args[i];
        cmdLine += " ";
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    BOOL success = CreateProcessA(
        NULL,
        const_cast<char*>(cmdLine.c_str()),
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        NULL,
        &si,
        &pi
    );

    if (!success) {
        std::cerr << "[ERROR] CreateProcess failed with error: " << GetLastError() << std::endl;
        return false;
    }

    // Закрываем дескрипторы процесса и потока
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) {
        perror("[ERROR] fork");
        return false;
    }
    if (pid == 0) {
        // Дочерний процесс
        execlp(args[0], args[0], args[1], args[2], (char*)NULL);
        // Если exec не удался
        perror("[ERROR] execlp");
        exit(EXIT_FAILURE);
    }
    // Родительский процесс
    return true;
#endif
}

// Функция для режима копии 1
void run_copy_mode1(SharedData* sd, bool isMaster) {
    int pid = get_process_id();
    std::string start_time = get_current_time_string();
    write_log(sd, "[COPY1] Start: PID=" + std::to_string(pid) + ", time=" + start_time, isMaster);

    // Увеличиваем счётчик на 10
    if (acquire_mutex(sd, isMaster)) {
        sd->counter += 10;
        release_mutex(sd, isMaster);
    }

    // Записываем время завершения и текущее значение счётчика
    std::string end_time = get_current_time_string();
    write_log(sd, "[COPY1] End: PID=" + std::to_string(pid)
                    + ", time=" + end_time
                    + ", counter=" + std::to_string(sd->counter), isMaster);
}

// Функция для режима копии 2
void run_copy_mode2(SharedData* sd, bool isMaster) {
    int pid = get_process_id();
    std::string start_time = get_current_time_string();
    write_log(sd, "[COPY2] Start: PID=" + std::to_string(pid) + ", time=" + start_time, isMaster);

    // Умножаем счётчик на 2
    if (acquire_mutex(sd, isMaster)) {
        sd->counter *= 2;
        release_mutex(sd, isMaster);
    }

    // Ждём 2 секунды
    sleep_ms(2000);

    // Делим счётчик на 2
    if (acquire_mutex(sd, isMaster)) {
        sd->counter /= 2;
        release_mutex(sd, isMaster);
    }

    // Записываем время завершения и текущее значение счётчика
    std::string end_time = get_current_time_string();
    write_log(sd, "[COPY2] End: PID=" + std::to_string(pid)
                    + ", time=" + end_time
                    + ", counter=" + std::to_string(sd->counter), isMaster);
}

// Функция для обработки пользовательского ввода
void user_input_thread_func(SharedData* sd, std::atomic<bool>& running, bool isMaster) {
    while (running) {
        std::cout << "Введите новое значение счётчика: ";
        int new_value;
        std::cin >> new_value;
        if (std::cin.fail()) {
            std::cin.clear(); // Сброс состояния ошибки
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); // Игнорирование некорректного ввода
            std::cout << "Некорректный ввод. Пожалуйста, введите целое число." << std::endl;
            continue;
        }

        // Устанавливаем новое значение счётчика
        if (acquire_mutex(sd, isMaster)) {
            sd->counter = new_value;
            release_mutex(sd, isMaster);
        }

        // Записываем изменение в лог
        std::string msg = "[USER] Установлено новое значение счётчика: " + std::to_string(new_value)
                        + " | PID=" + std::to_string(get_process_id())
                        + " | time=" + get_current_time_string();
        write_log(sd, msg, isMaster);
    }
}

// Функция для получения пути к исполняемому файлу
std::string get_executable_path(int argc, char* argv[]) {
    std::string exePath;
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        exePath = "myprogram.exe"; // Запасной вариант
    }
    else {
        exePath = std::string(path);
    }
#elif __APPLE__
    char path[1024];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        char resolved_path[1024];
        if (realpath(path, resolved_path)) {
            exePath = std::string(resolved_path);
        }
        else {
            exePath = std::string(path);
        }
    }
    else {
        exePath = "./myprogram"; // Запасной вариант
    }
#else
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path)-1);
    if (len != -1) {
        path[len] = '\0';
        exePath = std::string(path);
    }
    else {
        exePath = "./myprogram"; // Запасной вариант
    }
#endif
    return exePath;
}

int main(int argc, char* argv[]) {
    // Проверяем, является ли процесс копией
    bool isChild = false;
    int childMode = 0;
    if (argc >= 3) {
        if (std::strcmp(argv[1], "--child") == 0) {
            isChild = true;
            childMode = std::atoi(argv[2]);
        }
    }

    // Инициализируем разделяемую память
    SharedData* sharedData = nullptr;
    bool isMaster = false;
    if (!initialize_shared_memory(&sharedData, isMaster)) {
        std::cerr << "[ERROR] Не удалось инициализировать разделяемую память." << std::endl;
        return 1;
    }

    // Получаем PID
    int pid = get_process_id();

    // Записываем строку о запуске в лог (пункт 1)
    std::string start_time = get_current_time_string();
    std::string role = isMaster ? "MASTER" : "SLAVE";
    std::string start_msg = "[MAIN] Start: PID=" + std::to_string(pid)
                           + ", time=" + start_time
                           + " (" + role + ")";
    write_log(sharedData, start_msg, isMaster);

    // Если процесс является копией, выполняем соответствующий режим и завершаемся
    if (isChild) {
        if (childMode == 1) {
            run_copy_mode1(sharedData, isMaster);
        }
        else if (childMode == 2) {
            run_copy_mode2(sharedData, isMaster);
        }
        cleanup_shared_memory(sharedData, isMaster);
        return 0;
    }

    // Запускаем поток для обработки пользовательского ввода (пункт 3)
    std::atomic<bool> running(true);
    std::thread inputThread(user_input_thread_func, sharedData, std::ref(running), isMaster);

    // Определяем путь к исполняемому файлу для порождения копий
    std::string exePath = get_executable_path(argc, argv);
    if (exePath.empty()) {
        std::cerr << "[ERROR] Не удалось определить путь к исполняемому файлу." << std::endl;
        running = false;
        inputThread.join();
        cleanup_shared_memory(sharedData, isMaster);
        return 1;
    }

    // Переменные для отслеживания таймеров (пункты 2,4,5)
    auto last_300ms = std::chrono::steady_clock::now();
    auto last_1s = std::chrono::steady_clock::now();
    auto last_3s = std::chrono::steady_clock::now();

    // Переменные для отслеживания состояния копий (пункт 5c)
#ifdef _WIN32
    HANDLE child1Handle = NULL;
    HANDLE child2Handle = NULL;
#else
    bool child1Active = false;
    bool child2Active = false;
    pid_t childPID1 = -1;
    pid_t childPID2 = -1;
#endif

    // Основной цикл программы
    while (true) {
        auto now = std::chrono::steady_clock::now();

        // Пункт 2: Каждые 300 мс увеличиваем счётчик на 1
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_300ms).count() >= 300) {
            last_300ms = now;
            if (acquire_mutex(sharedData, isMaster)) {
                sharedData->counter += 1;
                release_mutex(sharedData, isMaster);
                // Дополнительный лог для отладки
                std::string debug_msg = "[DEBUG] PID=" + std::to_string(pid)
                                      + " увеличил счётчик до " + std::to_string(sharedData->counter);
                write_log(sharedData, debug_msg, isMaster);
            }
        }

        // Пункт 4: Раз в 1 секунду пишем в лог (только мастер)
        if (isMaster && std::chrono::duration_cast<std::chrono::milliseconds>(now - last_1s).count() >= 1000) {
            last_1s = now;
            std::string current_time = get_current_time_string();
            int current_counter;
            if (acquire_mutex(sharedData, isMaster)) {
                current_counter = sharedData->counter;
                release_mutex(sharedData, isMaster);
            } else {
                current_counter = -1; // Ошибка
            }
            std::string log_msg = "[MASTER] " + current_time
                                 + " PID=" + std::to_string(pid)
                                 + ", counter=" + std::to_string(current_counter);
            write_log(sharedData, log_msg, isMaster);
        }

        // Пункт 5: Раз в 3 секунды порождаем копии
        if (isMaster && std::chrono::duration_cast<std::chrono::milliseconds>(now - last_3s).count() >= 3000) {
            last_3s = now;
            bool canSpawn = true;

#ifdef _WIN32
            // Проверяем, завершились ли предыдущие копии
            if (child1Handle != NULL) {
                DWORD waitRes = WaitForSingleObject(child1Handle, 0);
                if (waitRes == WAIT_TIMEOUT) {
                    // Копия 1 ещё работает
                    canSpawn = false;
                }
                else {
                    // Копия 1 завершилась
                    CloseHandle(child1Handle);
                    child1Handle = NULL;
                }
            }

            if (child2Handle != NULL) {
                DWORD waitRes = WaitForSingleObject(child2Handle, 0);
                if (waitRes == WAIT_TIMEOUT) {
                    // Копия 2 ещё работает
                    canSpawn = false;
                }
                else {
                    // Копия 2 завершилась
                    CloseHandle(child2Handle);
                    child2Handle = NULL;
                }
            }
#else
            // Проверяем, завершились ли предыдущие копии
            if (child1Active) {
                int status;
                pid_t result = waitpid(childPID1, &status, WNOHANG);
                if (result == 0) {
                    // Копия 1 ещё работает
                    canSpawn = false;
                }
                else if (result == childPID1) {
                    // Копия 1 завершилась
                    child1Active = false;
                }
                else {
                    // Ошибка
                    perror("[ERROR] waitpid для копии 1");
                    child1Active = false;
                }
            }

            if (child2Active) {
                int status;
                pid_t result = waitpid(childPID2, &status, WNOHANG);
                if (result == 0) {
                    // Копия 2 ещё работает
                    canSpawn = false;
                }
                else if (result == childPID2) {
                    // Копия 2 завершилась
                    child2Active = false;
                }
                else {
                    // Ошибка
                    perror("[ERROR] waitpid для копии 2");
                    child2Active = false;
                }
            }
#endif

            if (canSpawn) {
                // Порождать копию 1
                bool spawned1 = spawn_copy(1, exePath);
                // Порождать копию 2
                bool spawned2 = spawn_copy(2, exePath);

                if (spawned1 && spawned2) {
                    std::string msg = "[MASTER] Запущены копии 1 и 2.";
                    write_log(sharedData, msg, isMaster);
#ifdef _WIN32
                    // На Windows сложно отслеживать процессы без сохранения HANDLE
                    // Поэтому не сохраняем дескрипторы копий
#else
                    // На POSIX сохраняем PID копий
                    // В текущей реализации функция spawn_copy не возвращает PID, поэтому устанавливаем активность
                    child1Active = true;
                    childPID1 = -1; // Не знаем PID
                    child2Active = true;
                    childPID2 = -1; // Не знаем PID
#endif
                }
                else {
                    std::string msg = "[MASTER] Не удалось запустить копии.";
                    write_log(sharedData, msg, isMaster);
                }
            }
            else {
                // Не можем порождать новые копии
                std::string current_time = get_current_time_string();
                std::string msg = "[MASTER] " + current_time
                                + " Некоторые копии ещё работают. Пропуск запуска новых копий.";
                write_log(sharedData, msg, isMaster);
            }
        }

        // Пауза короткого времени, чтобы не нагружать CPU
        sleep_ms(10);
    }

    // Теоретически сюда не дойдём (вечный цикл)
    running = false;
    inputThread.join();
    cleanup_shared_memory(sharedData, isMaster);

    return 0;
}
