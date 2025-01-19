#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>

#define SHARED_MEMORY_NAME "/shared_counter"
#define SHARED_MEMORY_SIZE sizeof(SharedData)
#define LOG_FILE "program_log.txt"
#define SIGNAL_FILE "is_main_program.lock"
#define TIMER_INTERVAL 300 // ms
#define LOG_INTERVAL 1000  // ms
#define COPY_INTERVAL 3000 // ms

typedef struct {
    int counter;
    pthread_mutex_t mutex;
} SharedData;

SharedData *sharedData = NULL;
FILE *logFile = NULL;

// Функция записи в лог
void log_message(const char *message) {
    time_t rawtime;
    struct tm *timeinfo;
    char timeBuffer[80];
    
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    fprintf(logFile, "[%s] %s\n", timeBuffer, message);
    fflush(logFile);
}

// Инициализация разделяемой памяти
int initialize_shared_memory() {
    int shm_fd = shm_open(SHARED_MEMORY_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        return -1;
    }

    // Убедитесь, что размер положительный
    if (ftruncate(shm_fd, SHARED_MEMORY_SIZE) == -1) {
        perror("ftruncate");
        close(shm_fd);
        return -1;
    }

    void *ptr = mmap(0, SHARED_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        return -1;
    }

    sharedData = (SharedData *)ptr;

    // Проверяем, нужно ли инициализировать данные
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);

    if (pthread_mutex_init(&sharedData->mutex, &attr) != 0) {
        perror("pthread_mutex_init");
        munmap(ptr, SHARED_MEMORY_SIZE);
        close(shm_fd);
        return -1;
    }

    // Инициализация счетчика (если это первая программа)
    sharedData->counter = 0;

    close(shm_fd);
    return 0;
}


// Закрытие разделяемой памяти
void cleanup_shared_memory() {
    if (sharedData != NULL) {
        munmap(sharedData, SHARED_MEMORY_SIZE);
        sharedData = NULL;
    }
}

// Функция для работы таймера, увеличивающего счётчик
void *timer_thread(void *arg) {
    while (1) {
        usleep(TIMER_INTERVAL * 1000);  // 300 мс
        pthread_mutex_lock(&sharedData->mutex);
        sharedData->counter++;
        pthread_mutex_unlock(&sharedData->mutex);
    }
    return NULL;
}

// Функция для запуска копий процесса
void spawn_copy(int copy_id) {
    pid_t pid = fork();
    if (pid == 0) {
        char message[256];
        pthread_mutex_lock(&sharedData->mutex);

        if (copy_id == 1) {
            sharedData->counter += 10;  // Копия 1 увеличивает счётчик на 10
        } else if (copy_id == 2) {
            sharedData->counter *= 2;  // Копия 2 умножает счётчик на 2
        }
        pthread_mutex_unlock(&sharedData->mutex);

        if (copy_id == 2) {
            usleep(2000000);  // Ждём 2 секунды
            pthread_mutex_lock(&sharedData->mutex);
            sharedData->counter /= 2;  // Делим счётчик на 2
            pthread_mutex_unlock(&sharedData->mutex);
        }

        pthread_mutex_lock(&sharedData->mutex);
        snprintf(message, sizeof(message), "Copy %d finished - Counter: %d", copy_id, sharedData->counter);
        pthread_mutex_unlock(&sharedData->mutex);
        log_message(message);
        exit(0);
    }
}

// Функция для записи лога (если программа главная)
void *log_thread(void *arg) {
    while (1) {
        usleep(LOG_INTERVAL * 1000);  // 1 секунда
        char message[256];
        pthread_mutex_lock(&sharedData->mutex);
        snprintf(message, sizeof(message), "Process ID: %d - Counter: %d", getpid(), sharedData->counter);
        pthread_mutex_unlock(&sharedData->mutex);
        log_message(message);
    }
    return NULL;
}

// Обработка пользовательского ввода
void handle_user_input() {
    while (1) {
        int newValue;
        printf("Enter new counter value: ");
        scanf("%d", &newValue);

        pthread_mutex_lock(&sharedData->mutex);
        sharedData->counter = newValue;
        pthread_mutex_unlock(&sharedData->mutex);
    }
}

int main() {
    // Открытие лог-файла
    logFile = fopen(LOG_FILE, "a");
    if (logFile == NULL) {
        perror("Error opening log file");
        return 1;
    }

    // Инициализация разделяемой памяти
    if (initialize_shared_memory() != 0) {
        fprintf(stderr, "Failed to initialize shared memory\n");
        return 1;
    }

    // Проверяем, является ли программа главной
    int is_main_program = access(SIGNAL_FILE, F_OK) == -1;

    if (is_main_program) {
        // Создаём файл-сигнал
        FILE *signalFile = fopen(SIGNAL_FILE, "w");
        if (signalFile) fclose(signalFile);
        log_message("Main program started");
    } else {
        log_message("Secondary program started");
    }

    // Запускаем таймер в отдельном потоке
    pthread_t timerThread;
    pthread_create(&timerThread, NULL, timer_thread, NULL);

    if (is_main_program) {
        // Если программа главная, запускаем поток для записи лога
        pthread_t loggerThread;
        pthread_create(&loggerThread, NULL, log_thread, NULL);

        // Главный цикл для запуска копий
        while (1) {
            usleep(COPY_INTERVAL * 1000);  // 3 секунды
            spawn_copy(1);
            spawn_copy(2);
        }
    } else {
        // Второстепенные программы только обрабатывают ввод пользователя
        handle_user_input();
    }

    // Завершаем работу
    cleanup_shared_memory();
    fclose(logFile);
    return 0;
}
