#pragma once

#include <complex>
#include <cstdint>
#include <memory>

#include "../common.h"

// TODO call resampling at here
// TODO Don't use unique ptr for the class
namespace chem {
class Signal {
   public:
    Signal(std::unique_ptr<char[]> iq, struct Header hdr, const double& freq,
           std::string id = "tx")
        : _srcId(id), _size(hdr.size), _freq(freq), header(hdr) {
        if (hdr.size) {
            iq_data = std::move(iq);
            // Copy header
            _srate = hdr.sample_rate;
            _numCh = hdr.number_of_channels;
            _numSamp = hdr.number_of_samples;
            sample_step = S_2_NS(1.0) / _srate;
        }
    };

    // ~Signal() = delete;

    // Pass iq data to class
    void initData(std::unique_ptr<char[]>& iq, const uint32_t& d_size) {
        if (iq) iq_data = std::move(iq);
        _size = d_size;
    }

    struct Header* getHeader() { return &header; }

    // Get data with header
    char* getData() { return iq_data.get(); }

    std::unique_ptr<char[]> dataArray() { return std::move(iq_data); }

    // Get data without header
    char* getData(const size_t& chNo) {
        if (chNo >= _numCh) return nullptr;
        return iq_data.get() + chNo * getSizePerBuff();
    }

    const std::string& getSrcId() const { return _srcId; }

    uint8_t getNumberOfChannels() { return _numCh; }

    size_t getNumSampPerBuff() { return _numSamp; }

    size_t getTotalNumSamps() { return _numSamp * _numCh; }

    size_t getSizePerBuff() {
        if (_numCh == 0) return 0;
        return getSize() / _numCh;
    }

    uint32_t getSize() const { return _size; };

    float getSrate() const { return _srate; };

    float getSampleStep() const { return sample_step; }

    double getFreq() const { return _freq; };

    void setTRecv(const int64_t& t) { t_recv = t; }

    void setTProcStart(const int64_t& t) { t_proc_start = t; }

    void setTProcEnd(const int64_t& t) { t_proc_end = t; }

    void setTSend(const int64_t& t) { t_send = t; }

    int64_t getTRecv() { return t_recv; }

    int64_t getTProcStart() { return t_proc_start; }

    int64_t getTProcEnd() { return t_proc_end; }

    int64_t getTSend() { return t_send; }

   private:
    // Processing related information
    std::string _srcId;
    uint32_t _size;
    float _srate;
    double _freq;
    uint8_t _numCh;
    size_t _numSamp;
    float sample_step;

    // TODO: Maybe add a new flag called profiling
    // Timestamps for profiling
    int64_t t_recv{0};
    int64_t t_proc_start{0};
    int64_t t_proc_end{0};
    int64_t t_send{0};
    // Timestamps for profiling

    struct Header header;

    std::unique_ptr<char[]> iq_data;
};
}  // namespace chem
