/**
 * @file follow_me.cpp
 *
 * @brief Example that demonstrates the usage of Follow Me plugin.
 * The example registers with RCSLocationProvider for location updates
 * and sends them to the Follow Me plugin which are sent to drone. You can observe
 * drone following you. We print last location of the drone in flight mode callback.
 *
 * @author Shakthi Prashanth <shakthi.prashanth.m@intel.com>
 * @date 2018-01-03
 */

#include <chrono>
#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/action/action.h>
#include <mavsdk/plugins/follow_me/follow_me.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <iostream>
#include <memory>
#include <thread>

#include "rcs_location_provider.h"

using namespace mavsdk;
using namespace std::placeholders; // for `_1`
using namespace std::chrono; // for seconds(), milliseconds(), etc
using namespace std::this_thread; // for sleep_for()

// For coloring output
#define ERROR_CONSOLE_TEXT "\033[31m" // Turn text on console red
#define TELEMETRY_CONSOLE_TEXT "\033[34m" // Turn text on console blue
#define NORMAL_CONSOLE_TEXT "\033[0m" // Restore normal console colour

// For sanity check of position to follow
#define MAX_FOLLOW_DISTANCE_METERS 5.0

#define MAX_SECONDS_TO_FOLLOW 60

inline void action_error_exit(Action::Result result, const std::string& message);
inline void follow_me_error_exit(FollowMe::Result result, const std::string& message);
inline void connection_error_exit(ConnectionResult result, const std::string& message);

void usage(std::string bin_name)
{
    std::cout << NORMAL_CONSOLE_TEXT << "Usage : " << bin_name << " <connection_url>" << std::endl
              << "Connection URL format should be :" << std::endl
              << " For TCP : tcp://[server_host][:server_port]" << std::endl
              << " For UDP : udp://[bind_host][:bind_port]" << std::endl
              << " For Serial : serial:///path/to/serial/dev[:baudrate]" << std::endl
              << "For example, to connect to the simulator use URL: udp://:14540" << std::endl;
}

