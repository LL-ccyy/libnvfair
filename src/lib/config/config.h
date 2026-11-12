#pragma once

namespace nvfair {

bool parse_memory_from_env(const char* str, unsigned long long* out_bytes);

void config_init_from_env();

unsigned long long config_memory_limit();

}
