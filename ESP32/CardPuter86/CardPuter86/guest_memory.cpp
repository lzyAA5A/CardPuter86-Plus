#include "guest_memory.h"
#include "gbConfig.h"
#include <esp_partition.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <wear_levelling.h>
#include <esp_heap_caps.h>

static const char *SWAP_PARTITION_LABEL = "swap";
static const uint32_t PAGE_SIZE = 4096;
// 客机内存三档（设置菜单可选）。运行期 ram_kb 决定 guest_memory_size()，
// 页表按最大档 512 KB 静态开好（page_to_slot 128 B + swap_valid 16 B，白送）。
static const uint32_t RAM_KB_LOW = 128;
static const uint32_t RAM_KB_MID = 256;
static const uint32_t RAM_KB_HIGH = 512;
static const uint16_t PAGE_COUNT_MAX = 512 * 1024 / 4096;   // 128 页
static uint32_t ram_kb = RAM_KB_MID;
static uint16_t page_count = RAM_KB_MID * 1024 / 4096;      // 64 页
// 页缓存上限（编译期）；运行时按「模式需求 + 堆余量」下调
#ifndef G86_CACHE_PAGES_MAX
#define G86_CACHE_PAGES_MAX 32
#endif
static const uint8_t CACHE_PAGE_MAX = G86_CACHE_PAGES_MAX;
static uint8_t cache_pages = 0;
static const int8_t NO_SLOT = -1;
static const uint8_t NO_PAGE = 0xFF;

static const esp_partition_t *swap_partition = nullptr;
static wl_handle_t swap_wl = WL_INVALID_HANDLE;
static uint8_t *cache_data = nullptr;
static int8_t page_to_slot[PAGE_COUNT_MAX];
static uint8_t slot_page[CACHE_PAGE_MAX];
static uint64_t slot_age[CACHE_PAGE_MAX];
static bool slot_dirty[CACHE_PAGE_MAX];
static uint8_t swap_valid[(PAGE_COUNT_MAX + 7) / 8];
static uint64_t access_clock = 0;
static bool mode_512k = false;

/* Hot path mirror of the page cache (see guest_memory.h).  read86()/write86()
   are IRAM_ATTR and run several times per guest instruction, so they index
   gb_gm_hot_base directly and only call back here when the accessed page
   changes. static_assert below keeps the 12-bit shift honest. */
uint8_t *gb_gm_data = nullptr;
uint32_t gb_gm_limit = 128 * 1024;
uint16_t gb_gm_hot_page = 0xFFFF;
uint8_t *gb_gm_hot_base = nullptr;
bool    *gb_gm_hot_dirty = nullptr;
static_assert(PAGE_SIZE == (1u << GUEST_PAGE_SHIFT), "hot path assumes 4 KB pages");
static void sync_hot_globals(void);

// 诊断：记录 512KB 决策链上每一步的返回值，仅供 use_lib_log_serial 打印。
static int32_t  diag_nvs_init  = 0x7FFF;
static int32_t  diag_nvs_open  = 0x7FFF;
static int32_t  diag_nvs_get   = 0x7FFF;
static int32_t  diag_part_found = -1;
static int32_t  diag_wl_mount  = 0x7FFF;
static uint32_t diag_wl_ssize  = 0;
static uint32_t diag_wl_size   = 0;
static char     diag_text[192];

// CDP86 诊断：flash 换页到底多贵？512K 模式的可用性完全由这几个数决定。
static uint32_t swap_ops     = 0;   // 淘汰次数（每次 = 一次 4KB 擦 + 写）
static uint32_t swap_loads   = 0;   // 从 swap 读回次数
static uint32_t swap_us_ops  = 0;   // write_back 累计微秒
static uint32_t swap_us_loads = 0;  // 读回累计微秒
static char     selftest_text[128];

