#include "chem/channel/channel.h"

#include <gtest/gtest.h>

#include <cmath>

#include "chem/aerpaw/node_characteristics.h"
#include "chem/common.h"
#include "chem/dsp/channel.h"
#include "chem/models/node_config.hpp"

using namespace chem;
using chem::dsp::channel::CIRApplyMethod;
using chem::signal_v;

// ---------------------------------------------------------------------------
// Port coefficient routing
// ---------------------------------------------------------------------------

TEST(Channel, DefaultCoeffIsOne) {
    Channel ch("s", "d", 2, 2);
    EXPECT_FLOAT_EQ(ch.getChCoeffs(0, 0), 1.0f);
    EXPECT_FLOAT_EQ(ch.getChCoeffs(1, 1), 1.0f);
}

TEST(Channel, InvalidPortReturnsZero) {
    Channel ch("s", "d", 1, 1);
    EXPECT_FLOAT_EQ(ch.getChCoeffs(5, 5), 0.0f);
}

TEST(Channel, UpdateCoeffAffectsRouting) {
    Channel ch("s", "d", 2, 2);
    chId id{0, 1};
    ch.updateChCoeff(id, 0.0f);
    EXPECT_FLOAT_EQ(ch.getChCoeffs(0, 1), 0.0f);
    // Other ports unaffected
    EXPECT_FLOAT_EQ(ch.getChCoeffs(0, 0), 1.0f);
}

// ---------------------------------------------------------------------------
// CIR tap management
// ---------------------------------------------------------------------------

TEST(Channel, ChTapsLifecycle) {
    Channel ch("s", "d", 2, 2);
    chId id{0, 0};

    EXPECT_EQ(ch.getChTapCount(id), 0u);

    signal_v taps = {fc(1.0f, 0.0f), fc(0.5f, 0.0f), fc(0.1f, 0.0f)};
    ch.updateChTaps(id, taps);
    EXPECT_EQ(ch.getChTapCount(id), 3u);

    // Overwrite with different taps
    signal_v taps2 = {fc(0.8f, 0.0f)};
    ch.updateChTaps(id, taps2);
    EXPECT_EQ(ch.getChTapCount(id), 1u);

    ch.clearChTaps(id);
    EXPECT_EQ(ch.getChTapCount(id), 0u);
}

// ---------------------------------------------------------------------------
// processChannel — core signal processing
// ---------------------------------------------------------------------------

static NodeConfig makeTestNodeConfig(const std::string& id, double txGain,
                                     double rxGain) {
    NodeConfig cfg;
    cfg.setId(id);
    cfg.setName(id);
    cfg.setNumChannels(1);
    cfg.setInputFormat("fc32");
    cfg.setOutputFormat("fc32");
    cfg.setDeviceGains(txGain, rxGain);

    chem::aerpaw::NodeCharacteristics chars{};
    chars.source_power_dbfs = -20.0;  // known reference power in dBFS
    cfg.setCharacteristics(chars);

    return cfg;
}

TEST(Channel, ProcessChannelNullConfigReturnsEarly) {
    Channel ch("s", "d", 1, 1);
    const size_t sz = 64;
    signal_v src(sz, fc(1.0f, 0.0f));
    signal_v dest(sz, fc(0.0f, 0.0f));

    ChannelProcessParams params{};
    params.coeff = 1.0f;
    params.s_perBuff = sz;
    params.sample_ratio = 1.0f;
    params.sample_rate_hz = 1e6;
    params.src_config = nullptr;
    params.dest_config = nullptr;

    ch.processChannel(src, dest, params);

    // dest should remain unchanged
    for (size_t i = 0; i < sz; ++i) {
        EXPECT_EQ(dest[i], fc(0.0f, 0.0f));
    }
}

TEST(Channel, ProcessChannelAppliesPathLoss) {
    // Two channels at different distances should produce different output power
    const size_t sz = 512;
    signal_v src(sz, fc(0.5f, 0.25f));
    signal_v dest_near(sz, fc(0.0f, 0.0f));
    signal_v dest_far(sz, fc(0.0f, 0.0f));

    auto src_cfg = makeTestNodeConfig("src", TX_GAIN, 0.0);
    auto dest_cfg = makeTestNodeConfig("dest", 0.0, RX_GAIN);

    // Use NONE propagation model to avoid path loss, establish baseline
    auto make_params = [&](const NodeConfig* sc, const NodeConfig* dc) {
        ChannelProcessParams p{};
        p.coeff = 1.0f;
        p.s_perBuff = sz;
        p.sample_ratio = 1.0f;
        p.sample_rate_hz = 1e6;
        p.src_channel_index = 0;
        p.dest_channel_index = 0;
        p.propagationModel = PropagationModel::FREE_SPACE;
        p.ground_coeff = -1.0f;
        p.src_config = sc;
        p.dest_config = dc;
        p.freq = 1e9;
        p.timing = nullptr;
        return p;
    };

    // Near channel: 10 m
    Channel ch_near("src", "dest", 1, 1);
    ch_near.updateDistance(10.0f);
    ch_near.setShadowingSTD(0.0);
    ch_near.updateNoiseType(NoiseType::NONE);
    auto p_near = make_params(&src_cfg, &dest_cfg);
    ch_near.processChannel(src, dest_near, p_near);

    //10 km
    Channel ch_far("src", "dest", 1, 1);
    ch_far.updateDistance(10000.0f);
    ch_far.setShadowingSTD(0.0);
    ch_far.updateNoiseType(NoiseType::NONE);
    auto p_far = make_params(&src_cfg, &dest_cfg);
    ch_far.processChannel(src, dest_far, p_far);

    // Compute average power of each output
    float power_near = 0.0f, power_far = 0.0f;
    for (size_t i = 0; i < sz; ++i) {
        power_near += std::norm(dest_near[i]);
        power_far += std::norm(dest_far[i]);
    }
    power_near /= static_cast<float>(sz);
    power_far /= static_cast<float>(sz);

    // Far signal should have significantly less power than near signal
    // FSPL difference: 20*log10(10000/10) = 60 dB
    EXPECT_GT(power_near, power_far);
}

