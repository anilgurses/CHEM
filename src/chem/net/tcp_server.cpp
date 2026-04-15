#include "chem/net/tcp_server.h"

#include <sys/socket.h>

#include <boost/asio.hpp>
#include <boost/asio/completion_condition.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/error_code.hpp>

#include "chem/common.h"

using namespace chem;
using namespace boost::asio;
using boost::asio::ip::tcp;

const std::string UNTIL = "\n";

TCPServer::TCPServer() : m_io_service(), m_acceptor(m_io_service) {}

TCPServer::~TCPServer() {}

bool TCPServer::Bind(
    const std::string endpoint, const unsigned short port,
    std::function<void(const std::string&, std::string&)> handler) {
    m_handler = std::move(handler);
    m_endpoint = tcp::endpoint(tcp::v4(), port);
    m_acceptor.open(m_endpoint.protocol());
    m_acceptor.set_option(boost::asio::socket_base::reuse_address(true));
    m_acceptor.bind(m_endpoint);
    m_acceptor.listen();

    return true;
}

void TCPServer::Unbind() {}

void TCPServer::startAccept() {
    m_acceptor.async_accept(
        [this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                // Pass a shared pointer to manage the socket's lifetime
                auto shared_socket =
                    std::make_shared<tcp::socket>(std::move(socket));
                handle_connection(shared_socket);
            }
            startAccept();
        });
}

void TCPServer::handle_connection(std::shared_ptr<tcp::socket> socket) {
    m_remote_ip = socket->remote_endpoint().address().to_string();

    auto buffer = std::make_shared<boost::asio::streambuf>();

    boost::asio::async_read_until(
        *socket, *buffer, UNTIL,
        [this, socket, buffer](boost::system::error_code ec,
                               std::size_t length) {
            if (!ec) {
                std::istream is(buffer.get());
                std::string request;
                std::getline(is, request);

                std::string response;

                if (m_handler) {
                    m_handler(request, response);
                }

                response += UNTIL;
                boost::asio::async_write(
                    *socket, boost::asio::buffer(response),
                    [response](boost::system::error_code, std::size_t) {});

                handle_connection(
                    socket);  // Continue handling messages from this client
            } else if (ec == boost::asio::error::eof) {
                // Just added this case so it doesn't print an error message
                LOG_DEBUG("COORDINATOR", "Connection closed by client.");
            } else {
                LOG_ERROR("COORDINATOR", ec.message());
            }
        });
}

void TCPServer::close() { m_acceptor.close(); }

void TCPServer::run() { m_io_service.run(); }

std::string TCPServer::getRemoteIp() const { return m_remote_ip; }
