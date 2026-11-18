#ifdef _WIN32
#define _WIN32_WINNT 0x0600
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

// ANSI Terminal Colors for timestamped kernel-style logging.
#define RESET   "\033[0m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN    "\033[36m"

using namespace std::chrono;

// ---------------------------------------------------------------------------
// Platform abstraction: mutex / condition variable / thread / sleep
// ---------------------------------------------------------------------------
#ifdef _WIN32
typedef CRITICAL_SECTION mtx_t;
typedef CONDITION_VARIABLE cnd_t;
typedef HANDLE thread_t;
#define mtx_init(m) InitializeCriticalSection(m)
#define mtx_lock(m) EnterCriticalSection(m)
#define mtx_unlock(m) LeaveCriticalSection(m)
#define mtx_destroy(m) DeleteCriticalSection(m)
#define cnd_init(c) InitializeConditionVariable(c)
#define cnd_wait(c, m) SleepConditionVariableCS(c, m, INFINITE)
#define cnd_wakeall(c) WakeAllConditionVariable(c)
#define thread_create(t, proc, arg) ((t) = CreateThread(NULL, 0, proc, arg, 0, NULL))
#define thread_join(t) WaitForSingleObject(t, INFINITE)
#define thread_close(t) CloseHandle(t)
#define thread_sleep_ms(m) Sleep(m)
#define thread_sleep_us(m) Sleep(((m) + 999) / 1000)
#else
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t cnd_t;
typedef pthread_t thread_t;
#define mtx_init(m) pthread_mutex_init(m, NULL)
#define mtx_lock(m) pthread_mutex_lock(m)
#define mtx_unlock(m) pthread_mutex_unlock(m)
#define mtx_destroy(m) pthread_mutex_destroy(m)
#define cnd_init(c) pthread_cond_init(c, NULL)
#define cnd_wait(c, m) pthread_cond_wait(c, m)
#define cnd_wakeall(c) pthread_cond_broadcast(c)
#define thread_create(t, proc, arg) pthread_create(&(t), NULL, proc, arg)
#define thread_join(t) pthread_join(t, NULL)
#define thread_close(t)
#define thread_sleep_ms(m) usleep((m) * 1000)
#define thread_sleep_us(m) usleep(m)
#endif

struct Vehicle {
    int id;
    int entry_zone;       // 0: NW, 1: NE, 2: SW, 3: SE
    int exit_zone;
    int group;            // 0: NS, 1: EW
    bool is_emergency;
};

struct Metrics {
    int total_vehicles_passed;
    int currently_waiting;
    int active_locks;
    int emergency_count;
};

// Four intersection quadrant resources. Ascending index lock order prevents deadlock.
mtx_t grid_zone_mtx[4];
// Traffic light control
mtx_t signal_mtx;
cnd_t signal_cv;
mtx_t metrics_mtx;

Metrics metrics = {0, 0, 0, 0};
volatile bool shutdown_requested = false;

// Signal state
int active_group = 0;           // 0: NS, 1: EW
enum { SIG_RED, SIG_GREEN, SIG_YELLOW, SIG_ALLRED };
int signal_state = SIG_RED;
int queue_count[2] = {0, 0};
bool emergency_preempt = false;
int emergency_group = -1;

// Timing (ms)
const long MIN_GREEN_MS = 3000;
const long MAX_GREEN_MS = 10000;
const long YELLOW_MS = 1500;
const long ALL_RED_MS = 800;
const long EMPTY_SWITCH_MS = 2000;

void set_active_locks(int delta) {
    mtx_lock(&metrics_mtx);
    metrics.active_locks += delta;
    if (metrics.active_locks < 0) metrics.active_locks = 0;
    mtx_unlock(&metrics_mtx);
}

void log_posix(const std::string& type, const std::string& message, const std::string& color) {
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    mtx_lock(&metrics_mtx);
    std::cout << color << "[" << type << "] " << message
              << " (ms:" << std::setfill('0') << std::setw(3) << ms << ")"
              << RESET << std::endl;
    mtx_unlock(&metrics_mtx);
}

void print_metrics() {
    mtx_lock(&metrics_mtx);
    std::cout << "\n=== FINAL OS TELEMETRY ===\n"
              << "Vehicles served: " << metrics.total_vehicles_passed << '\n'
              << "Emergency vehicles: " << metrics.emergency_count << '\n'
              << "Remaining waiting: " << metrics.currently_waiting << '\n'
              << "Active locks: " << metrics.active_locks << '\n'
              << "==========================\n";
    mtx_unlock(&metrics_mtx);
}

const char* group_name(int g) { return g == 0 ? "NS" : "EW"; }

long adaptive_green_ms(int own, int other) {
    long total = own + other;
    if (total <= 0) return MIN_GREEN_MS;
    long ms = MIN_GREEN_MS + ((long)own * (MAX_GREEN_MS - MIN_GREEN_MS) / total);
    if (ms < MIN_GREEN_MS) ms = MIN_GREEN_MS;
    if (ms > MAX_GREEN_MS) ms = MAX_GREEN_MS;
    return ms;
}

