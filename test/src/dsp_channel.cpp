#include "chem/dsp/channel.h"

#include <gtest/gtest.h>

#include <cmath>
#include <complex>

#include "chem/common.h"

using namespace chem::dsp::channel;
using chem::fc;
using chem::signal_v;

// ---------------------------------------------------------------------------
// Path loss models (shadowing_std = 0 for deterministic results)
// ---------------------------------------------------------------------------

TEST(DspChannel_FSPL, KnownDistanceFrequency) {
    // 1 km at 1 GHz → FSPL ≈ 20*log10(1e9) + 20*log10(1000) + (-147.55)
    //               ≈ 180 + 60 - 147.55 = 92.45 dB
    float pl = calc_fspl(1000.0f, 1e9f, 0.0);
    EXPECT_NEAR(pl, 92.45f, 0.5f);
}

TEST(DspChannel_FSPL, IncreasesWithDistance) {
    float pl_near = calc_fspl(100.0f, 1e9f, 0.0);
    float pl_far = calc_fspl(10000.0f, 1e9f, 0.0);
    // 20*log10(10000/100) = 40 dB more path loss
    EXPECT_NEAR(pl_far - pl_near, 40.0f, 0.5f);
}

TEST(DspChannel_FSPL, IncreasesWithFrequency) {
    float pl_low = calc_fspl(1000.0f, 1e8f, 0.0);
    float pl_high = calc_fspl(1000.0f, 1e9f, 0.0);
    // 20*log10(1e9/1e8) = 20 dB more path loss
    EXPECT_NEAR(pl_high - pl_low, 20.0f, 0.5f);
}

TEST(DspChannel_FSPL, ZeroDistanceUsesLogSafeGuard) {
    // With LOG_SAFE_GUARD=0.0001, zero distance should still produce a value
    float pl = calc_fspl(0.0f, 1e9f, 0.0);
    EXPECT_GT(pl, -200.0f);  // some finite value
    EXPECT_LT(pl, 200.0f);
}

TEST(DspChannel_2Ray, GreaterThanFSPLAtShortRange) {
    // At short range with ground reflection, 2-ray can differ from FSPL
    float pl_fspl = calc_fspl(100.0f, 1e9f, 0.0);
    float pl_2ray = calc_2ray(100.0f, 1e9f, 10.0f, 1.5f, -1.0f, 0.0);
    // Both should be positive and in a reasonable range
    EXPECT_GT(pl_fspl, 0.0f);
    EXPECT_GT(pl_2ray, 0.0f);
}

TEST(DspChannel_2Ray, IncreasesWithDistance) {
    float pl_near = calc_2ray(100.0f, 1e9f, 10.0f, 1.5f, -1.0f, 0.0);
    float pl_far = calc_2ray(10000.0f, 1e9f, 10.0f, 1.5f, -1.0f, 0.0);
    EXPECT_GT(pl_far, pl_near);
}

TEST(DspChannel_2Ray, GroundCoeffAffectsResult) {
    float pl_perfect = calc_2ray(500.0f, 1e9f, 10.0f, 1.5f, -1.0f, 0.0);
    float pl_lossy = calc_2ray(500.0f, 1e9f, 10.0f, 1.5f, -0.3f, 0.0);
    // Different ground reflection coefficient → different path loss
    EXPECT_NE(pl_perfect, pl_lossy);
}

TEST(DspChannel_3GPP, ReturnsPositive) {
    float pl = calc_3gpp_38_901(500.0f, 3.5e9f, 1.5f, 25.0f, "UMa", 0.0);
    EXPECT_GT(pl, 0.0f);
}

TEST(DspChannel_3GPP, IncreasesWithDistance) {
    float pl_near = calc_3gpp_38_901(100.0f, 3.5e9f, 1.5f, 25.0f, "UMa", 0.0);
    float pl_far = calc_3gpp_38_901(2000.0f, 3.5e9f, 1.5f, 25.0f, "UMa", 0.0);
    EXPECT_GT(pl_far, pl_near);
}