// TODO: Do some calculation on matlab or python to verify CIR output against known convolution result
// Just for the unit tests
TEST(Channel, ProcessChannelWithCIR) {
    // Verify that CIR taps modify the output signal
    const size_t sz = 256;
    signal_v src(sz, fc(0.0f, 0.0f));
    // Put energy in first few samples only (impulse-like)
    for (size_t i = 0; i < 4; ++i) src[i] = fc(1.0f, 0.0f);

    auto src_cfg = makeTestNodeConfig("src", TX_GAIN, 0.0);
    auto dest_cfg = makeTestNodeConfig("dest", 0.0, RX_GAIN);

    ChannelProcessParams params{};
    params.coeff = 1.0f;
    params.s_perBuff = sz;
    params.sample_ratio = 1.0f;
    params.sample_rate_hz = 1e6;
    params.src_channel_index = 0;
    params.dest_channel_index = 0;
    params.propagationModel = PropagationModel::NONE;
    params.ground_coeff = -1.0f;
    params.src_config = &src_cfg;
    params.dest_config = &dest_cfg;
    params.freq = 1e9;

    // Without CIR
    signal_v dest_no_cir(sz, fc(0.0f, 0.0f));
    Channel ch_no_cir("src", "dest", 1, 1);
    ch_no_cir.setShadowingSTD(0.0);
    ch_no_cir.updateNoiseType(NoiseType::NONE);
    ch_no_cir.setCIRApplyMethod(CIRApplyMethod::NONE);
    ch_no_cir.processChannel(src, dest_no_cir, params);

    // With CIR 
    signal_v dest_cir(sz, fc(0.0f, 0.0f));
    Channel ch_cir("src", "dest", 1, 1);
    ch_cir.setShadowingSTD(0.0);
    ch_cir.updateNoiseType(NoiseType::NONE);
    ch_cir.setCIRApplyMethod(CIRApplyMethod::DIRECT_CONV);
    // CIR key uses (dest_channel, src_channel)
    chId cir_key{0, 0};
    signal_v taps = {fc(1.0f, 0.0f), fc(0.0f, 0.0f), fc(0.5f, 0.0f)};
    ch_cir.updateChTaps(cir_key, taps);
    ch_cir.processChannel(src, dest_cir, params);

    // The CIR should spread energy into later samples
    bool differs = false;
    for (size_t i = 0; i < sz; ++i) {
        float diff = std::abs(dest_cir[i] - dest_no_cir[i]);
        if (diff > 1e-6f) {
            differs = true;
            break;
        }
    }
    EXPECT_TRUE(differs);
}

TEST(Channel, ProcessChannelTimingCollection) {
    const size_t sz = 128;
    signal_v src(sz, fc(0.3f, 0.1f));
    signal_v dest(sz, fc(0.0f, 0.0f));

    auto src_cfg = makeTestNodeConfig("src", TX_GAIN, 0.0);
    auto dest_cfg = makeTestNodeConfig("dest", 0.0, RX_GAIN);

    ChannelProcessTiming timing{};
    ChannelProcessParams params{};
    params.coeff = 1.0f;
    params.s_perBuff = sz;
    params.sample_ratio = 1.0f;
    params.sample_rate_hz = 1e6;
    params.src_channel_index = 0;
    params.dest_channel_index = 0;
    params.propagationModel = PropagationModel::FREE_SPACE;
    params.ground_coeff = -1.0f;
    params.src_config = &src_cfg;
    params.dest_config = &dest_cfg;
    params.freq = 1e9;
    params.timing = &timing;

    Channel ch("src", "dest", 1, 1);
    ch.updateDistance(100.0f);
    ch.setShadowingSTD(0.0);
    ch.processChannel(src, dest, params);

    // Timing fields should be populated in order
    EXPECT_GT(timing.t_start, 0);
    EXPECT_GE(timing.t_resample, timing.t_start);
    EXPECT_GE(timing.t_cir, timing.t_resample);
    EXPECT_GE(timing.t_pathloss, timing.t_cir);
    EXPECT_GE(timing.t_noise, timing.t_pathloss);
    EXPECT_GE(timing.t_end, timing.t_noise);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
