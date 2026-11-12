#include "util/log/log.h"
#include <cstdlib>
#include <cctype>
#include <cerrno>
namespace nvfair {

static unsigned long long g_memory_limit = 0;

bool parse_memory_from_env(const char* str, unsigned long long* out_bytes){
    if(str == nullptr || out_bytes == nullptr){
        return false;
    }
    const char* p = str;
    while(std::isspace(static_cast<unsigned char>(*p))){
        ++p;
    }
    if(!std::isdigit(static_cast<unsigned char>(*p))){
        return false;
    }

    errno = 0;
    char* end = nullptr;
    unsigned long long digits = std::strtoull(p,&end,10);
    if (end == p || errno == ERANGE) {
        return false;
    }
    unsigned long long scalar = 1;
    bool has_unit = false;
    switch (*end) {
        case 'k':case 'K': scalar = 1000ULL;        has_unit = true; break;
        case 'm':case 'M': scalar = 1000000ULL;     has_unit = true; break;
        case 'g':case 'G': scalar = 1000000000ULL;  has_unit = true; break;
        default: break;
    }
    if (has_unit) {
        ++end;
        if (*end == 'i' || *end == 'I') {
            switch (*(end - 1)) {
                case 'k':case 'K': scalar = 1ULL << 10; break;
                case 'm':case 'M': scalar = 1ULL << 20; break;
                case 'g':case 'G': scalar = 1ULL << 30; break;
            }
            ++end;
        }
        if (*end == 'b' || *end == 'B') {
            ++end;
        }
    }
    while (std::isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }
    if (*end != '\0') {
        return false;
    }
    if (digits > 0xFFFFFFFFFFFFFFFFULL / scalar) {
        return false;
    }
    *out_bytes = digits * scalar;
    return true;
}

void config_init_from_env(){
    const char* env = std::getenv("NVFAIR_MEMORY_LIMIT");
    if(env == nullptr){
        return;
    }
    unsigned long long bytes = 0;
    if(!parse_memory_from_env(env, &bytes)){
        log_write(LogLevel::Warn, "Invalid memory limit: %s", env);
        return;
    }
    g_memory_limit = bytes;
    if(g_memory_limit == 0){
        log_write(LogLevel::Info, "memory limit set to 0, no limit");
    }
}

unsigned long long config_memory_limit(){
    return g_memory_limit;
}


}
