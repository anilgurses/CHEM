#pragma once

#include <sqlite3.h>
#include <string.h>

#include <mutex>
#include <thread>
#include <vector>

#include "../common.h"
#include "../nlohmann/json.hpp"
#include "../node/node_entity.h"
#include "spdlog/spdlog.h"

using json = nlohmann::json;

namespace chem {
class DBHandler {
   public:
    DBHandler(const std::string& db_path);
    ~DBHandler() = default;

    DBHandler(DBHandler& other) = delete;
    void operator=(const DBHandler&) = delete;

    static DBHandler* GetInstance(const std::string& db_path);

    static int callback(void* data, int argc, char** argv, char** azColName);

    bool CreateTables();

    bool AddNode(const Node& node);

    bool AddChannel(const double& freq, const std::string& chMode,
                    const uint32_t& srate);

    bool DeleteNode(const Node& node);

    bool ChangeChannelCoeff(const std::string& name, const double& coeff);

    bool ChangePathLoss(const double& freq, const std::string&);

    bool ChangeAntenna(const std::string& nodeName, const std::string& pattern);

    bool DeleteChannel(const double& freq);

    bool setLocations();

    json GetNodes();

    json GetChannels();

   private:
    static DBHandler* pinstance_;
    static std::mutex mutex_;

    sqlite3* m_db;
    std::string m_dbPath;
};

}  // namespace chem
