#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <pthread.h>
#include <unistd.h>
#include <atomic>

// ANSI Terminal Colors for timestamped kernel-style logging.
#define RESET   "\033[0m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN    "\033[36m"

struct Vehicle {
    int id;
    int entry_zone;       // 0: NW, 1: NE, 2: SW, 3: SE
    int exit_zone;
    bool is_emergency;
};

struct Metrics {
    int total_vehicles_passed;
    int currently_waiting;
    int active_locks;
    int emergency_count;
};

// Four intersection quadrant resources. Every thread must acquire these mutexes in ascending index order.
pthread_mutex_t grid_zone[4];
pthread_mutex_t emergency_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t emergency_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t metrics_mutex = PTHREAD_MUTEX_INITIALIZER;

Metrics metrics = {0, 0, 0, 0};
volatile bool emergency_vehicle_present = false;
volatile bool shutdown_requested = false;

void set_active_locks(int delta) {
    pthread_mutex_lock(&metrics_mutex);
    metrics.active_locks += delta;
    if (metrics.active_locks < 0) metrics.active_locks = 0;
    pthread_mutex_unlock(&metrics_mutex);
}

void log_posix(const std::string& type, const std::string& message, const std::string& color) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    pthread_mutex_lock(&metrics_mutex);
    std::cout << color << "[" << type << "] " << message
              << " (ms:" << std::setfill('0') << std::setw(3) << ms << ")"
              << RESET << std::endl;
    pthread_mutex_unlock(&metrics_mutex);
}

void increment_metric(void (*update)(Metrics*)) {
    pthread_mutex_lock(&metrics_mutex);
    update(&metrics);
    pthread_mutex_unlock(&metrics_mutex);
}

void print_metrics() {
    pthread_mutex_lock(&metrics_mutex);
    std::cout << "\n=== FINAL OS TELEMETRY ===\n"
              << "Vehicles served: " << metrics.total_vehicles_passed << '\n'
              << "Emergency vehicles: " << metrics.emergency_count << '\n'
              << "Remaining waiting: " << metrics.currently_waiting << '\n'
              << "Active locks: " << metrics.active_locks << '\n'
              << "==========================\n";
    pthread_mutex_unlock(&metrics_mutex);
}

