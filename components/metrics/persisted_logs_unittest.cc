// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/metrics/persisted_logs.h"

#include <stddef.h>

#include "base/base64.h"
#include "base/hash/sha1.h"
#include "base/macros.h"
#include "base/rand_util.h"
#include "base/values.h"
#include "components/metrics/persisted_logs_metrics_impl.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/zlib/google/compression_utils.h"

namespace metrics {

namespace {

const char kTestPrefName[] = "TestPref";
const size_t kLogCountLimit = 3;
const size_t kLogByteLimit = 1000;

// Compresses |log_data| and returns the result.
std::string Compress(const std::string& log_data) {
  std::string compressed_log_data;
  EXPECT_TRUE(compression::GzipCompress(log_data, &compressed_log_data));
  return compressed_log_data;
}

// Generates and returns log data such that its size after compression is at
// least |min_compressed_size|.
std::string GenerateLogWithMinCompressedSize(size_t min_compressed_size) {
  // Since the size check is done against a compressed log, generate enough
  // data that compresses to larger than |log_size|.
  std::string rand_bytes = base::RandBytesAsString(min_compressed_size);
  while (Compress(rand_bytes).size() < min_compressed_size)
    rand_bytes.append(base::RandBytesAsString(min_compressed_size));
  std::string base64_data_for_logging;
  base::Base64Encode(rand_bytes, &base64_data_for_logging);
  SCOPED_TRACE(testing::Message() << "Using random data "
                                  << base64_data_for_logging);
  return rand_bytes;
}

class PersistedLogsTest : public testing::Test {
 public:
  PersistedLogsTest() {
    prefs_.registry()->RegisterListPref(kTestPrefName);
  }

 protected:
  TestingPrefServiceSimple prefs_;

 private:
  DISALLOW_COPY_AND_ASSIGN(PersistedLogsTest);
};

class TestPersistedLogs : public PersistedLogs {
 public:
  TestPersistedLogs(PrefService* service, size_t min_log_bytes)
      : PersistedLogs(std::unique_ptr<PersistedLogsMetricsImpl>(
                          new PersistedLogsMetricsImpl()),
                      service,
                      kTestPrefName,
                      kLogCountLimit,
                      min_log_bytes,
                      0,
                      std::string()) {}
  TestPersistedLogs(PrefService* service,
                    size_t min_log_bytes,
                    const std::string& signing_key)
      : PersistedLogs(std::unique_ptr<PersistedLogsMetricsImpl>(
                          new PersistedLogsMetricsImpl()),
                      service,
                      kTestPrefName,
                      kLogCountLimit,
                      min_log_bytes,
                      0,
                      signing_key) {}

