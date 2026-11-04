#include "cardputer_cpu.h"
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>

struct CpuProfile {
    const char *label;
    uint32_t clock_hz;
};

// 档位 = 节流目标时钟。节流预算 = instructions × APPROX_CLOCKS_PER_INSTRUCTION /
// clock_hz，只有「目标时间 > 实际耗时」时才会去等 —— 也就是说这套档位只能
// *放慢*，不可能把虚拟 CPU 提速到超过宿主的能力。
// 实测宿主的真实吞吐（dbglean 的 `c:` = 每 2000 条指令的微秒数）≈ 4700-4850µs
// → 约 0.42 MIPS ≈ 6 MHz 级 8086。因此 4.77 MHz 就已经接近「不节流」，
// 8 MHz 以上所有档位（含 33/80 MHz 与 Unlimited）行为完全相同：
// 节流条件永远不成立，虚拟 CPU 一直以宿主的极限速度跑。
// 这一点写在这里是为了避免以后又有人以为「选了 80 MHz 就会变快」。
static const CpuProfile CPU_PROFILES[] = {
    {"4.77 MHz", 4770000},
    {"8 MHz", 8000000},
    {"10 MHz", 10000000},
    {"12 MHz", 12000000},
    {"Unlimited", 0},
    {"16 MHz", 16000000},
    {"24 MHz", 24000000},
    {"33 MHz", 33000000},
    {"80 MHz (turbo)", 80000000},
};

static const uint8_t DEFAULT_PROFILE = 4;
static const uint32_t APPROX_CLOCKS_PER_INSTRUCTION = 4;
static uint8_t current_profile = DEFAULT_PROFILE;
static int64_t next_batch_deadline_us = 0;

void cardputer_cpu_init(void) {
    if (nvs_flash_init() != ESP_OK) return;
    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t saved = DEFAULT_PROFILE;
    if (nvs_get_u8(handle, "cpu_profile", &saved) == ESP_OK &&
        saved < cardputer_cpu_profile_count()) {
        current_profile = saved;
    }
    nvs_close(handle);
}

uint8_t cardputer_cpu_profile(void) {
    return current_profile;
}

bool cardputer_cpu_set_profile(uint8_t profile) {
    if (profile >= cardputer_cpu_profile_count()) return false;
    current_profile = profile;
    next_batch_deadline_us = 0;

    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t result = nvs_set_u8(handle, "cpu_profile", profile);
    const esp_err_t commit_result = result == ESP_OK ? nvs_commit(handle) : result;
    nvs_close(handle);
    return result == ESP_OK && commit_result == ESP_OK;
}

uint8_t cardputer_cpu_profile_count(void) {
    return sizeof(CPU_PROFILES) / sizeof(CPU_PROFILES[0]);
}

const char *cardputer_cpu_profile_label(uint8_t profile) {
    if (profile >= cardputer_cpu_profile_count()) profile = DEFAULT_PROFILE;
    return CPU_PROFILES[profile].label;
}

void cardputer_cpu_throttle(uint32_t instructions) {
    const uint32_t clock_hz = CPU_PROFILES[current_profile].clock_hz;
    if (clock_hz == 0) {
        next_batch_deadline_us = 0;
        return;
    }

    const int64_t now = esp_timer_get_time();
    if (next_batch_deadline_us == 0 || now - next_batch_deadline_us > 100000) {
        next_batch_deadline_us = now;
    }
    next_batch_deadline_us +=
        ((int64_t)instructions * APPROX_CLOCKS_PER_INSTRUCTION * 1000000LL) /
        clock_hz;

    const int64_t wait_us = next_batch_deadline_us - esp_timer_get_time();
    if (wait_us > 0) {
        // 忙等（delayMicroseconds）既不省电也不喂看门狗：它只空转 esp_timer，
        // IDLE 任务照旧跑不到。等得久了就改成真正的阻塞让出（N2-③）。
        // 换算成 tick 向上取整，绝不比要求的等待短。
        if (wait_us >= 2000) {
            const TickType_t ticks =
                (TickType_t)((wait_us + (int64_t)(portTICK_PERIOD_MS * 1000) - 1) /
                             (int64_t)(portTICK_PERIOD_MS * 1000));
            vTaskDelay(ticks > 0 ? ticks : 1);
        } else {
            delayMicroseconds((uint32_t)wait_us);
        }
    }
}
