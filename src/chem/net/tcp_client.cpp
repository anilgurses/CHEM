#include "chem/net/tcp_client.h"

#include <sys/socket.h>
#include <unistd.h>

#include <boost/asio.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstring>
#include <memory>

#define HEADER_LENGTH 8

using namespace chem;
using namespace boost::asio;

const char request[5] = "0xFF";

Client::Client(const std::string endpoint, const unsigned short port)
    : m_endpoint(endpoint), m_port(port), m_socket(m_io_service) {}

Client::~Client() { m_socket.close(); }

bool Client::Connect() {
    tcp::resolver resolver(m_io_service);
    boost::system::error_code err = boost::asio::error::host_not_found;
    tcp::endpoint ep(boost::asio::ip::address::from_string(m_endpoint), m_port);
    m_socket.open(tcp::v4());

    m_socket.connect(ep, err);

    m_socket.set_option(boost::asio::ip::tcp::no_delay(true));
    m_socket.set_option(boost::asio::socket_base::send_buffer_size(2048));
    // NOTE in the future, change it to something else
    // m_socket.set_option(boost::asio::socket_base::receive_buffer_size(700000));

    if (!err)
        return true;
    else {
        return false;
    }
}

void Client::Disconnect() {}

/**
 * @brief Recv stream from UHD-TX and send reply immediatly
 */
std::unique_ptr<char[]> Client::Recv(size_t& _r_size) {
    char req[8];

    // auto n = std::chrono::steady_clock::now();
    // First send request, then receive
    boost::asio::write(m_socket, boost::asio::buffer(request, 5));

    // Receive the header
    boost::asio::read(m_socket, boost::asio::buffer(req, 8));
    int64_t pck_size = *static_cast<int64_t*>(static_cast<void*>(&req[0]));

    // TODO create struct for header

    _r_size = pck_size;
    if (!pck_size) return nullptr;

    auto pckt = std::make_unique<char[]>(pck_size);

    boost::asio::read(m_socket, boost::asio::buffer(pckt.get() + 8,
                                                    pck_size - HEADER_LENGTH));
    std::memcpy(pckt.get(), req, HEADER_LENGTH);

    // int64_t* hdr = static_cast<int64_t*>(static_cast<void*>(pckt.get()));
    // auto now     = std::chrono::steady_clock::now();
    // auto tk =
    //     std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch())
    //         .count();
    // auto t_k = std::chrono::duration_cast<std::chrono::microseconds>(now -
    // n).count();
    //
    // auto took = tk - hdr[2];
    // if (took > 400)
    //     LOG_CRITICAL("TOOK", fmt::format("{} {} ", took, t_k));
    return std::move(pckt);
}

// TODO finish here
void Client::Close() { m_socket.close(); }
