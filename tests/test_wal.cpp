// ============================================
// test_wal.cpp - WAL writer/reader round-trip + CRC corruption tests
// ============================================

#include "common/crc32.h"
#include "common/wal_writer.h"
#include "common/wal_reader.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#define TEST_ASSERT(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            throw std::runtime_error(std::string("ASSERT FAILED: ") + #cond +  \
                                     " at " + __FILE__ + ":" +                 \
                                     std::to_string(__LINE__));                \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_EQ(a, b)                                                  \
    do {                                                                       \
        if ((a) != (b)) {                                                      \
            throw std::runtime_error(std::string("ASSERT_EQ FAILED: ") +       \
                                     #a " != " #b " at " + __FILE__ + ":" +   \
                                     std::to_string(__LINE__));                \
        }                                                                      \
    } while (0)

namespace {

static const char* kTestWalPath = "test_wal_roundtrip.bin";
static const char* kTestWalCorruptPath = "test_wal_corrupt.bin";

struct TestPayload {
    int32_t id;
    double value;
    char name[16];
};

void cleanup_file(const char* path) {
    std::remove(path);
}

} // anonymous namespace

void test_crc32_basic() {
    const char* data = "hello";
    uint32_t crc = hft::CRC32::compute(data, 5);
    TEST_ASSERT(crc != 0);

    uint32_t crc2 = hft::CRC32::compute(data, 5);
    TEST_ASSERT_EQ(crc, crc2);

    const char* data2 = "hellx";
    uint32_t crc3 = hft::CRC32::compute(data2, 5);
    TEST_ASSERT(crc3 != crc);
}

void test_crc32_incremental() {
    const char* data = "hello world";
    uint32_t full = hft::CRC32::compute(data, 11);
    uint32_t partial = hft::CRC32::compute(data, 5);
    uint32_t inc = hft::CRC32::update(partial, data + 5, 6);
    TEST_ASSERT_EQ(full, inc);
}

void test_wal_roundtrip() {
    cleanup_file(kTestWalPath);

    TestPayload p1{1, 3.14, "alpha"};
    TestPayload p2{2, 2.72, "beta"};
    TestPayload p3{3, 1.41, "gamma"};

    {
        hft::WalWriter writer;
        TEST_ASSERT(writer.open(kTestWalPath));
        TEST_ASSERT(writer.append(hft::WalEntryType::OrderUpdate, p1));
        TEST_ASSERT(writer.append(hft::WalEntryType::TradeRecord, p2));
        TEST_ASSERT(writer.append(hft::WalEntryType::PositionSnap, p3));
        TEST_ASSERT(writer.write_checkpoint());
        writer.close();
    }

    std::vector<hft::WalEntry> entries;
    size_t read_count = 0, corrupt_count = 0;
    auto result = hft::WalReader::replay(kTestWalPath,
        [&](const hft::WalEntry& e) { entries.push_back(e); },
        &read_count, &corrupt_count);

    TEST_ASSERT(result == hft::WalReader::ReplayResult::Ok);
    TEST_ASSERT_EQ(read_count, size_t(4));
    TEST_ASSERT_EQ(corrupt_count, size_t(0));
    TEST_ASSERT_EQ(entries.size(), size_t(4));

    TEST_ASSERT(entries[0].type == hft::WalEntryType::OrderUpdate);
    TEST_ASSERT(entries[1].type == hft::WalEntryType::TradeRecord);
    TEST_ASSERT(entries[2].type == hft::WalEntryType::PositionSnap);
    TEST_ASSERT(entries[3].type == hft::WalEntryType::Checkpoint);

    TestPayload out1{};
    TEST_ASSERT(hft::WalReader::extract(entries[0], out1));
    TEST_ASSERT_EQ(out1.id, 1);

    TestPayload out2{};
    TEST_ASSERT(hft::WalReader::extract(entries[1], out2));
    TEST_ASSERT_EQ(out2.id, 2);

    TEST_ASSERT(entries[3].payload.empty());

    cleanup_file(kTestWalPath);
}

void test_wal_crc_corruption() {
    cleanup_file(kTestWalCorruptPath);

    TestPayload p{42, 9.81, "test"};
    {
        hft::WalWriter writer;
        TEST_ASSERT(writer.open(kTestWalCorruptPath));
        TEST_ASSERT(writer.append(hft::WalEntryType::OrderUpdate, p));
        TEST_ASSERT(writer.append(hft::WalEntryType::TradeRecord, p));
        writer.close();
    }

    // Corrupt middle of file
    FILE* fp = nullptr;
#ifdef _WIN32
    fopen_s(&fp, kTestWalCorruptPath, "r+b");
#else
    fp = std::fopen(kTestWalCorruptPath, "r+b");
#endif
    TEST_ASSERT(fp != nullptr);
    // Flip a byte in the first entry's payload area (after header)
    std::fseek(fp, static_cast<long>(hft::kWalHeaderSize + 2), SEEK_SET);
    uint8_t byte = 0xFF;
    std::fwrite(&byte, 1, 1, fp);
    std::fclose(fp);

    size_t read_count = 0, corrupt_count = 0;
    auto result = hft::WalReader::replay(kTestWalCorruptPath,
        [](const hft::WalEntry&) {},
        &read_count, &corrupt_count);

    // First entry corrupted, second should still read
    TEST_ASSERT(corrupt_count >= 1);
    TEST_ASSERT(read_count >= 1);

    cleanup_file(kTestWalCorruptPath);
}

void test_wal_file_not_found() {
    auto result = hft::WalReader::replay("nonexistent_wal_file.bin",
        [](const hft::WalEntry&) {});
    TEST_ASSERT(result == hft::WalReader::ReplayResult::FileNotFound);
}

void test_wal_empty_payload() {
    cleanup_file(kTestWalPath);

    {
        hft::WalWriter writer;
        TEST_ASSERT(writer.open(kTestWalPath));
        TEST_ASSERT(writer.write_checkpoint());
        TEST_ASSERT(writer.write_checkpoint());
        writer.close();
    }

    size_t read_count = 0;
    auto result = hft::WalReader::replay(kTestWalPath,
        [](const hft::WalEntry& e) {
            TEST_ASSERT(e.type == hft::WalEntryType::Checkpoint);
            TEST_ASSERT(e.payload.empty());
        },
        &read_count, nullptr);

    TEST_ASSERT(result == hft::WalReader::ReplayResult::Ok);
    TEST_ASSERT_EQ(read_count, size_t(2));

    cleanup_file(kTestWalPath);
}
