#include <iostream>
#include <cstring>

#ifndef _WIN32
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

int main() {
    #ifndef _WIN32
    const char* shm_name = "/mysharedmemory";
    if (shm_unlink(shm_name) == 0) {
        std::cout << "Разделяемая память " << shm_name << " успешно удалена." << std::endl;
    } else {
        perror("shm_unlink");
    }
    #else
    std::cerr << "Удаление разделяемой памяти на Windows не требуется." << std::endl;
    #endif
    return 0;
}
