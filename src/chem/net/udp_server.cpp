#include "chem/net/udp_server.h"

#include <chem/common.h>
#include <sys/socket.h>

#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <cstring>
#include <memory>

#include "chem/models/data_pool.hpp"
#define _GNU_SOURCE

using namespace chem;

UDPServer::UDPServer(const std::string& endpoint, unsigned short port,
                     iqPoolSPtr pool)
    : m_socket(m_io_service, udp::endpoint(udp::v4(), port)),
      m_pool(pool),
      m_udpPool(UDP_CAP, "SERVER", true) {
    m_endpoint =
        udp::endpoint(boost::asio::ip::address::from_string(endpoint), port);

    try {
        boost::asio::socket_base::receive_buffer_size option(8 * 1024 * 1024);
        m_socket.set_option(option);
        boost::asio::socket_base::receive_buffer_size actual_size;
        m_socket.get_option(actual_size);
        LOG_DEBUG("UDP_SERVER",
                  fmt::format("Socket receive buffer size set to: {} bytes",
                              actual_size.value()));
    } catch (const boost::system::system_error& ex) {
        LOG_ERROR("UDP_SERVER",
                  fmt::format("Failed to set socket receive buffer size: {}",
                              ex.what()));
    }
}

UDPServer::~UDPServer() {}

/**
 * @brief Receives and reassembles a complete signal using batched recvmmsg.
 * This function blocks until one complete signal (super-packet) is received
 * or a sequence is broken. It is highly efficient due to minimized system
 * calls and contiguous memory writes that avoid the need for memmove.
 * @param _hdr An output parameter that will be filled with the final header of
 * the reassembled signal.
 * @return A unique_ptr to the buffer containing the reassembled signal data, or
 * nullptr on failure/timeout.
 */