static uint32_t load_ram_kb_setting(void) {
#ifdef G86_FORCE_RAMKB
    // 编译期强制档位：仅用于真机验证（串口写方向可能不通，只能靠日志读回）。
    // pio run -e cardputer86-dbglean，build_flags 加 -DG86_FORCE_RAMKB=512
    diag_nvs_get = (int32_t)G86_FORCE_RAMKB;
    return G86_FORCE_RAMKB;
#endif
    // 默认 256 KB：MS-DOS 5.0 的 IO.SYS(33430)+MSDOS.SYS(37394)+
    // COMMAND.COM(47845) 约 118 KB，128 KB 的机器装不下 —— 实测 IO.SYS
    // 在自重定位阶段（guest 1087:0998 的 REP MOVSB）就把目的地址顶出
    // 128 KB 顶端，随后崩进 F000 段的 ROM 空洞。
    // NVS 键：ramkb = 128/256/512。旧固件只存布尔 ram512（0=128K/1=256K），
    // 用 ram512v=3 做一次性迁移，任何读不到显式设置的情形都按 256 KB 处理。
    diag_nvs_init = (int32_t)nvs_flash_init();
    if (diag_nvs_init != ESP_OK) return RAM_KB_MID;
    nvs_handle_t handle;
    diag_nvs_open = (int32_t)nvs_open("cardputer86", NVS_READWRITE, &handle);
    if (diag_nvs_open != ESP_OK) return RAM_KB_MID;

    uint8_t schema = 0;
    if (nvs_get_u8(handle, "ram512v", &schema) != ESP_OK || schema < 3) {
        // 迁移一律落到 256 KB（DOS 5.0 的最低要求、也是推荐值）。旧固件里的
        // ram512 可能被历史固件默认写成 0（128 KB），先前的迁移因此读到 128K，
        // 于是开机走「128K 要 32 页整块常驻」→ 拿不到 32 页 → 退回分页模式这条
        // 又慢又绕的路。直接给 256 KB，用户在设置菜单里随时可以改。
        nvs_set_u16(handle, "ramkb", (uint16_t)RAM_KB_MID);
        nvs_set_u8(handle, "ram512v", 3);
        nvs_commit(handle);
    }
    uint16_t kb = (uint16_t)RAM_KB_MID;
    diag_nvs_get = (int32_t)nvs_get_u16(handle, "ramkb", &kb);
    nvs_close(handle);
    if (diag_nvs_get != ESP_OK) return RAM_KB_MID;
    if (kb != RAM_KB_LOW && kb != RAM_KB_MID && kb != RAM_KB_HIGH) return RAM_KB_MID;
    return kb;
}

static void save_ram_kb_setting(uint32_t kb) {
    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u16(handle, "ramkb", (uint16_t)kb);
    nvs_set_u8(handle, "ram512v", 3);   // 标记为显式设定，不再走迁移
    nvs_commit(handle);
    nvs_close(handle);
}

static bool mount_swap(void) {
    if (swap_wl != WL_INVALID_HANDLE) return true;
    swap_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        SWAP_PARTITION_LABEL);
    diag_part_found = swap_partition ? 1 : 0;
    if (!swap_partition) return false;
    diag_wl_mount = (int32_t)wl_mount(swap_partition, &swap_wl);
    if (diag_wl_mount != ESP_OK) {
        if (swap_wl != WL_INVALID_HANDLE) wl_unmount(swap_wl);
        swap_wl = WL_INVALID_HANDLE;
        return false;
    }
    diag_wl_ssize = (uint32_t)wl_sector_size(swap_wl);
    diag_wl_size = (uint32_t)wl_size(swap_wl);
    if (diag_wl_ssize != PAGE_SIZE || diag_wl_size < ram_kb * 1024) {
        wl_unmount(swap_wl);
        swap_wl = WL_INVALID_HANDLE;
        return false;
    }
    return true;
}

static bool swap_page_valid(uint8_t page) {
    return swap_valid[page >> 3] & (1U << (page & 7));
}

