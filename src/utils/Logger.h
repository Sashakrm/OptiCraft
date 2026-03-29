//
// Created by noktemor on 28.03.2026.
//

#pragma once

#include <iostream>
#include <string>

class Logger {
public:
    static void info(const std::string& message) {
        std::cout << "[INFO] " << message << std::endl;
    }

    static void error(const std::string& message) {
        std::cerr << "[ERROR] " << message << std::endl;
    }

    static void warning(const std::string& message) {
        std::cout << "[WARN] " << message << std::endl;
    }

    template<typename... Args>
    static void debug(Args&&... args) {
#ifdef DEBUG
        std::cout << "[DEBUG] ";
        (std::cout << ... << args) << std::endl;
#endif
    }
};

// Макросы для удобства
#define LOG_INFO(msg) Logger::info(msg)
#define LOG_ERROR(msg) Logger::error(msg)
#define LOG_WARN(msg) Logger::warning(msg)
#define LOG_DEBUG(...) Logger::debug(__VA_ARGS__)