dArray_uptr UDPServer::Recv(struct Header& _hdr) {
    struct mmsghdr msgvec[BATCH_SIZE];
    struct iovec iovecs[BATCH_SIZE];
    std::array<dArray_uptr, BATCH_SIZE> packet_buffers;

    for (int i = 0; i < BATCH_SIZE; i++) {
        packet_buffers[i] = m_udpPool.acquire();
        iovecs[i].iov_base = packet_buffers[i].get();
        iovecs[i].iov_len = MTU_SIZE;
        msgvec[i].msg_hdr.msg_iov = &iovecs[i];
        msgvec[i].msg_hdr.msg_iovlen = 1;
        msgvec[i].msg_hdr.msg_name = nullptr;
        msgvec[i].msg_hdr.msg_namelen = 0;
        msgvec[i].msg_hdr.msg_control = nullptr;
        msgvec[i].msg_hdr.msg_controllen = 0;
    }

    Header final_hdr;
    dArray_uptr final_data_buffer = m_pool->acquire();
    bool is_assembling = false;
    uint32_t last_seq_no = 0;
    uint32_t fragments_received = 0;
    size_t bytes_written_per_ch = 0;
    size_t per_channel_stride = 0;
    uint8_t active_num_channels = 0;
    int remaining_frags = 0;
    m_current_batch_size = BATCH_SIZE;

    auto reset_assembly = [&](const char* reason) {
#ifdef ENABLE_DEBUG_LOG
        LOG_WARN("UDP_SERVER", reason);
#endif
        is_assembling = false;
        last_seq_no = 0;
        fragments_received = 0;
        bytes_written_per_ch = 0;
        per_channel_stride = 0;
        active_num_channels = 0;
        final_hdr = Header{};
        m_current_batch_size = BATCH_SIZE;
    };

    while (m_socket.is_open()) {
        struct timespec timeout = {0, 1000000};  // 1 ms
        int packets_received = recvmmsg(m_socket.native_handle(), msgvec,
                                        m_current_batch_size, 0, &timeout);

        if (packets_received <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (is_assembling) {
#ifdef ENABLE_DEBUG_LOG
                    LOG_WARN("UDP_SERVER", "Timeout while assembling packet");
#endif
                    is_assembling = false;
                    m_pool->release(std::move(final_data_buffer));
                    _hdr.size = 0;
                    m_current_batch_size = BATCH_SIZE;
                    return nullptr;
                }
                continue;
            }

#ifdef ENABLE_DEBUG_LOG
            char err_buf[256];
            strerror_r(errno, err_buf, sizeof(err_buf));
            LOG_WARN("UDP_SERVER",
                     fmt::format("No packets received or error: {}", err_buf));
#endif
            for (auto& buf : packet_buffers) {
                m_udpPool.release(std::move(buf));
            }
            m_pool->release(std::move(final_data_buffer));
            _hdr.size = 0;
            m_current_batch_size = BATCH_SIZE;
            return nullptr;
        }

        for (int i = 0; i < packets_received; i++) {
            char* packet =
                static_cast<char*>(msgvec[i].msg_hdr.msg_iov->iov_base);
            size_t rx_len = msgvec[i].msg_len;

            if (rx_len < UDP_HEADER_SIZE) continue;

            UDPHeader* udp_hdr = reinterpret_cast<UDPHeader*>(packet);
            const bool start_flag = (udp_hdr->flag & 0x1) != 0;
            const bool end_flag = (udp_hdr->flag & 0x4) != 0;

            if (!is_assembling) {
                if (!start_flag) {
#ifdef ENABLE_DEBUG_LOG
                    LOG_WARN(
                        "UDP_SERVER",
                        fmt::format(
                            "Discarding packet with seq no: {} and flag: {} "
                            "while not assembling",
                            udp_hdr->seq_number, udp_hdr->flag));
#endif
                    continue;
                }

                if (rx_len < UDP_HEADER_SIZE + sizeof(Header)) {
                    continue;
                }

                final_hdr =
                    *(reinterpret_cast<Header*>(packet + UDP_HEADER_SIZE));
                active_num_channels = final_hdr.number_of_channels;
                per_channel_stride = (active_num_channels == 0)
                                         ? 0
                                         : final_hdr.size / active_num_channels;
                bytes_written_per_ch = 0;
                fragments_received = 0;
                last_seq_no = udp_hdr->seq_number;

                if (active_num_channels == 0 || per_channel_stride == 0 ||
                    final_hdr.size % active_num_channels != 0) {
                    reset_assembly(
                        "Invalid header information, dropping burst");
                    continue;
                }

                is_assembling = true;
            } else {
                if (udp_hdr->seq_number != last_seq_no + 1) {
                    reset_assembly("Fragment out of order, dropping burst");
                    continue;
                }
                last_seq_no = udp_hdr->seq_number;
            }

            const char* payload_start = packet + UDP_HEADER_SIZE;
            size_t payload_len = rx_len - UDP_HEADER_SIZE;

            if (start_flag) {
                if (payload_len < HEADER_SIZE) {
                    reset_assembly("Malformed start fragment");
                    continue;
                }
                payload_start += HEADER_SIZE;
                payload_len -= HEADER_SIZE;
            }

            if (payload_len != udp_hdr->size) {
                reset_assembly("Payload size mismatch, discarding burst");
                continue;
            }

            if (!active_num_channels ||
                payload_len % active_num_channels != 0) {
                reset_assembly("Invalid channel payload, discarding burst");
                continue;
            }

            const size_t chunk_per_channel = payload_len / active_num_channels;

            if (chunk_per_channel == 0 ||
                bytes_written_per_ch + chunk_per_channel > per_channel_stride) {
                reset_assembly("Channel payload overflow, discarding burst");
                continue;
            }

            for (uint8_t c = 0; c < active_num_channels; ++c) {
                char* dest = final_data_buffer.get() +
                             (c * per_channel_stride) + bytes_written_per_ch;
                const char* src = payload_start + (c * chunk_per_channel);
                std::memcpy(dest, src, chunk_per_channel);
            }

            bytes_written_per_ch += chunk_per_channel;
            fragments_received++;

            if (end_flag) {
                if (bytes_written_per_ch != per_channel_stride) {
                    reset_assembly("Burst ended prematurely, discarding");
                    continue;
                }
                _hdr = final_hdr;
                for (auto& buf : packet_buffers) {
                    m_udpPool.release(std::move(buf));
                }
                m_current_batch_size = BATCH_SIZE;
                return std::move(final_data_buffer);
            }
        }
        // Dynamic batch size adjustment
        if (is_assembling && final_hdr.total_frags > 0) {
            remaining_frags =
                static_cast<int>(final_hdr.total_frags) - fragments_received;
            if (remaining_frags > 0 && remaining_frags < BATCH_SIZE) {
                m_current_batch_size = std::max(1, remaining_frags);
            } else {
                m_current_batch_size = BATCH_SIZE;
            }
        }
    }

    for (auto& buf : packet_buffers) {
        m_udpPool.release(std::move(buf));
    }

    m_pool->release(std::move(final_data_buffer));
    _hdr.size = 0;
    return nullptr;
}

void UDPServer::Close() { m_socket.close(); }
