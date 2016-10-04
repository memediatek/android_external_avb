/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <string.h>

#include <gtest/gtest.h>

#include "avb_unittest_util.h"
#include "fake_avb_ops.h"
#include "libavb.h"

static_assert(sizeof(AvbABSlotData) == 4, "AvbABSlotData has wrong size");
static_assert(sizeof(AvbABData) == AVB_AB_DATA_SIZE,
              "AvbABData has wrong size");
static_assert(offsetof(AvbABData, slots) % 8 == 0,
              "AvbABData slots member has wrong offset");

TEST(ABTest, InitData) {
  AvbABData data;
  avb_ab_data_init(&data);
  EXPECT_EQ(0, strncmp(reinterpret_cast<const char*>(data.magic), AVB_AB_MAGIC,
                       AVB_AB_MAGIC_LEN));
  EXPECT_EQ(AVB_AB_MAX_PRIORITY, data.slots[0].priority);
  EXPECT_EQ(AVB_AB_MAX_TRIES_REMAINING, data.slots[0].tries_remaining);
  EXPECT_EQ(0, data.slots[0].successful_boot);
  EXPECT_EQ(AVB_AB_MAX_PRIORITY - 1, data.slots[1].priority);
  EXPECT_EQ(AVB_AB_MAX_TRIES_REMAINING, data.slots[1].tries_remaining);
  EXPECT_EQ(0, data.slots[1].successful_boot);
  EXPECT_EQ(uint32_t(0), data.crc32);
}

TEST(ABTest, DataSerialization) {
  AvbABData data;
  AvbABData serialized;
  AvbABData restored;

  avb_ab_data_init(&data);
  EXPECT_EQ(uint32_t(0), data.crc32);
  avb_ab_data_update_crc_and_byteswap(&data, &serialized);
  EXPECT_NE(uint32_t(0), serialized.crc32);
  EXPECT_TRUE(avb_ab_data_verify_and_byteswap(&serialized, &restored));
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(data.magic), 4),
            std::string(reinterpret_cast<const char*>(restored.magic), 4));
  EXPECT_EQ(data.version_major, restored.version_major);
  EXPECT_EQ(data.version_minor, restored.version_minor);
  EXPECT_EQ(0, memcmp(reinterpret_cast<void*>(data.slots),
                      reinterpret_cast<void*>(restored.slots),
                      sizeof(AvbABSlotData) * 2));
}

TEST(ABTest, CatchBadCRC) {
  AvbABData data;
  AvbABData serialized;
  AvbABData restored;

  avb_ab_data_init(&data);
  avb_ab_data_update_crc_and_byteswap(&data, &serialized);
  serialized.crc32 += 1;
  EXPECT_FALSE(avb_ab_data_verify_and_byteswap(&serialized, &restored));
}

TEST(ABTest, CatchUnsupportedMajorVersion) {
  AvbABData data;
  AvbABData serialized;
  AvbABData restored;

  avb_ab_data_init(&data);
  data.version_major += 1;
  avb_ab_data_update_crc_and_byteswap(&data, &serialized);
  EXPECT_FALSE(avb_ab_data_verify_and_byteswap(&serialized, &restored));
}

TEST(ABTest, SupportSameMajorFutureMinorVersion) {
  AvbABData data;
  AvbABData serialized;
  AvbABData restored;

  avb_ab_data_init(&data);
  data.version_minor += 1;
  avb_ab_data_update_crc_and_byteswap(&data, &serialized);
  EXPECT_TRUE(avb_ab_data_verify_and_byteswap(&serialized, &restored));
}

class AvbABFlowTest : public BaseAvbToolTest {
 public:
  AvbABFlowTest() {}

  virtual void SetUp() override {
    BaseAvbToolTest::SetUp();
    ops_.set_partition_dir(testdir_);
    ops_.set_stored_rollback_indexes({0, 0, 0, 0});
    ops_.set_stored_is_device_unlocked(false);

    // Create large enough 'misc' partition and initialize it with
    // zeroes.
    std::vector<uint8_t> misc;
    misc.resize(8 * 1024);
    base::FilePath misc_path = testdir_.Append("misc.img");
    EXPECT_EQ(misc.size(),
              static_cast<const size_t>(base::WriteFile(
                  misc_path, reinterpret_cast<const char*>(misc.data()),
                  misc.size())));

    // We're going to use this key for all images.
    ops_.set_expected_public_key(
        PublicKeyAVB(base::FilePath("test/data/testkey_rsa2048.pem")));
  }

