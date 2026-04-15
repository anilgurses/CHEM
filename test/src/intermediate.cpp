#include "chem/channel/intermediate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <functional>

#include "chem/common.h"

using namespace chem;

// Helper: create minimal pools needed by Intermediate constructor
static payloadPoolSPtr makePayloadPool() {
    return std::make_shared<udpDataPool_t>(4, "test_payload");
}

static std::vector<iqPoolSPtr> makeIqPools() {
    return {std::make_shared<iqPool_t>(4, "test_iq")};
}

// ---------------------------------------------------------------------------
// Path loss mode — string parsing and enum dispatch
// ---------------------------------------------------------------------------

TEST(Intermediate, SetPathLossModeString) {
    auto pp = makePayloadPool();
    auto iq = makeIqPools();
    Intermediate im(1e9, pp, iq);

    im.setPathLossMode("FREE_SPACE");
    EXPECT_EQ(im.getPathLossMode(), PropagationModel::FREE_SPACE);

    im.setPathLossMode("2_RAY");
    EXPECT_EQ(im.getPathLossMode(), PropagationModel::TWO_RAY);

    im.setPathLossMode("3GPP_38_901");
    EXPECT_EQ(im.getPathLossMode(), PropagationModel::THREE_GPP_38_901);

    im.setPathLossMode("OKUMURA_HATA");
    EXPECT_EQ(im.getPathLossMode(), PropagationModel::OKUMURA_HATA);

    im.setPathLossMode("LONGLEY_RICE");
    EXPECT_EQ(im.getPathLossMode(), PropagationModel::LONGLEY_RICE);

    im.setPathLossMode("NONE");
    EXPECT_EQ(im.getPathLossMode(), PropagationModel::NONE);
}

TEST(Intermediate, SetPathLossModeStringAliases) {
    auto pp = makePayloadPool();
    auto iq = makeIqPools();
    Intermediate im(1e9, pp, iq);

    im.setPathLossMode("TWO_RAY");
    EXPECT_EQ(im.getPathLossMode(), PropagationModel::TWO_RAY);

    im.setPathLossMode("3GPP");
    EXPECT_EQ(im.getPathLossMode(), PropagationModel::THREE_GPP_38_901);

    im.setPathLossMode("HATA");
    EXPECT_EQ(im.getPathLossMode(), PropagationModel::OKUMURA_HATA);

    im.setPathLossMode("ITM");
    EXPECT_EQ(im.getPathLossMode(), PropagationModel::LONGLEY_RICE);
}

TEST(Intermediate, UpdatePathLossSetsModel) {
    auto pp = makePayloadPool();
    auto iq = makeIqPools();
    Intermediate im(1e9, pp, iq);

    im.updatePathLoss("2_RAY", -0.8f);
    EXPECT_EQ(im.getPathLossMode(), PropagationModel::TWO_RAY);

    im.updatePathLoss(PropagationModel::FREE_SPACE, -1.0f);
    EXPECT_EQ(im.getPathLossMode(), PropagationModel::FREE_SPACE);
}

// ---------------------------------------------------------------------------
// Max latency — verifies ms ↔ ns conversion math
// ---------------------------------------------------------------------------

TEST(Intermediate, MaxLatencyConversion) {
    auto pp = makePayloadPool();
    auto iq = makeIqPools();
    Intermediate im(1e9, pp, iq, 10);

    EXPECT_EQ(im.getMaxLatency(), 10);

    im.setMaxLatency(50);
    EXPECT_EQ(im.getMaxLatency(), 50);

    im.setMaxLatency(1);
    EXPECT_EQ(im.getMaxLatency(), 1);

    im.setMaxLatency(1000);
    EXPECT_EQ(im.getMaxLatency(), 1000);
}

// ---------------------------------------------------------------------------
// Timing callback state machine
// ---------------------------------------------------------------------------

TEST(Intermediate, TimingCallbackSetAndClear) {
    auto pp = makePayloadPool();
    auto iq = makeIqPools();
    Intermediate im(1e9, pp, iq);

    EXPECT_FALSE(im.isTimingEnabled());

    std::atomic<int> call_count{0};
    im.setTimingCallback(
        [&call_count](const ChannelProcessTiming&) { ++call_count; });
    EXPECT_TRUE(im.isTimingEnabled());

    im.clearTimingCallback();
    EXPECT_FALSE(im.isTimingEnabled());
}

// ---------------------------------------------------------------------------
// PropagationModel string conversions (common.h utilities)
// ---------------------------------------------------------------------------

TEST(PropagationModelConversion, RoundTrip) {
    auto models = {PropagationModel::FREE_SPACE,   PropagationModel::TWO_RAY,
                   PropagationModel::THREE_GPP_38_901,
                   PropagationModel::OKUMURA_HATA, PropagationModel::LONGLEY_RICE,
                   PropagationModel::NONE};

    for (auto m : models) {
        std::string str = PropagationModelToString(m);
        PropagationModel parsed = PropagationModelFromString(str);
        EXPECT_EQ(parsed, m) << "Failed round-trip for: " << str;
    }
}

TEST(PropagationModelConversion, UnknownString) {
    EXPECT_EQ(PropagationModelFromString("GARBAGE"),
              PropagationModel::UNKNOWN);
}

TEST(PropagationModelConversion, CaseInsensitive) {
    EXPECT_EQ(PropagationModelFromString("free_space"),
              PropagationModel::FREE_SPACE);
    EXPECT_EQ(PropagationModelFromString("Free_Space"),
              PropagationModel::FREE_SPACE);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
