// File/layer: tests/test_operation_plan.cpp | C++17, Qt-free
// Testing: UNIT: OperationPlanTest (test_core)
//
// A plan's rules by themselves (core/src/operation_plan.h, pure): what a
// step's numbers must fit, when two ranges share an address, the class
// and the unlock count core derives, and what fits one CAN frame. The
// loader's words for the same rules are PluginOperationTest's. Every
// address is made up.

#include "operation_plan.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

using ecuio_operation::plan;
using ecuio_operation::step;
using ecuio_operation::step_fault;

step read_step()
{
    step s;
    s.kind = ECUIO_STEP_READ;
    s.session = ECUIO_STEP_SESSION_EXTENDED;
    s.address = 0x00010000;
    s.bytes = 4096;
    s.chunk = 256;
    s.address_bytes = 4;
    s.size_bytes = 2;
    s.name = "low";
    return s;
}

step upload_step()
{
    step s = read_step();
    s.kind = ECUIO_STEP_UPLOAD;
    s.chunk = 0;
    s.size_bytes = 4;
    s.name = "high";
    return s;
}

/* The fault of base after change, "" for none */
template <typename Change> std::string fault(step base, Change change)
{
    change(base);
    const char* f = step_fault(base);
    return f != nullptr ? f : "";
}

TEST(OperationPlanTest, AStepAsDeclaredHasNoFault)
{
    EXPECT_EQ(step_fault(read_step()), nullptr);
    EXPECT_EQ(step_fault(upload_step()), nullptr);
    EXPECT_EQ(ecuio_operation::most_of(1), 0xFFu);
    EXPECT_EQ(ecuio_operation::most_of(2), 0xFFFFu);
    EXPECT_EQ(ecuio_operation::most_of(4), 0xFFFFFFFFu);
}

TEST(OperationPlanTest, EachNumberFitsWhatCarriesIt)
{
    const step r = read_step();
    const step u = upload_step();
    EXPECT_EQ(fault(r, [](step& s) { s.kind = 3; }),
              "its kind isn't one this Havne runs");
    EXPECT_EQ(fault(r, [](step& s) { s.kind = 0; }),
              "its kind isn't one this Havne runs");
    for (const uint32_t session : {0u, 1u, 4u}) {
        EXPECT_EQ(fault(r, [session](step& s) { s.session = session; }),
                  "its session isn't one this Havne reads in");
    }
    EXPECT_EQ(
        fault(r, [](step& s) { s.session = ECUIO_STEP_SESSION_PROGRAMMING; }),
        "");
    for (const uint8_t n : {uint8_t{0}, uint8_t{5}}) {
        EXPECT_EQ(fault(r, [n](step& s) { s.address_bytes = n; }),
                  "its address_bytes is 1 to 4");
        EXPECT_EQ(fault(r, [n](step& s) { s.size_bytes = n; }),
                  "its size_bytes is 1 to 4");
    }
    EXPECT_EQ(fault(r, [](step& s) { s.bytes = 0; }),
              "its bytes is 1 to the image's bound");
    EXPECT_EQ(fault(u, [](step& s) { s.bytes = ECUIO_DUMP_IMAGE_MAX + 1ull; }),
              "its bytes is 1 to the image's bound");
    EXPECT_EQ(fault(u,
                    [](step& s) {
                        s.address = 0;
                        s.bytes = ECUIO_DUMP_IMAGE_MAX;
                    }),
              "");

    /* The last address, not the one after it, fits the address's bytes */
    const auto ends_at = [](uint64_t address, uint64_t bytes, uint8_t n) {
        return [=](step& s) {
            s.address = address;
            s.bytes = bytes;
            s.address_bytes = n;
        };
    };
    EXPECT_EQ(fault(r, ends_at(0xFF00, 0x100, 2)), "");
    EXPECT_EQ(fault(r, ends_at(0xFF00, 0x101, 2)),
              "its last address doesn't fit its address_bytes");
    EXPECT_EQ(fault(r, ends_at(0xFFFFF000, 0x1000, 4)), "");
    EXPECT_EQ(fault(r, ends_at(0xFFFFF000, 0x1001, 4)),
              "its last address doesn't fit its address_bytes");
    EXPECT_EQ(fault(r, ends_at(0x100, 1, 1)),
              "its last address doesn't fit its address_bytes");
}

