#pragma once

#include <string.h>

#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/spdlog.h"

constexpr auto max_size = 1048576 * 3;
constexpr auto max_files = 3;

namespace chem {

class Logger {
   public:
    Logger(const std::string& fpath) {
        sinks.reserve(2);
        sinks.emplace_back(
            std::make_shared<spdlog::sinks::ansicolor_stdout_sink_st>());
        sinks.emplace_back(
            std::make_shared<spdlog::sinks::rotating_file_sink_st>(
                fpath, max_size, max_files));

        std::shared_ptr<spdlog::logger> logger =
            std::make_shared<spdlog::logger>("CHEM", begin(sinks), end(sinks));
        logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
        spdlog::register_logger(logger);
    };

    ~Logger() = default;

   private:
    std::vector<spdlog::sink_ptr> sinks;
};
}  // namespace chem
