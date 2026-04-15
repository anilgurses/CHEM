/**
 * @file client.h
 * @brief TCP Client
 * @author Anıl Gürses
 * @version v1.0
 */

#pragma once

#include <string.h>

#include <boost/asio.hpp>
#include <boost/asio/io_service.hpp>
#include <memory>

#include "../common.h"

using boost::asio::io_service;
using boost::asio::ip::tcp;

namespace chem {
class Client {
   public:
    Client(const std::string endpoint, const unsigned short port);
    ~Client();

    /**
     * @brief Establishing connection to server
     */
    bool Connect();

    /**
     * @brief Disconnects from server
     */
    void Disconnect();

    /**
     * @brief Receive data from server
     */
    std::unique_ptr<char[]> Recv(size_t& _r_size);

    /**
     * @brief Close connection
     */
    void Close();

   private:
    io_service m_io_service;
    // tcp::endpoint m_endpoint;
    tcp::socket m_socket;

    std::string m_endpoint;
    unsigned short m_port;
};
}  // namespace chem