  void GenerateSlot(unsigned int slot_number, bool make_valid,
                    uint64_t rollback_boot, uint64_t rollback_odm) {
    std::string boot_name = "boot_a.img";
    std::string vbmeta_name = "vbmeta_a.img";
    std::string odm_name = "odm_a.img";
    if (slot_number > 0) {
      boot_name = "boot_b.img";
      vbmeta_name = "vbmeta_b.img";
      odm_name = "odm_b.img";
    }

    if (!make_valid) {
      // If asked to make an invalid slot, just generate 1MiB garbage
      // for each the three images in the slot.
      GenerateImage(boot_name, 1024 * 1024);
      GenerateImage(vbmeta_name, 1024 * 1024);
      GenerateImage(odm_name, 1024 * 1024);
      return;
    }

    const size_t boot_partition_size = 16 * 1024 * 1024;
    const size_t boot_image_size = 5 * 1024 * 1024;
    base::FilePath boot_path = GenerateImage(boot_name, boot_image_size);
    EXPECT_COMMAND(0,
                   "./avbtool add_hash_footer"
                   " --image %s"
                   " --rollback_index %" PRIu64
                   " --partition_name boot"
                   " --partition_size %zd"
                   " --salt deadbeef",
                   boot_path.value().c_str(), rollback_boot,
                   boot_partition_size);

    const size_t odm_partition_size = 512 * 1024;
    const size_t odm_image_size = 80 * 1024;
    base::FilePath odm_path = GenerateImage(odm_name, odm_image_size);
    EXPECT_COMMAND(0,
                   "./avbtool add_hashtree_footer"
                   " --image %s"
                   " --rollback_index %" PRIu64
                   " --partition_name odm"
                   " --partition_size %zd"
                   " --salt deadbeef"
                   " --algorithm SHA512_RSA4096 "
                   " --key test/data/testkey_rsa4096.pem",
                   odm_path.value().c_str(), rollback_odm, odm_partition_size);

    base::FilePath pk_path = testdir_.Append("testkey_rsa4096.avbpubkey");
    EXPECT_COMMAND(
        0,
        "./avbtool extract_public_key --key test/data/testkey_rsa4096.pem"
        " --output %s",
        pk_path.value().c_str());

    GenerateVBMetaImage(
        vbmeta_name, "SHA256_RSA2048", rollback_boot,
        base::FilePath("test/data/testkey_rsa2048.pem"),
        base::StringPrintf("--include_descriptors_from_image %s"
                           " --chain_partition odm:1:%s",
                           boot_path.value().c_str(), pk_path.value().c_str()));
  }

  void SetMD(int a_pri, int a_tries, bool a_success, bool a_slot_valid,
             uint64_t a_rollback_boot, uint64_t a_rollback_odm, int b_pri,
             int b_tries, bool b_success, bool b_slot_valid,
             uint64_t b_rollback_boot, uint64_t b_rollback_odm,
             const std::vector<uint64_t>& stored_rollback_indexes) {
    AvbABData data;
    avb_ab_data_init(&data);
    data.slots[0].priority = a_pri;
    data.slots[0].tries_remaining = a_tries;
    data.slots[0].successful_boot = (a_success ? 1 : 0);
    data.slots[1].priority = b_pri;
    data.slots[1].tries_remaining = b_tries;
    data.slots[1].successful_boot = (b_success ? 1 : 0);
    EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_data_write(ops_.avb_ops(), &data));
    GenerateSlot(0, a_slot_valid, a_rollback_boot, a_rollback_odm);
    GenerateSlot(1, b_slot_valid, b_rollback_boot, b_rollback_odm);
    ops_.set_stored_rollback_indexes(stored_rollback_indexes);
  }

  FakeAvbOps ops_;
};

#define ExpMD(a_pri, a_tries, a_success, b_pri, b_tries, b_success,       \
              stored_rollback_indexes)                                    \
  do {                                                                    \
    AvbABData data;                                                       \
    EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_data_read(ops_.avb_ops(), &data)); \
    EXPECT_EQ(a_pri, data.slots[0].priority);                             \
    EXPECT_EQ(a_tries, data.slots[0].tries_remaining);                    \
    EXPECT_EQ(a_success ? 1 : 0, data.slots[0].successful_boot);          \
    EXPECT_EQ(b_pri, data.slots[1].priority);                             \
    EXPECT_EQ(b_tries, data.slots[1].tries_remaining);                    \
    EXPECT_EQ(b_success ? 1 : 0, data.slots[1].successful_boot);          \
    EXPECT_EQ(std::vector<uint64_t>(stored_rollback_indexes),             \
              ops_.get_stored_rollback_indexes());                        \
  } while (0);

