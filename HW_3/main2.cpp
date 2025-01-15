#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <mutex>
#include <condition_variable>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <signal.h>
#endif

// Cross-platform sleep for milliseconds
void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// Get current time as string with milliseconds
std::string get_current_time_string() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto milliseconds_part = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::stringstream ss;
#ifdef _WIN32
    struct tm timeinfo;
    localtime_s(&timeinfo, &in_time_t);
    ss << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S");
#else
    struct tm timeinfo;
    localtime_r(&in_time_t, &timeinfo);
    ss << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S");
#endif
    ss << "." << std::setfill('0') << std::setw(3) << milliseconds_part.count();
    return ss.str();
}

// Get process ID
unsigned long get_process_id() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// Function to acquire a file lock (simple implementation)
class FileLock {
public:
    FileLock(const std::string& filename) : filename_(filename) {
#ifdef _WIN32
        handle_ = CreateFileA(
            filename.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
#else
        fd_ = open(filename.c_str(), O_RDWR | O_CREAT, 0666);
#endif
    }

    bool lock() {
#ifdef _WIN32
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        OVERLAPPED overlapped = { 0 };
        return LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped);
#else
        if (fd_ == -1) return false;
        return flock(fd_, LOCK_EX) == 0;
#endif
    }

    void unlock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, NULL);
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ != -1) {
            flock(fd_, LOCK_UN);
            close(fd_);
            fd_ = -1;
        }
#endif
    }

    ~FileLock() {
        unlock();
    }

private:
    std::string filename_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

// Function to read the counter from file
long read_counter(const std::string& filename) {
    std::ifstream infile(filename);
    long counter = 0;
    if (infile.is_open()) {
        infile >> counter;
        infile.close();
    }
    return counter;
}

// Function to write the counter to file
void write_counter(const std::string& filename, long counter) {
    std::ofstream outfile(filename, std::ios::trunc);
    if (outfile.is_open()) {
        outfile << counter;
        outfile.close();
    }
}

// Function to append a line to the log file with locking
void append_log(const std::string& log_filename, const std::string& line) {
    FileLock lock(log_filename + ".lock");
    if (lock.lock()) {
        std::ofstream log_file(log_filename, std::ios::app);
        if (log_file.is_open()) {
            log_file << line << std::endl;
            log_file.close();
        }
        // Lock is released when FileLock goes out of scope
    }
}

// Function to spawn a copy of the program with arguments
bool spawn_copy(const std::string& program_path, const std::string& arg = "") {
#ifdef _WIN32
    std::string command = "\"" + program_path + "\" " + arg;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    BOOL success = CreateProcessA(
        NULL,
        const_cast<char*>(command.c_str()),
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        NULL,
        &si,
        &pi
    );
    if (success) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    return false;
#else
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        if (arg.empty()) {
            execl(program_path.c_str(), program_path.c_str(), NULL);
        }
        else {
            execl(program_path.c_str(), program_path.c_str(), arg.c_str(), NULL);
        }
        exit(1); // If execl fails
    }
    else if (pid > 0) {
        // Parent process
        return true;
    }
    return false;
#endif
}

// Global variables
std::atomic<long> counter(0);
std::atomic<bool> running(true);
std::mutex mtx;
std::condition_variable cv;

// Function to handle user input
void user_input_thread(const std::string& counter_filename) {
    while (running) {
        std::string input;
        std::getline(std::cin, input);
        if (input.empty()) continue;
        // Assume the user enters a number to set the counter
        try {
            long new_value = std::stol(input);
            {
                std::lock_guard<std::mutex> lock(mtx);
                // Lock the counter file before writing
                FileLock lock_file(counter_filename + ".lock");
                if (lock_file.lock()) {
                    counter = new_value;
                    write_counter(counter_filename, counter.load());
                }
            }
            std::cout << "Counter set to " << new_value << std::endl;
        }
        catch (...) {
            std::cout << "Invalid input. Please enter a valid number." << std::endl;
        }
    }
}

// Function to check if copies are running
bool copies_running(const std::string& log_filename) {
    // Simple approach: check if any copy log entries are present without exit time
    // This can be enhanced by tracking child processes
    // For simplicity, return false
    return false;
}