// Shared controller body (platform-agnostic).
void controller_body() {
    steady_clock::time_point phase_start = steady_clock::now();

    while (!shutdown_requested) {
        bool idle = false;
        long elapsed = 0;
        mtx_lock(&signal_mtx);
        {
            auto now = steady_clock::now();
            elapsed = duration_cast<milliseconds>(now - phase_start).count();

            // Both sides empty: idle until a vehicle arrives.
            if (queue_count[0] == 0 && queue_count[1] == 0) {
                idle = true;
            } else if (signal_state == SIG_GREEN && emergency_preempt && emergency_group != -1 &&
                       emergency_group != active_group) {
                // Emergency preemption: abort green and force ambulance group green.
                signal_state = SIG_YELLOW;
                phase_start = steady_clock::now();
                cnd_wakeall(&signal_cv);
            }
        }
        mtx_unlock(&signal_mtx);

        if (idle) {
            thread_sleep_ms(200);
            continue;
        }

        mtx_lock(&signal_mtx);
        auto now = steady_clock::now();
        elapsed = duration_cast<milliseconds>(now - phase_start).count();

        switch (signal_state) {
            case SIG_YELLOW:
                if (elapsed >= YELLOW_MS) {
                    signal_state = SIG_ALLRED;
                    phase_start = steady_clock::now();
                    cnd_wakeall(&signal_cv);
                }
                break;

            case SIG_ALLRED:
                if (elapsed >= ALL_RED_MS) {
                    // Choose next group: ambulance group if preempting, else the waiting side.
                    int next = active_group;
                    if (emergency_preempt && emergency_group != -1) {
                        next = emergency_group;
                        emergency_preempt = false;
                        emergency_group = -1;
                        log_posix("PREEMPT", "AMBULANCE priority: " + std::string(group_name(next)) + " forced GREEN", MAGENTA);
                    } else {
                        int other = 1 - active_group;
                        if (queue_count[active_group] == 0 && queue_count[other] > 0) {
                            next = other; // active side empty, other side waiting
                        }
                    }
                    active_group = next;
                    long green_ms = adaptive_green_ms(queue_count[active_group], queue_count[1 - active_group]);
                    signal_state = SIG_GREEN;
                    phase_start = steady_clock::now();
                    log_posix("PHASE", std::string(group_name(active_group)) + " GREEN for " +
                              std::to_string(green_ms) + "ms | queued NS=" +
                              std::to_string(queue_count[0]) + " EW=" + std::to_string(queue_count[1]), CYAN);
                    cnd_wakeall(&signal_cv);
                }
                break;

            case SIG_GREEN: {
                long green_ms = adaptive_green_ms(queue_count[active_group], queue_count[1 - active_group]);
                int own_q = queue_count[active_group];
                int other_q = queue_count[1 - active_group];
                bool heavy_other = (other_q >= own_q + 2 && other_q >= 3);
                bool should_switch = false;

                if (emergency_preempt && emergency_group != -1 && emergency_group != active_group) {
                    should_switch = true; // handled by preemption branch above
                } else if (own_q == 0 && other_q > 0 && elapsed >= EMPTY_SWITCH_MS) {
                    should_switch = true; // active side empty, other side waiting
                } else if (heavy_other && elapsed >= MIN_GREEN_MS) {
                    should_switch = true; // opposing side heavier
                } else if (elapsed >= green_ms) {
                    should_switch = true; // green time expired
                }

                if (should_switch) {
                    signal_state = SIG_YELLOW;
                    phase_start = steady_clock::now();
                    cnd_wakeall(&signal_cv);
                }
                break;
            }

            case SIG_RED:
                // Should not linger; move to yellow/allred to pick next group.
                signal_state = SIG_YELLOW;
                phase_start = steady_clock::now();
                cnd_wakeall(&signal_cv);
                break;
        }

        mtx_unlock(&signal_mtx);
        thread_sleep_ms(100);
    }
}