int main(int argc, char** argv)
{
    Mavsdk dc;
    std::string connection_url;
    ConnectionResult connection_result;

    if (argc == 2) {
        connection_url = argv[1];
        connection_result = dc.add_any_connection(connection_url);
    } else {
        usage(argv[0]);
        return 1;
    }

    if (connection_result != ConnectionResult::Success) {
        std::cout << ERROR_CONSOLE_TEXT << "Connection failed: " << connection_result
                  << NORMAL_CONSOLE_TEXT << std::endl;
        return 1;
    }

    std::cout << "Waiting to discover system..." << std::endl;
    bool discovered_system = false;
    dc.register_on_discover([&discovered_system](uint64_t uuid) {
        std::cout << "Discovered system with UUID: " << uuid << std::endl;
        discovered_system = true;
    });

    while (!discovered_system)
        sleep_for(seconds(1));

    if (dc.system_uuids().size() > 1) {
        std::cout << "Discovered more than one system! Exiting." << std::endl;
        exit(1);
    }

    // System got discovered.
    System& system = dc.system(dc.system_uuids().front());
    auto action = std::make_shared<Action>(system);
    auto follow_me = std::make_shared<FollowMe>(system);
    auto telemetry = std::make_shared<Telemetry>(system);

    while (!telemetry->health_all_ok()) {
        std::cout << "Waiting for system to be ready" << std::endl;
        sleep_for(seconds(1));
    }
    std::cout << "System is ready" << std::endl;

    // Arm
    if (!telemetry->armed()) {
        Action::Result arm_result = action->arm();
        action_error_exit(arm_result, "Arming failed");
    }
    std::cout << "Armed" << std::endl;

    // Takeoff
    if (!telemetry->in_air()) {
        Action::Result takeoff_result = action->takeoff();
        action_error_exit(takeoff_result, "Takeoff failed");
        while (telemetry->position().relative_altitude_m < 2.4)
            sleep_for(seconds(1)); // Wait for drone to reach takeoff altitude
    }
    std::cout << "In Air..." << std::endl;

    FollowMe::Config config;
    config.min_height_m = 8.0;
    config.follow_direction = FollowMe::Config::FollowDirection::Front;
    config.follow_distance_m = 1.0;
    FollowMe::Result follow_me_result = follow_me->set_config(config);

    // Start Follow Me
    follow_me_result = follow_me->start();
    follow_me_error_exit(follow_me_result, "Failed to start FollowMe mode");

    // Subscribe to receive updates on flight mode. You can find out whether FollowMe is active.
    telemetry->subscribe_flight_mode(std::bind(
        [&](Telemetry::FlightMode flight_mode) {
            if (flight_mode != Telemetry::FlightMode::FollowMe) {
                std::cout << "Flight mode was changed externally! Exiting.\n";
                exit(0);
            }

            const FollowMe::TargetLocation last_location = follow_me->get_last_location();
            std::cout << "[FlightMode: " << flight_mode
                      << "] Vehicle is at: " << last_location.latitude_deg << ", "
                      << last_location.longitude_deg << " degrees." << std::endl;
        },
        std::placeholders::_1));

    RCSLocationProvider location_provider;
    location_provider.request_location_updates("localhost", 65191, 0, [&follow_me, &telemetry](double lat, double lon) {
        FollowMe::TargetLocation target_location{};
        target_location.latitude_deg = lat;
        target_location.longitude_deg = lon;

        double lat_diff = fabs(telemetry->position().latitude_deg - target_location.latitude_deg);
        double lon_diff = fabs(telemetry->position().longitude_deg - target_location.longitude_deg);

        // Sanity-check: do not follow if target is more than (roughly) MAX_FOLLOW_DISTANCE_METERS away in either x or y
        if (lat_diff/RCSLocationProvider::LATITUDE_DEG_PER_METER < MAX_FOLLOW_DISTANCE_METERS
                && lon_diff/RCSLocationProvider::LONGITUDE_DEG_PER_METER < MAX_FOLLOW_DISTANCE_METERS)
            follow_me->set_target_location(target_location);
        else
            std::cout << "Warning: skipped position " << target_location.latitude_deg << ", " << target_location.longitude_deg << std::endl;
    });

    while (location_provider.is_running()) {
        static int count = 0;

        sleep_for(seconds(1));
        if (++count >= MAX_SECONDS_TO_FOLLOW)
            break;
    }

    // Stop Follow Me
    follow_me_result = follow_me->stop();
    follow_me_error_exit(follow_me_result, "Failed to stop FollowMe mode");

    // Stop flight mode updates.
    telemetry->subscribe_flight_mode(nullptr);

    // Land
    const Action::Result land_result = action->land();
    action_error_exit(land_result, "Landing failed");
    while (telemetry->in_air()) {
        std::cout << "waiting until landed" << std::endl;
        sleep_for(seconds(1));
    }
    std::cout << "Landed..." << std::endl;
    return 0;
}

// Handles Action's result
inline void action_error_exit(Action::Result result, const std::string& message)
{
    if (result != Action::Result::Success) {
        std::cerr << ERROR_CONSOLE_TEXT << message << result << NORMAL_CONSOLE_TEXT << std::endl;
        exit(EXIT_FAILURE);
    }
}
// Handles FollowMe's result
inline void follow_me_error_exit(FollowMe::Result result, const std::string& message)
{
    if (result != FollowMe::Result::Success) {
        std::cerr << ERROR_CONSOLE_TEXT << message << result << NORMAL_CONSOLE_TEXT << std::endl;
        exit(EXIT_FAILURE);
    }
}
// Handles connection result
inline void connection_error_exit(ConnectionResult result, const std::string& message)
{
    if (result != ConnectionResult::Success) {
        std::cerr << ERROR_CONSOLE_TEXT << message << result << NORMAL_CONSOLE_TEXT << std::endl;
        exit(EXIT_FAILURE);
    }
}
