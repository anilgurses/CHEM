#include "chem/net/udp_client.h"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <chrono>
#include <cstring>

#include "chem/common.h"
#include "chem/models/signal.hpp"

#define _GNU_SOURCE

using namespace chem;
using namespace std::chrono;
using namespace std::chrono_literals;

UDPClient::UDPClient(const std::string& endpoint, const unsigned short port,
                     iqPoolSPtr pool)
    : m_socket(m_io_service, udp::endpoint(udp::v4(), 0)), m_iq_pool(pool) {
    m_port = port;
    m_remote_endpoint =
        udp::endpoint(boost::asio::ip::address::from_string(endpoint), port);
    std::memset(&m_dest_addr, 0, sizeof(m_dest_addr));
    m_dest_addr.sin_family = AF_INET;
    m_dest_addr.sin_port = htons(m_port);
    inet_pton(AF_INET, endpoint.c_str(), &m_dest_addr.sin_addr);

    int priority = 6;
    setsockopt(m_socket.native_handle(), SOL_SOCKET, SO_PRIORITY, &priority,
               sizeof(priority));

    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);

    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        LOG_ERROR("UDPCLIENT", "Failed to set thread priority");
    }
}

UDPClient::~UDPClient() {}

void UDPClient::Close() { m_socket.close(); }

void UDPClient::Send(std::unique_ptr<chem::Signal> sig,
                     const uint8_t& bytes_per_otw) {
    if (!sig) {
        return;
    }

    Header final_header = *(sig->getHeader());
    const uint8_t nof_ch = final_header.number_of_channels;
    const uint32_t n_samples = final_header.number_of_samples;

    bool should_pass = n_samples == 0;
    should_pass |= nof_ch == 0;
    should_pass |= bytes_per_otw == 0;
    should_pass |= m_port == 0;

    if (should_pass) {
        m_iq_pool->release(std::move(sig->dataArray()));
        return;
    }

    const size_t size = sig->getSize();

    size_t current_payload_size = PAYLOAD_SIZE;
    size_t total_frags =
        (size + HEADER_SIZE) / current_payload_size +
        (((size + HEADER_SIZE) % current_payload_size) ? 1 : 0);

    if (total_frags < BATCH_SIZE && total_frags > 0) {
        current_payload_size =
            (size + HEADER_SIZE + BATCH_SIZE - 1) / BATCH_SIZE;
        total_frags = (size + HEADER_SIZE) / current_payload_size;
    }

    if (total_frags == 0) {
        m_iq_pool->release(std::move(sig->dataArray()));
        return;
    }

    struct mmsghdr msgvec[BATCH_SIZE];
    struct iovec iovecs[BATCH_SIZE][nof_ch + 2];  // One iovec for the header +
                                                  // one for each channel
    UDPHeader headers[BATCH_SIZE];

    Header main_header = final_header;
    main_header.total_frags = total_frags;

    UDPHeader frag_hdr;

    size_t sent = 0;
    size_t remaining = size;
    size_t payload_size = 0;
    size_t size_per_ch = 0;
    size_t sent_bytes_per_ch = 0;
    int msg_idx_in_batch = 0;

    for (size_t i = 0; i < total_frags; i++) {
        UDPHeader& current_header = headers[msg_idx_in_batch];
        current_header = frag_hdr;

        current_header.seq_number = i;

        if (total_frags == 1) {
            current_header.flag = 1 | 4;  // Start and End
        } else if (i == 0) {
            current_header.flag = 1;  // Start
        } else if (i == total_frags - 1) {
            current_header.flag = 4;  // End
        } else {
            current_header.flag = 2;  // Data
        }

        remaining = size - sent;
        payload_size = std::min(remaining, current_payload_size);

        if (payload_size == 0) {
            break;
        }

        auto now = steady_clock::now();
        current_header.timestamp =
            duration_cast<nanoseconds>(now.time_since_epoch()).count();

        iovecs[msg_idx_in_batch][0].iov_base = &current_header;
        iovecs[msg_idx_in_batch][0].iov_len = UDP_HEADER_SIZE;

        int iov_offset = 1;
        if (i == 0) {
            iovecs[msg_idx_in_batch][1].iov_base = &main_header;
            iovecs[msg_idx_in_batch][1].iov_len = HEADER_SIZE;
            iov_offset = 2;
            if (total_frags > 1) {
                payload_size -= HEADER_SIZE;
            }
        }

        size_per_ch = payload_size / nof_ch;
        size_t aligned_payload = size_per_ch * nof_ch;

        if (aligned_payload == 0) {
#ifdef ENABLE_DEBUG_LOG
            LOG_ERROR("UDPCLIENT",
                      "Payload too small to distribute across channels");
#endif
            break;
        }

        if (aligned_payload != payload_size) {
            payload_size = aligned_payload;
        }

        for (size_t c = 0; c < nof_ch; c++) {
            iovecs[msg_idx_in_batch][c + iov_offset].iov_base =
                sig->getData(c) + sent_bytes_per_ch;
            iovecs[msg_idx_in_batch][c + iov_offset].iov_len = size_per_ch;
        }

        current_header.size = payload_size;
        sent += payload_size;
        sent_bytes_per_ch += size_per_ch;

        msgvec[msg_idx_in_batch].msg_hdr.msg_name = &m_dest_addr;
        msgvec[msg_idx_in_batch].msg_hdr.msg_namelen = sizeof(m_dest_addr);
        msgvec[msg_idx_in_batch].msg_hdr.msg_iov = iovecs[msg_idx_in_batch];
        msgvec[msg_idx_in_batch].msg_hdr.msg_iovlen = nof_ch + iov_offset;
        msgvec[msg_idx_in_batch].msg_hdr.msg_control = NULL;
        msgvec[msg_idx_in_batch].msg_hdr.msg_controllen = 0;

        msg_idx_in_batch++;

        if (msg_idx_in_batch == BATCH_SIZE || i == total_frags - 1) {
            int ret =
                sendmmsg(m_socket.native_handle(), msgvec, msg_idx_in_batch, 0);
            if (ret < 0) {
                char err_buf[256];
                strerror_r(errno, err_buf, sizeof(err_buf));
                LOG_ERROR("UDPCLIENT",
                          "Failed to send messages: " + std::string(err_buf));
            }
            msg_idx_in_batch = 0;
        }
    }
    m_iq_pool->release(std::move(sig->dataArray()));
}

