/**
 * @file udp_server.h
 * @brief UDP Server
 * @author Anıl Gürses
 * @version v1.0
 */

#pragma once

#include <string.h>

#include <array>
#include <boost/asio.hpp>
#include <boost/asio/io_service.hpp>
#include <memory>

#include "../common.h"
#include "../models/data_pool.hpp"

using boost::asio::io_service;
using boost::asio::ip::udp;

namespace chem {
class UDPServer {
   public:
    UDPServer(const std::string& endpoint, unsigned short port,
              iqPoolSPtr pool);
    ~UDPServer();

    /*
     * @brief Receive data to UHD
     */
    dArray_uptr Recv(struct Header& hdr);

    /*
     * @brief Close connection
     */
    void Close();

   private:
    io_service m_io_service;
    udp::socket m_socket;

    iqPoolSPtr m_pool;
    udpDataPool_t m_udpPool;
    udp::endpoint m_endpoint;

    // Dynamic batch size management
    int m_current_batch_size = BATCH_SIZE;
};
}  // namespace chem