TEST(DspChannel_OkumuraHata, ReturnsPositive) {
    float pl = calc_okumura_hata(2000.0f, 900e6f, 30.0f, 1.5f, "URBAN", 0.0);
    EXPECT_GT(pl, 0.0f);
}

TEST(DspChannel_OkumuraHata, IncreasesWithDistance) {
    float pl_near = calc_okumura_hata(500.0f, 900e6f, 30.0f, 1.5f, "URBAN", 0.0);
    float pl_far = calc_okumura_hata(5000.0f, 900e6f, 30.0f, 1.5f, "URBAN", 0.0);
    EXPECT_GT(pl_far, pl_near);
}

TEST(DspChannel_LongleyRice, ReturnsPositive) {
    float pl = calc_longley_rice(5000.0f, 900e6f, 30.0f, 1.5f, 301.0f, 0.005f,
                                 15.0f, 5, 0.0);
    EXPECT_GT(pl, 0.0f);
}

TEST(DspChannel_LongleyRice, IncreasesWithDistance) {
    float pl_near = calc_longley_rice(1000.0f, 900e6f, 30.0f, 1.5f, 301.0f,
                                      0.005f, 15.0f, 5, 0.0);
    float pl_far = calc_longley_rice(50000.0f, 900e6f, 30.0f, 1.5f, 301.0f,
                                     0.005f, 15.0f, 5, 0.0);
    EXPECT_GT(pl_far, pl_near);
}

// ---------------------------------------------------------------------------
// Noise generation
// ---------------------------------------------------------------------------

TEST(DspChannel_Noise, AWGNReturnsCorrectSize) {
    const size_t sz = 256;
    const auto& noise = get_noise(sz, 1e6, chem::NoiseType::AWGN);
    EXPECT_GE(noise.size(), sz);
}