  // Stages and removes the next log, while testing it's value.
  void ExpectNextLog(const std::string& expected_log) {
    StageNextLog();
    EXPECT_EQ(staged_log(), Compress(expected_log));
    DiscardStagedLog();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(TestPersistedLogs);
};

}  // namespace

// Store and retrieve empty list_value.
TEST_F(PersistedLogsTest, EmptyLogList) {
  TestPersistedLogs persisted_logs(&prefs_, kLogByteLimit);

  persisted_logs.PersistUnsentLogs();
  const base::ListValue* list_value = prefs_.GetList(kTestPrefName);
  EXPECT_EQ(0U, list_value->GetSize());

  TestPersistedLogs result_persisted_logs(&prefs_, kLogByteLimit);
  result_persisted_logs.LoadPersistedUnsentLogs();
  EXPECT_EQ(0U, result_persisted_logs.size());
}

// Store and retrieve a single log value.
TEST_F(PersistedLogsTest, SingleElementLogList) {
  TestPersistedLogs persisted_logs(&prefs_, kLogByteLimit);

  persisted_logs.StoreLog("Hello world!");
  persisted_logs.PersistUnsentLogs();

  TestPersistedLogs result_persisted_logs(&prefs_, kLogByteLimit);
  result_persisted_logs.LoadPersistedUnsentLogs();
  EXPECT_EQ(1U, result_persisted_logs.size());

  // Verify that the result log matches the initial log.
  persisted_logs.StageNextLog();
  result_persisted_logs.StageNextLog();
  EXPECT_EQ(persisted_logs.staged_log(), result_persisted_logs.staged_log());
  EXPECT_EQ(persisted_logs.staged_log_hash(),
            result_persisted_logs.staged_log_hash());
  EXPECT_EQ(persisted_logs.staged_log_signature(),
            result_persisted_logs.staged_log_signature());
  EXPECT_EQ(persisted_logs.staged_log_timestamp(),
            result_persisted_logs.staged_log_timestamp());
}

// Store a set of logs over the length limit, but smaller than the min number of
// bytes.
TEST_F(PersistedLogsTest, LongButTinyLogList) {
  TestPersistedLogs persisted_logs(&prefs_, kLogByteLimit);

  size_t log_count = kLogCountLimit * 5;
  for (size_t i = 0; i < log_count; ++i)
    persisted_logs.StoreLog("x");

  persisted_logs.PersistUnsentLogs();

  TestPersistedLogs result_persisted_logs(&prefs_, kLogByteLimit);
  result_persisted_logs.LoadPersistedUnsentLogs();
  EXPECT_EQ(persisted_logs.size(), result_persisted_logs.size());

  result_persisted_logs.ExpectNextLog("x");
}

// Store a set of logs over the length limit, but that doesn't reach the minimum
// number of bytes until after passing the length limit.
TEST_F(PersistedLogsTest, LongButSmallLogList) {
  size_t log_count = kLogCountLimit * 5;
  size_t log_size = 50;

  std::string first_kept = "First to keep";
  first_kept.resize(log_size, ' ');

  std::string blank_log = std::string(log_size, ' ');

  std::string last_kept = "Last to keep";
  last_kept.resize(log_size, ' ');

  // Set the byte limit enough to keep everything but the first two logs.
  const size_t min_log_bytes =
      Compress(first_kept).length() + Compress(last_kept).length() +
      (log_count - 4) * Compress(blank_log).length();
  TestPersistedLogs persisted_logs(&prefs_, min_log_bytes);

  persisted_logs.StoreLog("one");
  persisted_logs.StoreLog("two");
  persisted_logs.StoreLog(first_kept);
  for (size_t i = persisted_logs.size(); i < log_count - 1; ++i) {
    persisted_logs.StoreLog(blank_log);
  }
  persisted_logs.StoreLog(last_kept);
  persisted_logs.PersistUnsentLogs();

  TestPersistedLogs result_persisted_logs(&prefs_, kLogByteLimit);
  result_persisted_logs.LoadPersistedUnsentLogs();
  EXPECT_EQ(persisted_logs.size() - 2, result_persisted_logs.size());

  result_persisted_logs.ExpectNextLog(last_kept);
  while (result_persisted_logs.size() > 1) {
    result_persisted_logs.ExpectNextLog(blank_log);
  }
  result_persisted_logs.ExpectNextLog(first_kept);
}

// Store a set of logs within the length limit, but well over the minimum
// number of bytes.
TEST_F(PersistedLogsTest, ShortButLargeLogList) {
  // Make the total byte count about twice the minimum.
  size_t log_count = kLogCountLimit;
  size_t log_size = (kLogByteLimit / log_count) * 2;
  std::string log_data = GenerateLogWithMinCompressedSize(log_size);

  TestPersistedLogs persisted_logs(&prefs_, kLogByteLimit);
  for (size_t i = 0; i < log_count; ++i) {
    persisted_logs.StoreLog(log_data);
  }
  persisted_logs.PersistUnsentLogs();

  TestPersistedLogs result_persisted_logs(&prefs_, kLogByteLimit);
  result_persisted_logs.LoadPersistedUnsentLogs();
  EXPECT_EQ(persisted_logs.size(), result_persisted_logs.size());
}

// Store a set of logs over the length limit, and over the minimum number of
// bytes.
TEST_F(PersistedLogsTest, LongAndLargeLogList) {
  TestPersistedLogs persisted_logs(&prefs_, kLogByteLimit);

  // Include twice the max number of logs.
  size_t log_count = kLogCountLimit * 2;
  // Make the total byte count about four times the minimum.
  size_t log_size = (kLogByteLimit / log_count) * 4;

  std::string target_log = "First to keep";
  target_log += GenerateLogWithMinCompressedSize(log_size);

  std::string log_data = GenerateLogWithMinCompressedSize(log_size);
  for (size_t i = 0; i < log_count; ++i) {
    if (i == log_count - kLogCountLimit)
      persisted_logs.StoreLog(target_log);
    else
      persisted_logs.StoreLog(log_data);
  }

  persisted_logs.PersistUnsentLogs();

  TestPersistedLogs result_persisted_logs(&prefs_, kLogByteLimit);
  result_persisted_logs.LoadPersistedUnsentLogs();
  EXPECT_EQ(kLogCountLimit, result_persisted_logs.size());

  while (result_persisted_logs.size() > 1) {
    result_persisted_logs.ExpectNextLog(log_data);
  }
  result_persisted_logs.ExpectNextLog(target_log);
}

// Check that the store/stage/discard functions work as expected.
TEST_F(PersistedLogsTest, Staging) {
  TestPersistedLogs persisted_logs(&prefs_, kLogByteLimit);
  std::string tmp;

  EXPECT_FALSE(persisted_logs.has_staged_log());
  persisted_logs.StoreLog("one");
  EXPECT_FALSE(persisted_logs.has_staged_log());
  persisted_logs.StoreLog("two");
  persisted_logs.StageNextLog();
  EXPECT_TRUE(persisted_logs.has_staged_log());
  EXPECT_EQ(persisted_logs.staged_log(), Compress("two"));
  persisted_logs.StoreLog("three");
  EXPECT_EQ(persisted_logs.staged_log(), Compress("two"));
  EXPECT_EQ(persisted_logs.size(), 3U);
  persisted_logs.DiscardStagedLog();
  EXPECT_FALSE(persisted_logs.has_staged_log());
  EXPECT_EQ(persisted_logs.size(), 2U);
  persisted_logs.StageNextLog();
  EXPECT_EQ(persisted_logs.staged_log(), Compress("three"));
  persisted_logs.DiscardStagedLog();
  persisted_logs.StageNextLog();
  EXPECT_EQ(persisted_logs.staged_log(), Compress("one"));
  persisted_logs.DiscardStagedLog();
  EXPECT_FALSE(persisted_logs.has_staged_log());
  EXPECT_EQ(persisted_logs.size(), 0U);
}

TEST_F(PersistedLogsTest, DiscardOrder) {
  // Ensure that the correct log is discarded if new logs are pushed while
  // a log is staged.
  TestPersistedLogs persisted_logs(&prefs_, kLogByteLimit);

  persisted_logs.StoreLog("one");
  persisted_logs.StageNextLog();
  persisted_logs.StoreLog("two");
  persisted_logs.DiscardStagedLog();
  persisted_logs.PersistUnsentLogs();

  TestPersistedLogs result_persisted_logs(&prefs_, kLogByteLimit);
  result_persisted_logs.LoadPersistedUnsentLogs();
  EXPECT_EQ(1U, result_persisted_logs.size());
  result_persisted_logs.ExpectNextLog("two");
}


TEST_F(PersistedLogsTest, Hashes) {
  const char kFooText[] = "foo";
  const std::string foo_hash = base::SHA1HashString(kFooText);

  TestPersistedLogs persisted_logs(&prefs_, kLogByteLimit);
  persisted_logs.StoreLog(kFooText);
  persisted_logs.StageNextLog();

  EXPECT_EQ(Compress(kFooText), persisted_logs.staged_log());
  EXPECT_EQ(foo_hash, persisted_logs.staged_log_hash());
}

TEST_F(PersistedLogsTest, Signatures) {
  const char kFooText[] = "foo";

  TestPersistedLogs persisted_logs(&prefs_, kLogByteLimit);
  persisted_logs.StoreLog(kFooText);
  persisted_logs.StageNextLog();

  EXPECT_EQ(Compress(kFooText), persisted_logs.staged_log());

  // The expected signature as a base 64 encoded string. The value was obtained
  // by running the test with an empty expected_signature_base64 and taking the
  // actual value from the test failure message. Can be verifying by the
  // following python code:
  // import hmac, hashlib, base64
  // key = ''
  // print(base64.b64encode(
  //   hmac.new(key, msg='foo', digestmod=hashlib.sha256).digest()).decode())
  std::string expected_signature_base64 =
      "DA2Y9+PZ1F5y6Id7wbEEMn77nAexjy/+ztdtgTB/H/8=";

  std::string actual_signature_base64;
  base::Base64Encode(persisted_logs.staged_log_signature(),
                     &actual_signature_base64);
  EXPECT_EQ(expected_signature_base64, actual_signature_base64);

  // Test a different key results in a different signature.
  std::string key = "secret key, don't tell anyone";
  TestPersistedLogs persisted_logs_different_key(&prefs_, kLogByteLimit, key);

  persisted_logs_different_key.StoreLog(kFooText);
  persisted_logs_different_key.StageNextLog();

  EXPECT_EQ(Compress(kFooText), persisted_logs_different_key.staged_log());

  // Base 64 encoded signature obtained in similar fashion to previous
  // signature. To use previous python code change:
  // key = "secret key, don't tell anyone"
  expected_signature_base64 = "DV7z8wdDrjLkQrCzrXR3UjWsR3/YVM97tIhMnhUvfXM=";
  base::Base64Encode(persisted_logs_different_key.staged_log_signature(),
                     &actual_signature_base64);

  EXPECT_EQ(expected_signature_base64, actual_signature_base64);
}

}  // namespace metrics