static void set_swap_page_valid(uint8_t page) {
    swap_valid[page >> 3] |= 1U << (page & 7);
}

static bool write_back(uint8_t slot) {
    if (!slot_dirty[slot] || slot_page[slot] == NO_PAGE) return true;
    // 防御：swap 未挂载时绝不能再擦页（否则 wl_erase_range 刷 invalid handle）
    if (swap_wl == WL_INVALID_HANDLE) { slot_dirty[slot] = false; return true; }
    const size_t offset = (size_t)slot_page[slot] * PAGE_SIZE;
    const uint32_t cdp_t0 = micros();
    const bool cdp_ok =
        wl_erase_range(swap_wl, offset, PAGE_SIZE) == ESP_OK &&
        wl_write(swap_wl, offset,
                 cache_data + slot * PAGE_SIZE, PAGE_SIZE) == ESP_OK;
    swap_us_ops += micros() - cdp_t0;
    swap_ops++;
    if (!cdp_ok) {
        return false;
    }
    set_swap_page_valid(slot_page[slot]);
    slot_dirty[slot] = false;
    return true;
}

static int8_t load_page(uint8_t page) {
    int8_t slot = page_to_slot[page];
    if (slot != NO_SLOT) {
        slot_age[slot] = ++access_clock;
        return slot;
    }

    // A miss may recycle the page the hot cache is pointing at, so drop the hot
    // mapping before the victim slot changes hands (guest_hot_page_map() sets it
    // again right afterwards when the miss came from the hot path).
    gb_gm_hot_page = 0xFFFF;
    gb_gm_hot_base = nullptr;
    gb_gm_hot_dirty = nullptr;

    uint8_t victim = 0;
    for (uint8_t i = 0; i < cache_pages; i++) {
        if (slot_page[i] == NO_PAGE) {
            victim = i;
            break;
        }
        if (slot_age[i] < slot_age[victim]) victim = i;
    }

    if (!write_back(victim)) return NO_SLOT;
    if (slot_page[victim] != NO_PAGE) page_to_slot[slot_page[victim]] = NO_SLOT;

    uint8_t *destination = cache_data + victim * PAGE_SIZE;
    if (swap_page_valid(page)) {
        const uint32_t cdp_t0 = micros();
        const bool cdp_ok = wl_read(swap_wl, (size_t)page * PAGE_SIZE,
                                    destination, PAGE_SIZE) == ESP_OK;
        swap_us_loads += micros() - cdp_t0;
        swap_loads++;
        if (!cdp_ok) {
            return NO_SLOT;
        }
    } else {
        memset(destination, 0, PAGE_SIZE);
    }

    slot_page[victim] = page;
    slot_age[victim] = ++access_clock;
    slot_dirty[victim] = false;
    page_to_slot[page] = victim;
    return victim;
}