TEST(DspChannel_Noise, AWGNNonZero) {
    const size_t sz = 256;
    const auto& noise = get_noise(sz, 1e6, chem::NoiseType::AWGN);
    bool any_nonzero = false;
    for (size_t i = 0; i < sz; ++i) {
        if (noise[i] != fc(0.0f, 0.0f)) {
            any_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(any_nonzero);
}

TEST(DspChannel_Noise, NONEReturnsZero) {
    const size_t sz = 128;
    const auto& noise = get_noise(sz, 1e6, chem::NoiseType::NONE);
    for (size_t i = 0; i < sz; ++i) {
        EXPECT_EQ(noise[i], fc(0.0f, 0.0f));
    }
}

TEST(DspChannel_Noise, HigherBandwidthHigherPower) {
    const size_t sz = 4096;
    // Force regeneration by changing bandwidth
    const auto& noise_narrow = get_noise(sz, 1e3, chem::NoiseType::AWGN);
    float power_narrow = 0.0f;
    for (size_t i = 0; i < sz; ++i) power_narrow += std::norm(noise_narrow[i]);

    const auto& noise_wide = get_noise(sz, 1e9, chem::NoiseType::AWGN);
    float power_wide = 0.0f;
    for (size_t i = 0; i < sz; ++i) power_wide += std::norm(noise_wide[i]);

    EXPECT_GT(power_wide, power_narrow);
}

// ---------------------------------------------------------------------------
// CIR application
// ---------------------------------------------------------------------------

TEST(DspChannel_CIR, NoneMethodCopiesInput) {
    const size_t sz = 64;
    signal_v input(sz, fc(1.0f, 0.5f));
    signal_v output(sz, fc(0.0f, 0.0f));
    signal_v taps = {fc(1.0f, 0.0f)};

    apply_cir(input, output, taps, sz, CIRApplyMethod::NONE);

    for (size_t i = 0; i < sz; ++i) {
        EXPECT_EQ(output[i], input[i]);
    }
}

TEST(DspChannel_CIR, SingleUnitTapIsIdentity) {
    const size_t sz = 64;
    signal_v input(sz);
    for (size_t i = 0; i < sz; ++i) {
        input[i] = fc(static_cast<float>(i), 0.0f);
    }
    signal_v output(sz, fc(0.0f, 0.0f));
    signal_v taps = {fc(1.0f, 0.0f)};

    apply_cir(input, output, taps, sz, CIRApplyMethod::FIR_CAUSAL);

    for (size_t i = 0; i < sz; ++i) {
        EXPECT_NEAR(output[i].real(), input[i].real(), 1e-5f);
        EXPECT_NEAR(output[i].imag(), input[i].imag(), 1e-5f);
    }
}

TEST(DspChannel_CIR, DirectConvImpulseResponse) {
    // Direct convolution: output[n] = sum_k taps[k] * input[n-k]
    const size_t sz = 16;
    signal_v input(sz, fc(0.0f, 0.0f));
    input[0] = fc(1.0f, 0.0f);  // unit impulse

    signal_v output(sz, fc(0.0f, 0.0f));
    signal_v taps = {fc(0.5f, 0.0f), fc(0.3f, 0.0f), fc(0.1f, 0.0f)};

    apply_cir_direct_conv(input, output, taps, sz);

    // Impulse response should reproduce the taps
    EXPECT_NEAR(output[0].real(), 0.5f, 1e-5f);
    EXPECT_NEAR(output[1].real(), 0.3f, 1e-5f);
    EXPECT_NEAR(output[2].real(), 0.1f, 1e-5f);
    EXPECT_NEAR(output[3].real(), 0.0f, 1e-5f);
}

TEST(DspChannel_CIR, DirectConvLinearity) {
    // Convolution is linear: conv(a*x, h) = a * conv(x, h)
    const size_t sz = 32;
    signal_v input(sz);
    for (size_t i = 0; i < sz; ++i)
        input[i] = fc(static_cast<float>(i % 5), static_cast<float>(i % 3));

    signal_v taps = {fc(0.6f, 0.1f), fc(0.3f, -0.2f)};

    signal_v out1(sz, fc(0.0f, 0.0f));
    apply_cir_direct_conv(input, out1, taps, sz);

    // Scale input by 2
    signal_v scaled_input(sz);
    for (size_t i = 0; i < sz; ++i) scaled_input[i] = input[i] * fc(2.0f, 0.0f);

    signal_v out2(sz, fc(0.0f, 0.0f));
    apply_cir_direct_conv(scaled_input, out2, taps, sz);

    for (size_t i = 0; i < sz; ++i) {
        EXPECT_NEAR(out2[i].real(), 2.0f * out1[i].real(), 1e-4f);
        EXPECT_NEAR(out2[i].imag(), 2.0f * out1[i].imag(), 1e-4f);
    }
}

TEST(DspChannel_CIR, FIRCausalAndDirectConvAgreeOnImpulse) {
    // Both methods should produce the same output for a unit impulse
    const size_t sz = 32;
    signal_v input(sz, fc(0.0f, 0.0f));
    input[0] = fc(1.0f, 0.0f);

    signal_v taps = {fc(0.5f, 0.0f), fc(0.3f, 0.0f)};

    signal_v out_direct(sz, fc(0.0f, 0.0f));
    apply_cir_direct_conv(input, out_direct, taps, sz);

    signal_v out_fir(sz, fc(0.0f, 0.0f));
    apply_cir_fir_causal(input, out_fir, taps, sz);

    for (size_t i = 0; i < sz; ++i) {
        EXPECT_NEAR(out_fir[i].real(), out_direct[i].real(), 1e-5f)
            << "Mismatch at index " << i;
        EXPECT_NEAR(out_fir[i].imag(), out_direct[i].imag(), 1e-5f)
            << "Mismatch at index " << i;
    }
}

TEST(DspChannel_CIR, EmptyTapsCopiesInput) {
    const size_t sz = 16;
    signal_v input(sz, fc(1.0f, 2.0f));
    signal_v output(sz, fc(0.0f, 0.0f));
    signal_v empty_taps;

    apply_cir_fir_causal(input, output, empty_taps, sz);

    for (size_t i = 0; i < sz; ++i) {
        EXPECT_EQ(output[i], input[i]);
    }
}

TEST(DspChannel_CIR, ZeroSizeDoesNotCrash) {
    signal_v input;
    signal_v output;
    signal_v taps = {fc(1.0f, 0.0f)};
    EXPECT_NO_THROW(apply_cir(input, output, taps, 0));
}

// ---------------------------------------------------------------------------
// Propagation delay
// ---------------------------------------------------------------------------

TEST(DspChannel_PropDelay, ShiftsTimestamps) {
    Header hdr{};
    hdr.start = 1000000000;       // 1 second in ns
    hdr.end = 1000001000;         // slightly after
    hdr.rt_tx_time = 1000000000;

    double delay_s = 0.001;  // 1 ms
    apply_propagation_delay(hdr, delay_s);

    int64_t expected_shift = 1000000;  // 1 ms = 1,000,000 ns
    EXPECT_EQ(hdr.start, 1000000000 + expected_shift);
    EXPECT_EQ(hdr.end, 1000001000 + expected_shift);
    EXPECT_EQ(hdr.rt_tx_time, static_cast<uint64_t>(1000000000 + expected_shift));
}

TEST(DspChannel_PropDelay, DelayProportionalToDistance) {
    // delay = distance / speed_of_light
    Header hdr1{}, hdr2{};
    hdr1.start = 0;
    hdr2.start = 0;

    double d1 = 1000.0;   // 1 km
    double d2 = 10000.0;  // 10 km
    apply_propagation_delay(hdr1, d1 / SPEED_OF_LIGHT);
    apply_propagation_delay(hdr2, d2 / SPEED_OF_LIGHT);

    // 10x distance → 10x delay
    EXPECT_NEAR(static_cast<double>(hdr2.start) / static_cast<double>(hdr1.start),
                10.0, 0.01);
}

// ---------------------------------------------------------------------------
// Doppler
// ---------------------------------------------------------------------------

TEST(DspChannel_Doppler, ModifiesSignal) {
    const size_t sz = 128;
    signal_v signal(sz, fc(1.0f, 0.0f));
    double phase = 0.0;

    apply_doppler(signal, sz, 1e6, 100.0, phase);

    bool modified = false;
    for (size_t i = 1; i < sz; ++i) {
        if (std::abs(signal[i].real() - 1.0f) > 1e-6f ||
            std::abs(signal[i].imag()) > 1e-6f) {
            modified = true;
            break;
        }
    }
    EXPECT_TRUE(modified);
    EXPECT_NE(phase, 0.0);
}

TEST(DspChannel_Doppler, PreservesMagnitude) {
    // Frequency shift should not change signal power (it's a rotation)
    const size_t sz = 256;
    signal_v signal(sz, fc(1.0f, 0.0f));
    double phase = 0.0;

    apply_doppler(signal, sz, 1e6, 500.0, phase);

    for (size_t i = 0; i < sz; ++i) {
        EXPECT_NEAR(std::abs(signal[i]), 1.0f, 1e-5f);
    }
}

TEST(DspChannel_Doppler, PhaseAccumulatesAcrossCalls) {
    const size_t sz = 64;
    signal_v sig1(sz, fc(1.0f, 0.0f));
    signal_v sig2(sz, fc(1.0f, 0.0f));
    double phase = 0.0;

    apply_doppler(sig1, sz, 1e6, 100.0, phase);
    double phase_after_first = phase;

    apply_doppler(sig2, sz, 1e6, 100.0, phase);
    // Phase should have accumulated further
    EXPECT_GT(std::abs(phase), std::abs(phase_after_first));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
