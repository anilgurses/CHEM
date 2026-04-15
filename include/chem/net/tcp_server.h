/**
 * @file tcp_server.h
 * @brief TCP Server
 * @author Anıl Gürses
 * @version v1.0
 */

#pragma once

#include <array>
#include <boost/asio.hpp>
#include <boost/asio/io_service.hpp>
#include <functional>
#include <memory>
#include <string>

using boost::asio::io_service;
using boost::asio::ip::tcp;

namespace chem {
class TCPServer {
   public:
    TCPServer();
    ~TCPServer();

    /**
     * @brief Binding the port
     */
    bool Bind(const std::string endpoint, const unsigned short port,
              std::function<void(const std::string&, std::string&)> handler);

    /**
     * @brief Disconnects from server
     */
    void Unbind();

    /**
     * @brief Receive handler for async TCP
     */
    void startAccept();

    /**
     * @brief Handle connection
     */
    void handle_connection(std::shared_ptr<tcp::socket> socket);

    void close();

    void run();

    std::string getRemoteIp() const;

   private:
    io_service m_io_service;
    tcp::endpoint m_endpoint;
    tcp::acceptor m_acceptor;
    std::string m_remote_ip;

    std::function<void(const std::string&, std::string&)> m_handler;
    unsigned short m_port;
};
}  // namespace chem