bool guest_memory_init(void) {
    static_assert(gb_max_ram <= 512 * 1024,
                  "swap layout supports at most 512 KB of guest RAM");
    ram_kb = load_ram_kb_setting();
    // 分页模式必须有 swap 落点：512 KB 拿不到就退 256 KB，再拿不到退 128 KB。
    if (ram_kb > RAM_KB_LOW && !mount_swap()) {
        ram_kb = RAM_KB_MID;
        if (!mount_swap()) ram_kb = RAM_KB_LOW;
    }
    page_count = (uint16_t)(ram_kb * 1024 / PAGE_SIZE);
    mode_512k = ram_kb > RAM_KB_LOW;
    // 128K 模式必须凑满 32 页（=128KB）才能整块驻留；少一页就会出现淘汰，
    // 而 128K 模式下 swap 未挂载，淘汰脏页会调用 wl_erase_range(无效句柄)
    // 刷屏并卡死模拟器（本 bug 已实测复现）。
    const uint8_t resident_all = (uint8_t)(128 * 1024 / PAGE_SIZE);   // 32
    const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t max_now = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    uint8_t tried_pages = 0;

    if (!mode_512k) {
        // 128K 模式要求 32 页整块常驻（此时还没有 swap 落点，缺页会擦无效句柄
        // 卡死）。拿不到 32 页就退回分页模式，宁可慢也别起不来。
        cache_data = static_cast<uint8_t *>(malloc((size_t)resident_all * PAGE_SIZE));
        if (cache_data) {
            cache_pages = resident_all;
        } else if (mount_swap()) {
            ram_kb = RAM_KB_MID;
            page_count = (uint16_t)(ram_kb * 1024 / PAGE_SIZE);
            mode_512k = true;
        } else {
            snprintf(diag_text, sizeof(diag_text),
                     "FAIL 128K needs %u resident pages, free=%u max=%u, no swap",
                     (unsigned)resident_all, (unsigned)free_now, (unsigned)max_now);
            return false;
        }
    }
    if (mode_512k) {
        // 分页模式：淘汰有 swap 落点，常驻页可以远少于总页数。
        // RESERVE 必须真的留出来 —— 这个函数跑在内部盘 FatFS 挂载之前，而
        // FatFS 每个打开文件要 4KB（FF_MAX_SS）连续块，WiFi 还要 ~62KB。
        // 实测不留余量时页缓存吃 114KB，最大连续块只剩 10,228B →
        // `[flash] mount rc=0x101 (ESP_ERR_NO_MEM)` → images=0 → 没有启动盘。
        // 实测：留 80KB 时缓存 17 页、内部盘 FatFS 还剩 69KB 可用；留 68KB
        // 时缓存 20 页、FatFS 仍能挂上（它只要 >10KB 连续块）。再多就会挤掉
        // 内部盘的挂载，见上一条序列的 `[flash] mount rc=0x101`。
        // 无 WiFi 的 lean 构建堆更宽裕，理论能拿更多页（每多一页就少一次
        // 「擦 4KB + 写 4KB」≈55ms 停顿），但只有带串口日志的构建才测得准页数，
        // 而未经验证就压低 RESERVE 会直接重现那次「内部盘挂不上 → 黑屏」的
        // 回归，所以这里保持 68KB 统一值，页数改为打印到屏幕/日志里观察。
        const size_t RESERVE = 68 * 1024;
        const uint8_t floor_pages = (uint8_t)(48 * 1024 / PAGE_SIZE);   // 12 页
        uint8_t want = (page_count < CACHE_PAGE_MAX) ? (uint8_t)page_count
                                                     : CACHE_PAGE_MAX;
        size_t can = free_now > RESERVE ? (free_now - RESERVE) / PAGE_SIZE : 0;
        if (can < want) want = (uint8_t)can;
        if (want < floor_pages) want = floor_pages;
        // 一路退到 4 页（16KB）：堆碎成什么样只有真去试才知道。
        for (uint8_t pages = want; pages >= 4; pages = (uint8_t)(pages - 4)) {
            tried_pages = pages;
            cache_data = static_cast<uint8_t *>(malloc((size_t)pages * PAGE_SIZE));
            if (cache_data) { cache_pages = pages; break; }
        }
        if (!cache_data || cache_pages == 0) {
            snprintf(diag_text, sizeof(diag_text),
                     "FAIL pages=%u free=%u max=%u ramkb=%lu wl=%d wlSize=%lu",
                     (unsigned)tried_pages, (unsigned)free_now, (unsigned)max_now,
                     (unsigned long)ram_kb, (int)diag_wl_mount,
                     (unsigned long)diag_wl_size);
            return false;
        }
    }
    // 兜底：分页模式下 swap 必须真的挂着，否则淘汰脏页会擦无效句柄卡死。
    if (mode_512k && swap_wl == WL_INVALID_HANDLE) {
        ram_kb = RAM_KB_LOW;
        page_count = (uint16_t)(ram_kb * 1024 / PAGE_SIZE);
        mode_512k = false;
    }
    guest_memory_clear();
    sync_hot_globals();
#ifdef use_lib_log_serial
    // CDP86 诊断（仅 dbg/dbglean）：启动时证明 512KB 分页层真的能往返。
    // 512K 模式下这一步要跑数百次 flash 淘汰，会拖慢启动几秒，故只在调试固件里开。
    guest_memory_selftest();
    guest_memory_clear();   // 自检把整块 RAM 填成了图案，必须复位
    swap_ops = swap_loads = swap_us_ops = swap_us_loads = 0;
#endif
    // guest_memory_init() 跑在 USB CDC 就绪之前，直接 Serial.print 会被丢掉，
    // 所以只格式化到缓冲区，由调用方（boot 日志）在串口可用后打印。
    snprintf(diag_text, sizeof(diag_text),
             "nvsInit=%d nvsOpen=%d nvsGet=%d part=%d wlMount=%d "
             "wlSec=%lu wlSize=%lu ramkb=%lu pages=%u free=%u",
             (int)diag_nvs_init, (int)diag_nvs_open, (int)diag_nvs_get,
             (int)diag_part_found, (int)diag_wl_mount,
             (unsigned long)diag_wl_ssize, (unsigned long)diag_wl_size,
             (unsigned long)ram_kb, (unsigned)cache_pages, (unsigned)free_now);
    return true;
}

