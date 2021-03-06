// Copyright (c) 2010 Baidu, Inc.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Author: Ge,Jun (gejun@baidu.com)
// Date: Fri Aug 29 15:01:15 CST 2014

#include <unistd.h>                          // close
#include <sys/types.h>                       // open
#include <sys/stat.h>                        // ^
#include <fcntl.h>                           // ^

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string.h>                          // memmem
#undef _GNU_SOURCE
#include <ctype.h>

#include "butil/time.h"

#if defined(NO_CLOCK_GETTIME_IN_MAC)
#include <mach/clock.h>                      // mach_absolute_time
#include <mach/mach_time.h>                  // mach_timebase_info
#include <pthread.h>                         // pthread_once
#include <stdlib.h>                          // exit

static mach_timebase_info_data_t s_timebase;
static timespec s_init_time;
static uint64_t s_init_ticks;
static pthread_once_t s_init_clock_once = PTHREAD_ONCE_INIT;

static void InitClock() {
    if (mach_timebase_info(&s_timebase) != 0) {
        exit(1);
    }
    timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        exit(1);
    }
    s_init_time.tv_sec = now.tv_sec;
    s_init_time.tv_nsec = now.tv_usec * 1000L;
    s_init_ticks = mach_absolute_time();
}

int clock_gettime(clockid_t id, timespec* time) {
    if (pthread_once(&s_init_clock_once, InitClock) != 0) {
        exit(1);
    }
    uint64_t clock = mach_absolute_time() - s_init_ticks;
    uint64_t elapsed = clock * (uint64_t)s_timebase.numer / (uint64_t)s_timebase.denom;
    *time = s_init_time;
    time->tv_sec += elapsed / 1000000000L;
    time->tv_nsec += elapsed % 1000000000L;
    time->tv_sec += time->tv_nsec / 1000000000L;
    time->tv_nsec = time->tv_nsec % 1000000000L;
    return 0;
}

#endif

namespace butil {

int64_t monotonic_time_ns() {
    // MONOTONIC_RAW is slower than MONOTONIC in linux 2.6.32, trying to
    // use the RAW version does not make sense anymore.
    // NOTE: Not inline to keep ABI-compatible with previous versions.
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000000L + now.tv_nsec;
}

namespace detail {

static int64_t read_cpu_current_frequency(char* buf, ssize_t n) {
    /* We read the information from the /proc filesystem.  It contains at
       least one line like
       cpu MHz         : 497.840237
       or also
       cpu MHz         : 497.841
       We search for this line and convert the number in an integer.  */

    int64_t result = 0;
    char *mhz = static_cast<char*>(memmem(buf, n, "cpu MHz", 7));
    if (mhz != NULL) {
        char *endp = buf + n;
        int seen_decpoint = 0;
        int ndigits = 0;

        /* Search for the beginning of the string.  */
        while (mhz < endp && (*mhz < '0' || *mhz > '9') && *mhz != '\n') {
            ++mhz;
        }
        while (mhz < endp && *mhz != '\n') {
            if (*mhz >= '0' && *mhz <= '9') {
                result *= 10;
                result += *mhz - '0';
                if (seen_decpoint)
                    ++ndigits;
            } else if (*mhz == '.') {
                seen_decpoint = 1;
            }
            ++mhz;
        }

        /* Compensate for missing digits at the end.  */
        while (ndigits++ < 6) {
            result *= 10;
        }
    }
    return result;
}

#if defined(__x86_64__) || defined(__i386__)
#if defined(__pic__) && defined(__i386__)
static void __cpuid(uint32_t reg[4], uint32_t code) {
    __asm__ volatile (
        "mov %%ebx, %%edi\n"
        "cpuid\n"
        "xchg %%edi, %%ebx\n"
        : "=a"(reg[0]), "=D"(reg[1]), "=c"(reg[2]), "=d"(reg[3])
        : "a"(code)
  );
}
#else
static void __cpuid(uint32_t reg[4], uint32_t code) {
    __asm__ volatile (
        "cpuid \n\t"
        : "=a"(reg[0]), "=b"(reg[1]), "=c"(reg[2]), "=d"(reg[3])
        : "a"(code)
    );
}
#endif
#endif

static int64_t read_cpu_frequency_by_cpuid() {
    int64_t result = 0;
#if defined(__x86_64__) || defined(__i386__)
    uint32_t reg[4];
    __cpuid(reg, 0);
    if (reg[0] >= 0x16 && reg[1] == 0x756e6547 && reg[2] == 0x6c65746e && reg[3] == 0x49656e69) {
        //Intel CPU only
        __cpuid(reg, 0x16);
        return static_cast<uint64_t>(reg[0]) * 1000000UL;
    }
#endif
    return result;
}

static int64_t read_cpu_frequency_from_brand_string() {
    int64_t result = 0;
#if defined(__x86_64__) || defined(__i386__)
    union {
        char brand[48];
        uint32_t reg[12];
    } buf;
    __cpuid(buf.reg, 0x80000000);
    if (buf.reg[0] < 0x80000004) {
        return 0;
    }
    __cpuid(buf.reg, 0x80000002);
    __cpuid(buf.reg+4, 0x80000003);
    __cpuid(buf.reg+8, 0x80000004);
    //Get something like: Intel(R) Xeon(R) CPU E5-2620 v2 @ 2.10GHz
    char* end = buf.brand + sizeof(buf.brand);
    char* p = buf.brand;
    while (p != end && *p != '@') {
        if (*p++ == '\n') {
            return 0;
        }
    }
    while (p != end && !isdigit(*p)) {
        p++;
    }
    //expect x.xxGhz
    //FSB may be 0.10GHz or 0.133...GHz
    if (end - p < 7 || p[1] != '.'
        || !isdigit(p[2]) || !isdigit(p[3]) ||  p[4] != 'G') {
        return 0;
    }
    result = (p[0]-'0') * 10 + (p[2]-'0');
    int64_t last = p[3] - '0';
    if (last == 7) {
        last = 6;
    }
    for (int i = 0; i < 8; i++) {
        result = result * 10 + last;
    }
#endif
    return result;
}

// read_cpu_frequency() is modified from source code of glibc.
int64_t read_cpu_frequency(bool* invariant_tsc) {
    const int fd = open("/proc/cpuinfo", O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    char buf[4096];  // should be enough
    const ssize_t n = read(fd, buf, sizeof(buf));
    close (fd);
    if (n <= 0) {
        return 0;
    }

    if (invariant_tsc) {
        char* flags_pos = static_cast<char*>(memmem(buf, n, "flags", 5));
        if (flags_pos
            && memmem(flags_pos, buf + n - flags_pos, "constant_tsc", 12)
            && memmem(flags_pos, buf + n - flags_pos, "nonstop_tsc", 11)) {
            int64_t result = read_cpu_frequency_by_cpuid();
            if (result <= 0) {
                result = read_cpu_frequency_from_brand_string();
            }
            if (result > 0) {
                *invariant_tsc = true;
                return result;
            }
        }
        //current frequency is usually not invariant
        *invariant_tsc = false; 
    }
    return read_cpu_current_frequency(buf, n);
}

// Return value must be >= 0
int64_t read_invariant_cpu_frequency() {
    bool invariant_tsc = false;
    const int64_t freq = read_cpu_frequency(&invariant_tsc);
    if (!invariant_tsc || freq < 0) {
        return 0;
    }
    return freq;
}

int64_t invariant_cpu_freq = -1;
}  // namespace detail

}  // namespace butil
