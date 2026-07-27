#pragma once

#include <string.h>

#include <memory>

#include "spdlog/sinks/ringbuffer_sink.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/spdlog.h"

constexpr auto max_size = 1048576 * 3;
constexpr auto max_files = 3;
constexpr size_t log_ringbuffer_capacity = 2048;

namespace chem {

class Logger {
   public:
    Logger(const std::string& fpath) {
        sinks.reserve(3);
        sinks.emplace_back(
            std::make_shared<spdlog::sinks::ansicolor_stdout_sink_st>());
        sinks.emplace_back(
            std::make_shared<spdlog::sinks::rotating_file_sink_st>(
                fpath, max_size, max_files));
        ringBuffer = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(
            log_ringbuffer_capacity);
        sinks.emplace_back(ringBuffer);

        std::shared_ptr<spdlog::logger> logger =
            std::make_shared<spdlog::logger>("CHEM", begin(sinks), end(sinks));
        logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
        spdlog::register_logger(logger);
    };

    ~Logger() = default;

    static std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> getRingBuffer() {
        return ringBuffer;
    }

   private:
    std::vector<spdlog::sink_ptr> sinks;
    inline static std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt>
        ringBuffer = nullptr;
};
}  // namespace chem