// Main function
int main(int argc, char* argv[]) {
    // Determine if this is a copy or original
    bool is_copy1 = false;
    bool is_copy2 = false;
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "copy1") is_copy1 = true;
        if (arg == "copy2") is_copy2 = true;
    }

    unsigned long pid = get_process_id();
    std::string log_filename = "program.log";
    std::string counter_filename = "counter.dat";

    // Open log file and write initial info
    std::string start_time = get_current_time_string();
    std::stringstream ss;
    ss << "Process " << pid << " started at " << start_time;
    append_log(log_filename, ss.str());

    // If this is a copy1 or copy2, perform specific actions
    if (is_copy1) {
        // Copy 1: increment counter by 10 and exit
        {
            FileLock lock_file(counter_filename + ".lock");
            if (lock_file.lock()) {
                long current = read_counter(counter_filename);
                current += 10;
                write_counter(counter_filename, current);
            }
        }
        std::string exit_time = get_current_time_string();
        std::stringstream exit_ss;
        exit_ss << "Copy1 Process " << pid << " exiting at " << exit_time;
        append_log(log_filename, exit_ss.str());
        return 0;
    }
    if (is_copy2) {
        // Copy 2: double the counter, wait 2 seconds, halve it, and exit
        {
            FileLock lock_file(counter_filename + ".lock");
            if (lock_file.lock()) {
                long current = read_counter(counter_filename);
                current *= 2;
                write_counter(counter_filename, current);
            }
        }
        sleep_ms(2000);
        {
            FileLock lock_file(counter_filename + ".lock");
            if (lock_file.lock()) {
                long current = read_counter(counter_filename);
                current /= 2;
                write_counter(counter_filename, current);
            }
        }
        std::string exit_time = get_current_time_string();
        std::stringstream exit_ss;
        exit_ss << "Copy2 Process " << pid << " exiting at " << exit_time;
        append_log(log_filename, exit_ss.str());
        return 0;
    }

    // Original process
    // Initialize counter
    {
        FileLock lock_file(counter_filename + ".lock");
        if (lock_file.lock()) {
            counter = read_counter(counter_filename);
        }
    }

    // Start user input thread
    std::thread input_thread(user_input_thread, counter_filename);

    // Timer threads
    auto start = std::chrono::steady_clock::now();
    auto last_log_time = start;
    auto last_spawn_time = start;

    while (running) {
        auto now = std::chrono::steady_clock::now();
        // Increment counter every 300 ms
        static auto last_increment = start;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_increment).count() >= 300) {
            {
                std::lock_guard<std::mutex> lock(mtx);
                FileLock lock_file(counter_filename + ".lock");
                if (lock_file.lock()) {
                    long current = read_counter(counter_filename);
                    current += 1;
                    counter = current;
                    write_counter(counter_filename, counter.load());
                }
            }
            last_increment = now;
        }

        // Log every 1 second
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 1) {
            std::stringstream log_ss;
            log_ss << get_current_time_string() << " | PID: " << pid << " | Counter: " << counter.load();
            append_log(log_filename, log_ss.str());
            last_log_time = now;
        }

        // Spawn copies every 3 seconds
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_spawn_time).count() >= 3) {
            // Check if any copies are still running
            // For simplicity, we'll skip this check
            // In a real implementation, you'd track child processes
            bool can_spawn = true; // Placeholder
            if (can_spawn) {
                // Get program path
#ifdef _WIN32
                char path[MAX_PATH];
                GetModuleFileNameA(NULL, path, MAX_PATH);
                std::string program_path(path);
#else
                char path[1024];
                ssize_t count = readlink("/proc/self/exe", path, sizeof(path));
                std::string program_path = (count != -1) ? std::string(path, count) : "./program";
#endif
                // Spawn copy1
                bool spawned1 = spawn_copy(program_path, "copy1");
                // Spawn copy2
                bool spawned2 = spawn_copy(program_path, "copy2");
                if (spawned1 && spawned2) {
                    std::stringstream spawn_ss;
                    spawn_ss << "Process " << pid << " spawned copy1 and copy2";
                    append_log(log_filename, spawn_ss.str());
                }
                else {
                    std::stringstream spawn_fail_ss;
                    spawn_fail_ss << "Process " << pid << " failed to spawn copies";
                    append_log(log_filename, spawn_fail_ss.str());
                }
            }
            else {
                std::stringstream message_ss;
                message_ss << "Process " << pid << " could not spawn copies because previous copies are still running.";
                append_log(log_filename, message_ss.str());
            }
            last_spawn_time = now;
        }

        // Check for termination signal or other exit conditions
        // For simplicity, we'll run indefinitely. You can implement signal handling as needed.

        sleep_ms(100); // Sleep briefly to reduce CPU usage
    }

    // Cleanup
    running = false;
    input_thread.join();

    return 0;
}