TEST_F(AvbABFlowTest, MetadataReadAndWrite) {
  AvbABData data;
  AvbABData loaded;

  // First load from an uninitialized 'misc' partition. This should
  // not fail and just returned initialized data.
  EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_data_read(ops_.avb_ops(), &loaded));
  EXPECT_EQ(AVB_AB_MAX_PRIORITY, loaded.slots[0].priority);
  EXPECT_EQ(AVB_AB_MAX_TRIES_REMAINING, loaded.slots[0].tries_remaining);
  EXPECT_EQ(0, loaded.slots[0].successful_boot);
  EXPECT_EQ(AVB_AB_MAX_PRIORITY - 1, loaded.slots[1].priority);
  EXPECT_EQ(AVB_AB_MAX_TRIES_REMAINING, loaded.slots[1].tries_remaining);
  EXPECT_EQ(0, loaded.slots[1].successful_boot);

  // Then initialize and save well-known A/B metadata and check we
  // read back the same thing.
  avb_ab_data_init(&data);
  data.slots[0].priority = 2;
  data.slots[0].tries_remaining = 3;
  EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_data_write(ops_.avb_ops(), &data));
  EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_data_read(ops_.avb_ops(), &loaded));
  EXPECT_EQ(2, loaded.slots[0].priority);
  EXPECT_EQ(3, loaded.slots[0].tries_remaining);
}