void* vehicle_routine(void* arg) {
    Vehicle* vehicle = static_cast<Vehicle*>(arg);

    // Emergency priority gate.
    pthread_mutex_lock(&emergency_mutex);

    while (emergency_vehicle_present && !vehicle->is_emergency && !shutdown_requested) {
        pthread_mutex_lock(&metrics_mutex);
        metrics.currently_waiting++;
        pthread_mutex_unlock(&metrics_mutex);

        log_posix("PTHREAD_COND_WAIT",
                  "Regular vehicle #" + std::to_string(vehicle->id) +
                  " paused by emergency corridor",
                  YELLOW);

        pthread_cond_wait(&emergency_cond, &emergency_mutex);

        pthread_mutex_lock(&metrics_mutex);
        metrics.currently_waiting--;
        pthread_mutex_unlock(&metrics_mutex);
    }

    if (vehicle->is_emergency) {
        emergency_vehicle_present = true;
        pthread_mutex_lock(&metrics_mutex);
        metrics.emergency_count++;
        pthread_mutex_unlock(&metrics_mutex);
        log_posix("PREEMPT", "AMBULANCE #" + std::to_string(vehicle->id) +
                  " established priority transit corridor", RED);
    }
    pthread_mutex_unlock(&emergency_mutex);

    // Strict ascending quadrant hierarchy prevents circular wait.
    const int low_zone = std::min(vehicle->entry_zone, vehicle->exit_zone);
    const int high_zone = std::max(vehicle->entry_zone, vehicle->exit_zone);

    log_posix("PTHREAD_MUTEX_LOCK",
              "Vehicle #" + std::to_string(vehicle->id) +
              " requests Q" + std::to_string(low_zone) + " -> Q" + std::to_string(high_zone),
              CYAN);

    if (low_zone == high_zone) {
        pthread_mutex_lock(&grid_zone[low_zone]);
        set_active_locks(1);

        log_posix("CRITICAL_SECTION", "Vehicle #" + std::to_string(vehicle->id) +
                  " crossing Q" + std::to_string(low_zone),
                  vehicle->is_emergency ? RED : GREEN);
        usleep(vehicle->is_emergency ? 300000 : 600000);

        pthread_mutex_unlock(&grid_zone[low_zone]);
        set_active_locks(-1);
    } else {
        // Both resources are always acquired in ascending order: no thread can form the reverse edge.
        pthread_mutex_lock(&grid_zone[low_zone]);
        pthread_mutex_lock(&grid_zone[high_zone]);
        set_active_locks(2);

        log_posix("CRITICAL_SECTION", "Vehicle #" + std::to_string(vehicle->id) +
                  " crossing Q" + std::to_string(low_zone) + " & Q" + std::to_string(high_zone),
                  vehicle->is_emergency ? RED : GREEN);
        usleep(vehicle->is_emergency ? 300000 : 600000);

        // Release reverse order: a conventional, symmetric complementary operation.
        pthread_mutex_unlock(&grid_zone[high_zone]);
        pthread_mutex_unlock(&grid_zone[low_zone]);
        set_active_locks(-2);
    }

    pthread_mutex_lock(&metrics_mutex);
    metrics.total_vehicles_passed++;
    pthread_mutex_unlock(&metrics_mutex);

    log_posix("PTHREAD_MUTEX_UNLOCK",
              "Vehicle #" + std::to_string(vehicle->id) + " cleared intersection",
              BLUE);

    if (vehicle->is_emergency) {
        pthread_mutex_lock(&emergency_mutex);
        emergency_vehicle_present = false;
        log_posix("PTHREAD_COND_BROADCAST",
                  "Ambulance corridor cleared; regular traffic resumed",
                  MAGENTA);
        pthread_cond_broadcast(&emergency_cond);
        pthread_mutex_unlock(&emergency_mutex);
    }

    delete vehicle;
    return NULL;
}

int main() {
    std::cout << "=====================================================================\n";
    std::cout << " Multi-Threaded Traffic Light Intersection Controller (POSIX)\n";
    std::cout << "=====================================================================\n\n";

    for (int i = 0; i < 4; ++i) {
        pthread_mutex_init(&grid_zone[i], NULL);
    }

    std::vector<pthread_t> threads;
    log_posix("INIT", "pthread_create: spawning North/South/East/West traffic", CYAN);

    // North/South/East/West-style alternating quadrant routes.
    for (int i = 1; i <= 4; ++i) {
        Vehicle* vehicle = new Vehicle{i, (i - 1) % 4, i % 4, false};
        pthread_t tid;
        pthread_create(&tid, NULL, vehicle_routine, vehicle);
        threads.push_back(tid);
        usleep(150000);
    }

    usleep(200000);
    Vehicle* ambulance = new Vehicle{999, 2, 3, true};
    pthread_t emergency_tid;
    pthread_create(&emergency_tid, NULL, vehicle_routine, ambulance);
    threads.push_back(emergency_tid);

    for (int i = 5; i <= 8; ++i) {
        Vehicle* vehicle = new Vehicle{i, (i - 1) % 4, i % 4, false};
        pthread_t tid;
        pthread_create(&tid, NULL, vehicle_routine, vehicle);
        threads.push_back(tid);
        usleep(150000);
    }

    for (std::vector<pthread_t>::size_type i = 0; i < threads.size(); ++i) {
        pthread_join(threads[i], NULL);
    }

    shutdown_requested = true;
    print_metrics();

    for (int i = 0; i < 4; ++i) pthread_mutex_destroy(&grid_zone[i]);
    pthread_mutex_destroy(&emergency_mutex);
    pthread_cond_destroy(&emergency_cond);
    pthread_mutex_destroy(&metrics_mutex);

    return 0;
}