TEST(OperationPlanTest, AReadsChunkAndAnUploadsSizeFitTheSizeBytes)
{
    const step r = read_step();
    const step u = upload_step();
    EXPECT_EQ(fault(r, [](step& s) { s.chunk = 0; }),
              "a read's chunk is 1 to 4094");
    EXPECT_EQ(fault(r, [](step& s) { s.chunk = 4094; }), "");
    EXPECT_EQ(fault(r, [](step& s) { s.chunk = 4095; }),
              "a read's chunk is 1 to 4094");
    const auto sized = [](uint32_t chunk, uint8_t n) {
        return [=](step& s) {
            s.chunk = chunk;
            s.size_bytes = n;
        };
    };
    EXPECT_EQ(fault(r, sized(255, 1)), "");
    EXPECT_EQ(fault(r, sized(256, 1)), "its chunk doesn't fit its size_bytes");
    EXPECT_EQ(fault(r, [](step& s) { s.dfi = 1; }), "a read takes no dfi");
    /* A read's whole size needn't fit: only each request's does */
    EXPECT_EQ(fault(r,
                    [](step& s) {
                        s.bytes = 0x20000;
                        s.size_bytes = 1;
                        s.chunk = 255;
                    }),
              "");

    EXPECT_EQ(fault(u, [](step& s) { s.chunk = 1; }),
              "an upload takes no chunk: the module names its block");
    const auto whole = [](uint64_t bytes, uint8_t n) {
        return [=](step& s) {
            s.bytes = bytes;
            s.size_bytes = n;
        };
    };
    EXPECT_EQ(fault(u, whole(0xFFFF, 2)), "");
    EXPECT_EQ(fault(u, whole(0x10000, 2)),
              "its bytes doesn't fit its size_bytes");
    EXPECT_EQ(fault(u, [](step& s) { s.dfi = 0x10; }), "an upload's dfi is 00");
}

TEST(OperationPlanTest, TwoRangesOverlapOnlyWhenTheyShareAnAddress)
{
    const auto range = [](uint64_t address, uint64_t bytes) {
        step s = read_step();
        s.address = address;
        s.bytes = bytes;
        return s;
    };
    const step a = range(0x1000, 0x100);
    EXPECT_FALSE(ecuio_operation::overlap(a, range(0x1100, 0x100)));
    EXPECT_FALSE(ecuio_operation::overlap(range(0x1100, 0x100), a));
    EXPECT_FALSE(ecuio_operation::overlap(a, range(0xF00, 0x100)));
    EXPECT_TRUE(ecuio_operation::overlap(a, range(0x10FF, 1)));
    EXPECT_TRUE(ecuio_operation::overlap(range(0x10FF, 1), a));
    EXPECT_TRUE(ecuio_operation::overlap(a, range(0xF01, 0x100)));
    EXPECT_TRUE(ecuio_operation::overlap(a, range(0x800, 0x2000)));
    EXPECT_TRUE(ecuio_operation::overlap(a, a));
}

/* The class is the steps', never a word: one step of a kind that does
 * more than read makes the plan write-class, wherever it stands */
TEST(OperationPlanTest, TheClassIsDerivedFromTheSteps)
{
    plan p;
    EXPECT_EQ(ecuio_operation::class_of(p), ECUIO_OPERATION_CLASS_READ);
    p.steps = {read_step(), upload_step(), read_step()};
    EXPECT_EQ(ecuio_operation::class_of(p), ECUIO_OPERATION_CLASS_READ);
    EXPECT_EQ(ecuio_operation::image_bytes(p), 3u * 4096u);
    for (size_t at = 0; at < p.steps.size(); ++at) {
        plan q = p;
        q.steps[at].kind = 3; /* a kind a later Havne adds */
        EXPECT_EQ(ecuio_operation::class_of(q), ECUIO_OPERATION_CLASS_WRITE)
            << at;
    }
}

/* One unlock for each run of steps in one session: a change of session
 * locks the module again, and going back costs another */
TEST(OperationPlanTest, AnUnlockIsCountedForEachSessionEntered)
{
    const auto in = [](uint32_t session) {
        step s = read_step();
        s.session = session;
        return s;
    };
    const uint32_t e = ECUIO_STEP_SESSION_EXTENDED;
    const uint32_t g = ECUIO_STEP_SESSION_PROGRAMMING;
    plan p;
    EXPECT_EQ(ecuio_operation::unlock_sessions(p), 0u);
    p.steps = {in(e)};
    EXPECT_EQ(ecuio_operation::unlock_sessions(p), 1u);
    p.steps = {in(e), in(e), in(e)};
    EXPECT_EQ(ecuio_operation::unlock_sessions(p), 1u);
    p.steps = {in(e), in(g), in(g)};
    EXPECT_EQ(ecuio_operation::unlock_sessions(p), 2u);
    p.steps = {in(e), in(g), in(e)};
    EXPECT_EQ(ecuio_operation::unlock_sessions(p), 3u);
}

/* An ELM327 adapter sends one frame of 7 bytes: 23, the format byte, the
 * address and the size; an upload's 35 carries its data format too */
TEST(OperationPlanTest, ARequestFitsOneFrameUpToSevenBytes)
{
    const auto shaped = [](step s, uint8_t address_bytes, uint8_t size_bytes) {
        s.address_bytes = address_bytes;
        s.size_bytes = size_bytes;
        return ecuio_operation::fits_one_frame(s);
    };
    EXPECT_TRUE(shaped(read_step(), 4, 1));
    EXPECT_TRUE(shaped(read_step(), 3, 2));
    EXPECT_FALSE(shaped(read_step(), 4, 2));
    EXPECT_TRUE(shaped(upload_step(), 3, 1));
    EXPECT_TRUE(shaped(upload_step(), 2, 2));
    EXPECT_FALSE(shaped(upload_step(), 4, 1));
    EXPECT_FALSE(shaped(upload_step(), 3, 2));
}

} // namespace
