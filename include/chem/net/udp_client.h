/**
 * @file udp_client.h
 * @brief UDP Client
 * @author Anıl Gürses
 * @version v1.0
 */

#pragma once

#include <netinet/in.h>
#include <string.h>

#include <boost/asio.hpp>
#include <boost/asio/io_service.hpp>

#include "../common.h"
#include "../models/data_pool.hpp"
#include "../models/signal.hpp"

using boost::asio::io_service;
using boost::asio::ip::udp;

namespace chem {
class UDPClient {
   public:
    UDPClient(const std::string& endpoint, const unsigned short port,
              iqPoolSPtr pool);
    ~UDPClient();

    /**
     * @brief Send data to UHD
     */
    void Send(std::unique_ptr<chem::Signal> sig, const uint8_t& bytes_per_otw);

    /**
     * @brief Close connection
     */
    void Close();

   private:
    io_service m_io_service;
    udp::socket m_socket;
    udp::endpoint m_remote_endpoint;
    sockaddr_in m_dest_addr{};
    iqPoolSPtr m_iq_pool;

    char packet[MTU_SIZE];
    // Deprecated
    // std::vector<std::array<char, MTU_SIZE>> m_packets;

    std::string m_endpoint;
    unsigned short m_port;
};
}  // namespace chem