const char *guest_memory_diag_text(void) {
    return diag_text[0] ? diag_text : "(guest_memory_init not called)";
}

// CDP86 诊断：512 KB 分页层往返自检。
// 顺序写满整块 guest RAM 再读回逐字节比对：任何一次淘汰/换入出错都会在这里
// 暴露成「首个坏地址」，不用再去猜 DOS 为什么崩。
bool guest_memory_selftest(void) {
    selftest_text[0] = '\0';
    if (!cache_data || cache_pages == 0) {
        snprintf(selftest_text, sizeof(selftest_text), "SKIP no cache");
        return false;
    }
    const uint32_t total = guest_memory_size();
    const uint32_t t0 = micros();
    for (uint32_t a = 0; a < total; a++) {
        guest_memory_write(a, (uint8_t)((a * 31u) ^ (a >> 8) ^ 0x5Au));
    }
    for (uint32_t a = 0; a < total; a++) {
        const uint8_t want = (uint8_t)((a * 31u) ^ (a >> 8) ^ 0x5Au);
        const uint8_t got = guest_memory_read(a);
        if (got != want) {
            snprintf(selftest_text, sizeof(selftest_text),
                     "FAIL addr=0x%05lX page=%lu want=%02X got=%02X",
                     (unsigned long)a, (unsigned long)(a / PAGE_SIZE),
                     (unsigned)want, (unsigned)got);
            return false;
        }
    }
    snprintf(selftest_text, sizeof(selftest_text),
             "PASS %luB %luus evict=%lu(%luus/ea) reload=%lu(%luus/ea)",
             (unsigned long)total, (unsigned long)(micros() - t0),
             (unsigned long)swap_ops,
             (unsigned long)(swap_ops ? swap_us_ops / swap_ops : 0),
             (unsigned long)swap_loads,
             (unsigned long)(swap_loads ? swap_us_loads / swap_loads : 0));
    return true;
}

const char *guest_memory_selftest_text(void) {
    return selftest_text[0] ? selftest_text : "(selftest not run)";
}

void guest_memory_swap_stats(uint32_t *ops, uint32_t *us_ops, uint32_t *loads,
                             uint32_t *us_loads) {
    if (ops) *ops = swap_ops;
    if (us_ops) *us_ops = swap_us_ops;
    if (loads) *loads = swap_loads;
    if (us_loads) *us_loads = swap_us_loads;
}

bool guest_memory_set_512k_enabled(bool enabled) {
    return guest_memory_set_ram_kb(enabled ? RAM_KB_MID : RAM_KB_LOW);
}