TEST_F(AvbABFlowTest, EverythingIsValid) {
  AvbSlotVerifyData* data;

  SetMD(14, 0, 1, true, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        15, 0, 1, true, 0, 0,  // B: pri, tries, success, slot_valid, RIs
        {0, 0});               // stored_rollback_indexes
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(14, 0, 1,                        // A: pri, tries, successful
        15, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_b", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);

  // Also check the other slot.
  SetMD(15, 0, 1, true, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, true, 0, 0,  // B: pri, tries, success, slot_valid, RIs
        {0, 0});               // stored_rollback_indexes
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(15, 0, 1,                        // A: pri, tries, successful
        14, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_a", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);
}

TEST_F(AvbABFlowTest, NoBootableSlots) {
  AvbSlotVerifyData* data;
  SetMD(0, 0, 0, true, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        0, 0, 0, true, 0, 0,  // B: pri, tries, success, slot_valid, RIs
        {0, 0});              // stored_rollback_indexes
  EXPECT_EQ(AVB_AB_FLOW_RESULT_ERROR_NO_BOOTABLE_SLOTS,
            avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(0, 0, 0,                         // A: pri, tries, successful
        0, 0, 0,                         // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_EQ(nullptr, data);
}

TEST_F(AvbABFlowTest, TriesRemainingDecreasing) {
  AvbSlotVerifyData* data;

  SetMD(15, 3, 0, true, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        0, 0, 0, true, 0, 0,   // B: pri, tries, success, slot_valid, RIs
        {0, 0});               // stored_rollback_indexes

  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(15, 2, 0,                        // A: pri, tries, successful
        0, 0, 0,                         // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_a", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);

  // Keep counting down...
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(15, 1, 0,                        // A: pri, tries, successful
        0, 0, 0,                         // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_a", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);

  // Last try...
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(15, 0, 0,                        // A: pri, tries, successful
        0, 0, 0,                         // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_a", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);

  // And we're out of tries. At this point, (15, 0, 0) is normalized
  // to (0, 0, 0) so expect that.
  EXPECT_EQ(AVB_AB_FLOW_RESULT_ERROR_NO_BOOTABLE_SLOTS,
            avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(0, 0, 0,                         // A: pri, tries, successful
        0, 0, 0,                         // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_EQ(nullptr, data);
}

TEST_F(AvbABFlowTest, TryingThenFallback) {
  AvbSlotVerifyData* data;

  SetMD(15, 2, 0, true, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, true, 0, 0,  // B: pri, tries, success, slot_valid, RIs
        {0, 0});               // stored_rollback_indexes
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(15, 1, 0,                        // A: pri, tries, successful
        14, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_a", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);

  // Last try...
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(15, 0, 0,                        // A: pri, tries, successful
        14, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_a", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);

  // And we're out of tries. Check we fall back to slot B.
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(0, 0, 0,                         // A: pri, tries, successful
        14, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_b", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);
}

TEST_F(AvbABFlowTest, TriesRemainingNotDecreasingIfNotPriority) {
  AvbSlotVerifyData* data;

  SetMD(15, 0, 1, true, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        14, 7, 0, true, 0, 0,  // B: pri, tries, success, slot_valid, RIs
        {0, 0});               // stored_rollback_indexes
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(15, 0, 1,                        // A: pri, tries, successful
        14, 7, 0,                        // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_a", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);
}

TEST_F(AvbABFlowTest, InvalidSlotIsMarkedAsSuch) {
  AvbSlotVerifyData* data;

  // Slot A fails verification.
  SetMD(15, 0, 1, false, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, true, 0, 0,   // B: pri, tries, success, slot_valid, RIs
        {0, 0});                // stored_rollback_indexes
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(0, 0, 0,                         // A: pri, tries, successful
        14, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_b", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);

  // Slot B fails verification.
  SetMD(15, 0, 1, true, 0, 0,   // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, false, 0, 0,  // B: pri, tries, success, slot_valid, RIs
        {0, 0});                // stored_rollback_indexes
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(15, 0, 1,                        // A: pri, tries, successful
        0, 0, 0,                         // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_a", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);

  // Both slots fail verification.
  SetMD(15, 0, 1, false, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, false, 0, 0,  // B: pri, tries, success, slot_valid, RIs
        {0, 0});                // stored_rollback_indexes
  EXPECT_EQ(AVB_AB_FLOW_RESULT_ERROR_NO_BOOTABLE_SLOTS,
            avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(0, 0, 0,                         // A: pri, tries, successful
        0, 0, 0,                         // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
  ASSERT_EQ(nullptr, data);
}

TEST_F(AvbABFlowTest, RollbackIndexFailures) {
  AvbSlotVerifyData* data;

  // Slot A rollback index failure for 'boot'.
  SetMD(15, 0, 1, true, 0, 2,  // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, true, 2, 2,  // B: pri, tries, success, slot_valid, RIs
        {2, 2});               // stored_rollback_indexes
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(0, 0, 0,                         // A: pri, tries, successful
        14, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({2, 2}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_b", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);

  // Slot A rollback index failure for 'odm'.
  SetMD(15, 0, 1, true, 2, 0,  // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, true, 2, 2,  // B: pri, tries, success, slot_valid, RIs
        {2, 2});               // stored_rollback_indexes
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(0, 0, 0,                         // A: pri, tries, successful
        14, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({2, 2}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_b", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);
}

TEST_F(AvbABFlowTest, StoredRollbackIndexBumped) {
  AvbSlotVerifyData* data;

  SetMD(15, 0, 1, true, 3, 3,  // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, true, 3, 3,  // B: pri, tries, success, slot_valid, RIs
        {2, 2});               // stored_rollback_indexes
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(15, 0, 1,                        // A: pri, tries, successful
        14, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({3, 3}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_a", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);

  // The case where different partitions have different rollback
  // index values.
  SetMD(15, 0, 1, true, 4, 9,  // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, true, 5, 7,  // B: pri, tries, success, slot_valid, RIs
        {0, 0});               // stored_rollback_indexes
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(15, 0, 1,                        // A: pri, tries, successful
        14, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({4, 7}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_a", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);

  // If the slot with the low RI fails verification, check that these
  // low Rollback Indexs are not taken into account after marking it
  // as unbootable.
  SetMD(15, 0, 1, false, 4, 9,  // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, true, 5, 7,   // B: pri, tries, success, slot_valid, RIs
        {0, 0});                // stored_rollback_indexes
  EXPECT_EQ(AVB_AB_FLOW_RESULT_OK, avb_ab_flow(ops_.avb_ops(), &data));
  ExpMD(0, 0, 0,                         // A: pri, tries, successful
        14, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({5, 7}));  // stored_rollback_indexes
  ASSERT_NE(nullptr, data);
  EXPECT_EQ("_b", std::string(data->ab_suffix));
  avb_slot_verify_data_free(data);
}

TEST_F(AvbABFlowTest, MarkSlotActive) {
  SetMD(15, 0, 1, false, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        11, 0, 1, true, 0, 0,   // B: pri, tries, success, slot_valid, RIs
        {0, 0});                // stored_rollback_indexes
  EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_mark_slot_active(ops_.avb_ops(), 0));
  ExpMD(15, 7, 0,                        // A: pri, tries, successful
        11, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes

  // Note how priority of slot A is altered to make room for newly
  // activated slot.
  SetMD(15, 0, 1, false, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, true, 0, 0,   // B: pri, tries, success, slot_valid, RIs
        {0, 0});                // stored_rollback_indexes
  EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_mark_slot_active(ops_.avb_ops(), 1));
  ExpMD(14, 0, 1,                        // A: pri, tries, successful
        15, 7, 0,                        // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
}

TEST_F(AvbABFlowTest, MarkSlotUnbootable) {
  SetMD(15, 0, 1, false, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        11, 0, 1, true, 0, 0,   // B: pri, tries, success, slot_valid, RIs
        {0, 0});                // stored_rollback_indexes
  EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_mark_slot_unbootable(ops_.avb_ops(), 0));
  ExpMD(0, 0, 0,                         // A: pri, tries, successful
        11, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes

  SetMD(15, 0, 1, false, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, true, 0, 0,   // B: pri, tries, success, slot_valid, RIs
        {0, 0});                // stored_rollback_indexes
  EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_mark_slot_unbootable(ops_.avb_ops(), 1));
  ExpMD(15, 0, 1,                        // A: pri, tries, successful
        0, 0, 0,                         // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
}

TEST_F(AvbABFlowTest, MarkSlotSuccessful) {
  SetMD(15, 5, 0, false, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        11, 3, 0, true, 0, 0,   // B: pri, tries, success, slot_valid, RIs
        {0, 0});                // stored_rollback_indexes
  EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_mark_slot_successful(ops_.avb_ops(), 0));
  ExpMD(15, 0, 1,                        // A: pri, tries, successful
        11, 3, 0,                        // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes

  SetMD(15, 5, 0, false, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, true, 0, 0,   // B: pri, tries, success, slot_valid, RIs
        {0, 0});                // stored_rollback_indexes
  EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_mark_slot_successful(ops_.avb_ops(), 1));
  ExpMD(15, 5, 0,                        // A: pri, tries, successful
        14, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes

  // Marking an unbootable slot (A) as successful won't work (it's a
  // programmer error to do so)... notice however that the unbootable
  // slot is normalized in the process.
  SetMD(0, 3, 2, false, 0, 0,  // A: pri, tries, success, slot_valid, RIs
        14, 0, 1, true, 0, 0,  // B: pri, tries, success, slot_valid, RIs
        {0, 0});               // stored_rollback_indexes
  EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_mark_slot_successful(ops_.avb_ops(), 0));
  ExpMD(0, 0, 0,                         // A: pri, tries, successful
        14, 0, 1,                        // B: pri, tries, successful
        std::vector<uint64_t>({0, 0}));  // stored_rollback_indexes
}

TEST_F(AvbABFlowTest, AvbtoolMetadataGeneratorEmptyFile) {
  AvbABData data;

  base::FilePath misc_path = testdir_.Append("misc.img");
  EXPECT_COMMAND(0,
                 "./avbtool set_ab_metadata"
                 " --misc_image %s"
                 " --slot_data 13:3:0:11:2:1",
                 misc_path.value().c_str());

  EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_data_read(ops_.avb_ops(), &data));
  EXPECT_EQ(13, data.slots[0].priority);
  EXPECT_EQ(3, data.slots[0].tries_remaining);
  EXPECT_EQ(0, data.slots[0].successful_boot);
  EXPECT_EQ(11, data.slots[1].priority);
  EXPECT_EQ(2, data.slots[1].tries_remaining);
  EXPECT_EQ(1, data.slots[1].successful_boot);
}

TEST_F(AvbABFlowTest, AvbtoolMetadataGeneratorExistingFile) {
  AvbABData data;
  size_t n;

  size_t misc_size = 1024 * 1024;
  base::FilePath misc_path = GenerateImage("misc.img", misc_size);
  EXPECT_COMMAND(0,
                 "./avbtool set_ab_metadata"
                 " --misc_image %s"
                 " --slot_data 12:2:1:10:5:0",
                 misc_path.value().c_str());

  EXPECT_EQ(AVB_IO_RESULT_OK, avb_ab_data_read(ops_.avb_ops(), &data));
  EXPECT_EQ(12, data.slots[0].priority);
  EXPECT_EQ(2, data.slots[0].tries_remaining);
  EXPECT_EQ(1, data.slots[0].successful_boot);
  EXPECT_EQ(10, data.slots[1].priority);
  EXPECT_EQ(5, data.slots[1].tries_remaining);
  EXPECT_EQ(0, data.slots[1].successful_boot);

  std::string misc_data;
  ASSERT_TRUE(base::ReadFileToString(misc_path, &misc_data));
  EXPECT_EQ(misc_size, misc_data.size());
  for (n = 0; n < 2048; n++) {
    ASSERT_EQ(uint8_t(misc_data[n]), uint8_t(n));
  }
  for (n = 2048 + 32; n < misc_data.size(); n++) {
    ASSERT_EQ(uint8_t(misc_data[n]), uint8_t(n));
  }
}