void vehicle_body(Vehicle* v) {
    int g = v->group;

    // Enter queue.
    mtx_lock(&signal_mtx);
    queue_count[g]++;
    mtx_lock(&metrics_mtx);
    metrics.currently_waiting++;
    mtx_unlock(&metrics_mtx);
    log_posix("QUEUE", "Vehicle #" + std::to_string(v->id) + " queued at " + std::string(group_name(g)), CYAN);

    // Ambulance requests immediate priority corridor.
    if (v->is_emergency) {
        emergency_preempt = true;
        emergency_group = g;
        mtx_lock(&metrics_mtx);
        metrics.emergency_count++;
        mtx_unlock(&metrics_mtx);
        log_posix("PREEMPT", "AMBULANCE #" + std::to_string(v->id) + " requests priority corridor on " + std::string(group_name(g)), RED);
        cnd_wakeall(&signal_cv);
    }

    // Wait until this group has green.
    while (signal_state != SIG_GREEN || active_group != g) {
        cnd_wait(&signal_cv, &signal_mtx);
    }

    // Green: leave queue and enter intersection.
    queue_count[g]--;
    mtx_lock(&metrics_mtx);
    metrics.currently_waiting--;
    mtx_unlock(&metrics_mtx);
    log_posix("RELEASE", "Vehicle #" + std::to_string(v->id) + " enters intersection on " + std::string(group_name(g)), GREEN);
    mtx_unlock(&signal_mtx);

    // Cross intersection using ascending quadrant lock order (deadlock-free).
    const int low_zone = std::min(v->entry_zone, v->exit_zone);
    const int high_zone = std::max(v->entry_zone, v->exit_zone);

    long cross_ms = v->is_emergency ? 300 : 600;
    if (low_zone == high_zone) {
        mtx_lock(&grid_zone_mtx[low_zone]);
        set_active_locks(1);
        log_posix("CROSS", "Vehicle #" + std::to_string(v->id) + " crossing Q" + std::to_string(low_zone),
                  v->is_emergency ? RED : GREEN);
        thread_sleep_ms(cross_ms);
        mtx_unlock(&grid_zone_mtx[low_zone]);
        set_active_locks(-1);
    } else {
        mtx_lock(&grid_zone_mtx[low_zone]);
        mtx_lock(&grid_zone_mtx[high_zone]);
        set_active_locks(2);
        log_posix("CROSS", "Vehicle #" + std::to_string(v->id) + " crossing Q" + std::to_string(low_zone) + " & Q" + std::to_string(high_zone),
                  v->is_emergency ? RED : GREEN);
        thread_sleep_ms(cross_ms);
        mtx_unlock(&grid_zone_mtx[high_zone]);
        mtx_unlock(&grid_zone_mtx[low_zone]);
        set_active_locks(-2);
    }

    mtx_lock(&metrics_mtx);
    metrics.total_vehicles_passed++;
    mtx_unlock(&metrics_mtx);
    log_posix("CLEAR", "Vehicle #" + std::to_string(v->id) + " cleared intersection", BLUE);

    // Notify controller the junction is free.
    mtx_lock(&signal_mtx);
    cnd_wakeall(&signal_cv);
    mtx_unlock(&signal_mtx);

    delete v;
}

#ifdef _WIN32
DWORD WINAPI controller_proc(LPVOID) { controller_body(); return 0; }
DWORD WINAPI vehicle_proc(LPVOID p) { vehicle_body(static_cast<Vehicle*>(p)); return 0; }
#else
void* controller_proc(void*) { controller_body(); return NULL; }
void* vehicle_proc(void* p) { vehicle_body(static_cast<Vehicle*>(p)); return NULL; }
#endif

int main() {
    std::cout << "=====================================================================\n";
    std::cout << " Multi-Threaded Traffic Light Intersection Controller (Adaptive)\n";
    std::cout << "=====================================================================\n\n";

    // Initialize sync objects before any logging (logging locks metrics_mtx).
    for (int i = 0; i < 4; ++i) mtx_init(&grid_zone_mtx[i]);
    mtx_init(&signal_mtx);
    cnd_init(&signal_cv);
    mtx_init(&metrics_mtx);

    log_posix("INIT", "Adaptive signal controller started (NS <-> EW phases)", CYAN);

    thread_t controller;
    thread_create(controller, controller_proc, NULL);

    std::vector<thread_t> threads;
    auto spawn_vehicle = [&](int id, int group, bool emergency) {
        int approach = (group == 0) ? (id % 2 == 0 ? 0 : 2) : (id % 2 == 0 ? 1 : 3);
        int entry_zone = approach;
        int exit_zone = (approach + 2) % 4;
        Vehicle* v = new Vehicle{id, entry_zone, exit_zone, group, emergency};
        thread_t h;
        thread_create(h, vehicle_proc, v);
        threads.push_back(h);
    };

    // Burst 1: heavy NS traffic -> NS gets a longer adaptive green.
    for (int i = 1; i <= 6; ++i) {
        spawn_vehicle(i, 0, false);
        thread_sleep_ms(250);
    }

    // Ambulance on EW (lighter side): preempts and passes first.
    thread_sleep_ms(800);
    spawn_vehicle(999, 1, true);

    // Burst 2: heavy EW traffic -> EW gets a longer adaptive green.
    for (int i = 7; i <= 12; ++i) {
        spawn_vehicle(i, 1, false);
        thread_sleep_ms(250);
    }

    // Burst 3: more NS traffic.
    for (int i = 13; i <= 16; ++i) {
        spawn_vehicle(i, 0, false);
        thread_sleep_ms(250);
    }

    for (size_t i = 0; i < threads.size(); ++i) {
        thread_join(threads[i]);
        thread_close(threads[i]);
    }

    shutdown_requested = true;
    thread_join(controller);
    thread_close(controller);

    print_metrics();

    for (int i = 0; i < 4; ++i) mtx_destroy(&grid_zone_mtx[i]);
    mtx_destroy(&signal_mtx);
    mtx_destroy(&metrics_mtx);

    return 0;
}