bool guest_memory_set_ram_kb(uint32_t kb) {
    if (kb != RAM_KB_LOW && kb != RAM_KB_MID && kb != RAM_KB_HIGH) return false;
    // 512 KB 需要 swap 落点（分区 0x90000 = 576 KB，装得下 512 KB 脏页）
    if (kb > RAM_KB_LOW && !mount_swap()) return false;
    guest_memory_clear();
    if (kb == RAM_KB_LOW && swap_wl != WL_INVALID_HANDLE) {
        wl_unmount(swap_wl);
        swap_wl = WL_INVALID_HANDLE;
    }
    ram_kb = kb;
    page_count = (uint16_t)(ram_kb * 1024 / PAGE_SIZE);
    mode_512k = ram_kb > RAM_KB_LOW;
    save_ram_kb_setting(kb);
    // 大小变了：热路径的 gb_gm_limit 必须同步，否则放大的那一段会被当成 ROM 读回 0
    sync_hot_globals();
    return true;
}

uint8_t guest_memory_cache_pages(void) { return cache_pages; }

bool guest_memory_512k_enabled(void) {
    return mode_512k;
}

uint32_t guest_memory_ram_kb(void) {
    return ram_kb;
}

uint32_t guest_memory_size(void) {
    return ram_kb * 1024;
}

/* Keep the hot path's mirror of guest_memory_size()/cache_data in sync. */
static void sync_hot_globals(void) {
    gb_gm_data = cache_data;
    gb_gm_limit = guest_memory_size();
    gb_gm_hot_page = 0xFFFF;
    gb_gm_hot_base = nullptr;
    gb_gm_hot_dirty = nullptr;
}


void guest_memory_clear(void) {
    memset(page_to_slot, NO_SLOT, sizeof(page_to_slot));
    memset(slot_page, NO_PAGE, sizeof(slot_page));
    memset(slot_age, 0, sizeof(slot_age));
    memset(slot_dirty, 0, sizeof(slot_dirty));
    memset(swap_valid, 0, sizeof(swap_valid));
    if (cache_data && cache_pages) memset(cache_data, 0, (size_t)cache_pages * PAGE_SIZE);
    access_clock = 0;
    gb_gm_hot_page = 0xFFFF;
    gb_gm_hot_base = nullptr;
    gb_gm_hot_dirty = nullptr;
}

/* Slow half of the hot path: map `page` into gb_gm_hot_base. Lives in flash,
   called only when the guest crosses a page boundary. */
bool guest_hot_page_map(uint16_t page) {
    if (!cache_data || page >= page_count) {
        gb_gm_hot_page = 0xFFFF;
        gb_gm_hot_base = nullptr;
        gb_gm_hot_dirty = nullptr;
        return false;
    }
    const int8_t slot = load_page((uint8_t)page);
    if (slot == NO_SLOT) {
        gb_gm_hot_page = 0xFFFF;
        gb_gm_hot_base = nullptr;
        gb_gm_hot_dirty = nullptr;
        return false;
    }
    gb_gm_hot_base = cache_data + (uint32_t)slot * PAGE_SIZE;
    gb_gm_hot_dirty = &slot_dirty[slot];
    gb_gm_hot_page = page;
    return true;
}


uint8_t guest_memory_read(uint32_t address) {
    if (!cache_data || address >= guest_memory_size()) return 0;
    const uint8_t page = address / PAGE_SIZE;
    const int8_t slot = load_page(page);
    if (slot == NO_SLOT) return 0;
    return cache_data[(uint32_t)slot * PAGE_SIZE + (address & (PAGE_SIZE - 1))];
}

void guest_memory_write(uint32_t address, uint8_t value) {
    if (!cache_data || address >= guest_memory_size()) return;
    const uint8_t page = address / PAGE_SIZE;
    const int8_t slot = load_page(page);
    if (slot == NO_SLOT) return;
    cache_data[(uint32_t)slot * PAGE_SIZE + (address & (PAGE_SIZE - 1))] = value;
    slot_dirty[slot] = true;
}