// void UDPClient::Send(std::unique_ptr<chem::Signal> sig, const uint8_t&
// bytes_per_otw) {
//     boost::system::error_code error;
//
//     Header hdr = *(sig->getHeader());
//
//     size_t sent      = 0;
//     uint8_t nof_ch   = hdr.number_of_channels;
//     size_t n_samples = hdr.number_of_samples;
//     size_t size      = sig->getSizeWoH();
//
//     size_t total_frags =
//         std::round(((n_samples * nof_ch) + PAYLOAD_N_SAMPLES - 1) /
//         PAYLOAD_N_SAMPLES);
//
//     while ((hdr.duration / nof_ch) % total_frags != 0) {
//         if (total_frags > MAX_FRAGS) {
//             total_frags = std::round(
//                 ((n_samples * nof_ch) + PAYLOAD_N_SAMPLES - 1) /
//                 PAYLOAD_N_SAMPLES);
//             break;
//         }
//         ++total_frags;
//     }
//
//     size_t raw_per_size_ch = n_samples * bytes_per_otw;
//     size_t ns_per_frag     = n_samples / total_frags;
//     size_t size_per_ch     = ns_per_frag * bytes_per_otw;
//     size_t size_per_fr     = size_per_ch * nof_ch;
//     hdr.size               = size_per_fr + HEADER_SIZE;
//     hdr.number_of_samples  = ns_per_frag;
//     hdr.tot_n_samples      = n_samples;
//
//     int64_t total_ns = S_2_NS(1) * hdr.number_of_samples;
//     hdr.duration     = (total_ns + sig->getSrate() / 2) / sig->getSrate();
//
//     size_t size_after_fr = total_frags * size_per_fr;
//     bool size_overflow   = size < size_after_fr;
//
//     if (size_overflow) {
//         total_frags++;
//     }
//
//     for (size_t i = 0; i < total_frags; i++) {
//         std::memset(packet, 0, HEADER_SIZE);
//
//         hdr.frag_no = i;
//
//         if (i == 0) {
//             hdr.flag = 1; // Start
//         } else if (i == total_frags - 1) {
//             hdr.flag = 4; // Data
//         } else {
//             hdr.flag = 2; // End
//         }
//
//         if ((i == total_frags - 1) && size_overflow) {
//             size_per_fr           = size - (sent * nof_ch);
//             size_per_ch           = size_per_fr / nof_ch;
//             hdr.size              = size_per_fr + HEADER_SIZE;
//             hdr.number_of_samples = size_per_ch / bytes_per_otw;
//             total_ns              = S_2_NS(1) * hdr.number_of_samples;
//             hdr.duration          = (total_ns + sig->getSrate() / 2) /
//             sig->getSrate();
//         }
//
//         hdr.end = hdr.start + hdr.duration;
//
//         std::memcpy(packet, &hdr, HEADER_SIZE);
//
//         for (size_t c = 0; c < nof_ch; c++) {
//             std::memcpy(packet + HEADER_SIZE + c * size_per_ch,
//                 sig->getData(c) + sent,
//                 size_per_ch);
//         }
//
//         // buffers.push_back(boost::asio::buffer(_packet, hdr.size));
//         m_socket.send_to(boost::asio::buffer(&packet, hdr.size),
//         m_remote_endpoint);
//
//         hdr.start = hdr.end;
//         sent += size_per_ch;
//     }
//
//     m_iq_pool->release(std::move(sig->dataArray()));
//
//     if (error) {
//         LOG_DEBUG("UDPCLIENT",
//             fmt::format("Couldn't send data to remote endpoint : {}",
//             error.message()));
//         return;
//     }
// }
