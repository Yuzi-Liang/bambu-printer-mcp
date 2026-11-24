#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

// Bambu Studio v02.09.01.70, src/slic3r/Utils/bambu_networking.hpp.
struct TaskQueryParams {
    std::string dev_id;
    int status = 0;
    int offset = 0;
    int limit = 20;
};

template <typename Function>
Function load_symbol(HMODULE module, const char* name) {
    const auto symbol = GetProcAddress(module, name);
    if (!symbol) {
        throw std::runtime_error(std::string("Bambu networking plug-in is missing ") + name);
    }
    return reinterpret_cast<Function>(symbol);
}

void check(int code, const char* operation) {
    if (code != 0) {
        throw std::runtime_error(std::string(operation) + " failed with code " + std::to_string(code));
    }
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 7) {
        std::cerr << "usage: bambu-print-history PLUGIN PROFILE CERTIFICATES REGION OFFSET LIMIT\n";
        return 2;
    }

    try {
        const std::filesystem::path plugin(argv[1]);
        const auto profile = std::filesystem::path(argv[2]).u8string();
        const auto certificates = std::filesystem::path(argv[3]).u8string();
        const auto region = std::filesystem::path(argv[4]).u8string();
        TaskQueryParams query;
        query.offset = std::stoi(argv[5]);
        query.limit = std::stoi(argv[6]);
        if (!plugin.is_absolute() || query.offset < 0 || query.limit < 1 || query.limit > 50) {
            throw std::runtime_error("Invalid plug-in path or history page");
        }

        const auto module = LoadLibraryExW(plugin.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            throw std::runtime_error("Could not load the Bambu networking plug-in (Windows error " +
                std::to_string(GetLastError()) + ")");
        }

        using AgentFunction = int (*)(void*);
        using AgentStringFunction = int (*)(void*, std::string);
        const auto version = load_symbol<std::string (*)()>(module, "bambu_network_get_version")();
        if (version.rfind("02.09.", 0) != 0) {
            throw std::runtime_error("This history helper supports the Bambu networking plug-in 02.09 family");
        }

        const auto create = load_symbol<void* (*)(std::string)>(module, "bambu_network_create_agent");
        const auto destroy = load_symbol<AgentFunction>(module, "bambu_network_destroy_agent");
        const auto tasks = load_symbol<int (*)(void*, TaskQueryParams, std::string*)>(module,
            "bambu_network_get_user_tasks");

        const std::unique_ptr<void, AgentFunction> agent(create(profile), destroy);
        if (!agent) {
            throw std::runtime_error("Could not create the Bambu networking agent");
        }
        const auto set_config = load_symbol<AgentStringFunction>(module, "bambu_network_set_config_dir");
        const auto init_log = load_symbol<AgentFunction>(module, "bambu_network_init_log");
        const auto set_cert = load_symbol<int (*)(void*, std::string, std::string)>(module,
            "bambu_network_set_cert_file");
        const auto set_country = load_symbol<AgentStringFunction>(module, "bambu_network_set_country_code");
        const auto start = load_symbol<AgentFunction>(module, "bambu_network_start");
        const auto logged_in = load_symbol<bool (*)(void*)>(module, "bambu_network_is_user_login");

        check(set_config(agent.get(), profile), "Setting profile directory");
        check(init_log(agent.get()), "Initializing plug-in log");
        check(set_cert(agent.get(), certificates, "slicer_base64.cer"), "Setting plug-in certificates");
        check(set_country(agent.get(), region), "Setting Bambu account region");
        check(start(agent.get()), "Starting Bambu networking agent");
        if (!logged_in(agent.get())) {
            return 3;
        }

        std::string response;
        check(tasks(agent.get(), query, &response), "Reading Bambu cloud print history");
        std::cout << response << std::flush;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
