#include "chem/db/dbHandler.h"

#include <spdlog/fmt/fmt.h>

#include <boost/lexical_cast.hpp>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "chem/db/dbModels.hpp"

using namespace chem;

DBHandler* DBHandler::pinstance_{nullptr};
std::mutex DBHandler::mutex_;

std::string quotesql(const std::string& s) {
    return std::string("'") + s + std::string("'");
}

DBHandler::DBHandler(const std::string& db_path) : m_dbPath(db_path) {
    int rc = sqlite3_open(db_path.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        LOG_ERROR("DB", "Error while opening the db!");
    } else {
        LOG_INFO("DB", "Opened successfuly!");
    }
}

DBHandler* DBHandler::GetInstance(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pinstance_ == nullptr) {
        pinstance_ = new DBHandler(db_path);
    }
    return pinstance_;
}

int DBHandler::callback(void* data, int argc, char** argv, char** azColName) {
    if (argc == 0) return 0;
    return 0;
}

bool DBHandler::CreateTables() {
    std::lock_guard<std::mutex> lock(mutex_);
    char* zErrMsg = 0;
    int rc;
    // TODO add option to config.json for saving params
    auto drp_nodes = "DROP TABLE IF EXISTS nodes";
    auto drp_chnls = "DROP TABLE IF EXISTS channels";

    rc = sqlite3_exec(m_db, drp_nodes, callback, nullptr, &zErrMsg);
    rc = sqlite3_exec(m_db, drp_chnls, callback, nullptr, &zErrMsg);

    auto crt_nodes =
        "CREATE TABLE nodes (id TEXT PRIMARY KEY, name TEXT, txFreq DOUBLE, "
        "rxFreq "
        "DOUBLE, numChannels INT, txsampleRate DOUBLE, rxsampleRate DOUBLE, "
        "chCoeff DOUBLE, vehicleAddress TEXT, antennaPattern TEXT)";
    auto crt_chnls =
        "CREATE TABLE channels (freq DOUBLE PRIMARY KEY, pathLoss TEXT, sRate "
        "DOUBLE)";
    rc = sqlite3_exec(m_db, crt_nodes, callback, nullptr, &zErrMsg);
    rc = sqlite3_exec(m_db, crt_chnls, callback, nullptr, &zErrMsg);

    if (rc == SQLITE_OK)
        return true;
    else
        return false;
}

bool DBHandler::AddNode(const Node& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    char* zErrMsg = 0;
    int rc;

    const auto& cfg = node.getConfig();
    const std::string default_antenna = "isotropic";
    std::string ins_q =
        "INSERT INTO nodes (id, name, txFreq, rxFreq, numChannels, "
        "txsampleRate, rxsampleRate, "
        "chCoeff, vehicleAddress, antennaPattern) VALUES (" +
        quotesql(cfg.getId()) + "," + quotesql(cfg.getName()) + "," +
        std::to_string(cfg.getChannels().at(0).getTxFreq() / 1000000) + "," +
        std::to_string(cfg.getChannels().at(0).getRxFreq() / 1000000) + "," +
        std::to_string(cfg.getNumChannels()) + "," +
        std::to_string(cfg.getSampleRate().getTxRate()) + "," +
        std::to_string(cfg.getSampleRate().getRxRate()) + "," + "1.0," +
        quotesql(cfg.getVehicleAddress()) + "," + quotesql(default_antenna) +
        ") "
        "ON CONFLICT(id) DO UPDATE SET "
        "name=excluded.name,"
        "txFreq=excluded.txFreq,"
        "rxFreq=excluded.rxFreq,"
        "numChannels=excluded.numChannels,"
        "txsampleRate=excluded.txsampleRate,"
        "rxsampleRate=excluded.rxsampleRate,"
        "vehicleAddress=excluded.vehicleAddress;";

    rc = sqlite3_exec(m_db, ins_q.c_str(), callback, nullptr, &zErrMsg);

    if (rc == SQLITE_OK)
        return true;
    else
        return false;
}

bool DBHandler::DeleteNode(const Node& node) {
    std::lock_guard<std::mutex> lock(mutex_);

    char* zErrMsg = 0;
    int rc;

    std::string dlt_q =
        "DELETE FROM nodes WHERE id=" + quotesql(node.getConfig().getId());

    rc = sqlite3_exec(m_db, dlt_q.c_str(), callback, nullptr, &zErrMsg);

    if (rc == SQLITE_OK)
        return true;
    else
        return false;
}

bool DBHandler::DeleteChannel(const double& freq) {
    std::lock_guard<std::mutex> lock(mutex_);

    char* zErrMsg = 0;
    int rc;

    std::string dlt_q =
        "DELETE FROM channels WHERE freq=" + std::to_string(freq / 1000000);

    rc = sqlite3_exec(m_db, dlt_q.c_str(), callback, nullptr, &zErrMsg);

    if (rc == SQLITE_OK)
        return true;
    else
        return false;
}

bool DBHandler::AddChannel(const double& freq, const std::string& chMode,
                           const uint32_t& srate) {
    char* zErrMsg = 0;
    int rc;

    std::string add_q = "INSERT INTO channels VALUES (" +
                        std::to_string(freq / 1000000) + "," +
                        quotesql(chMode) + "," + std::to_string(srate) + ");";

    rc = sqlite3_exec(m_db, add_q.c_str(), callback, nullptr, &zErrMsg);

    if (rc == SQLITE_OK)
        return true;
    else
        return false;
}

json DBHandler::GetNodes() {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt;
    json j;

    std::string slc_q = "SELECT * FROM nodes";
    int rc = sqlite3_prepare_v2(m_db, slc_q.c_str(), -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        return j;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        struct db::Node n;
        n.specs.id =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        n.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        n.specs.txFreq = sqlite3_column_double(stmt, 2);
        n.specs.rxFreq = sqlite3_column_double(stmt, 3);
        n.specs.numChannels = sqlite3_column_int(stmt, 4);
        n.specs.txsampleRate = sqlite3_column_double(stmt, 5);
        n.specs.rxsampleRate = sqlite3_column_double(stmt, 6);
        n.specs.chCoeff = sqlite3_column_double(stmt, 7);
        n.specs.vehicleAddress =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        n.specs.antennaPattern =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        j[n.name] = n.specs;
    }

    if (rc != SQLITE_DONE) {
        LOG_WARN("DB", "Get Nodes query error!");
    }
    sqlite3_finalize(stmt);

    return j;
}

bool DBHandler::ChangeAntenna(const std::string& nodeName,
                              const std::string& pattern) {
    std::lock_guard<std::mutex> lock(mutex_);
    char* zErrMsg = 0;
    int rc;

    std::string upd_q = "UPDATE nodes SET antennaPattern=" + quotesql(pattern) +
                        " WHERE name=" + quotesql(nodeName);

    rc = sqlite3_exec(m_db, upd_q.c_str(), callback, nullptr, &zErrMsg);
    return rc == SQLITE_OK;
}

json DBHandler::GetChannels() {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt;
    json j;

    std::string slc_q = "SELECT * FROM channels";
    int rc = sqlite3_prepare_v2(m_db, slc_q.c_str(), -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        return j;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        struct db::Channel c;
        c.freq = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        c.specs.pathLoss =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        c.specs.sRate = sqlite3_column_double(stmt, 2);
        j[c.freq] = c.specs;
    }

    if (rc != SQLITE_DONE) {
        LOG_WARN("DB", "Get Channels query error!");
    }
    sqlite3_finalize(stmt);

    return j;
}
