// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/time/time.h"
#include "chrome/browser/sync/test/integration/bookmarks_helper.h"
#include "chrome/browser/sync/test/integration/encryption_helper.h"
#include "chrome/browser/sync/test/integration/profile_sync_service_harness.h"
#include "chrome/browser/sync/test/integration/sync_integration_test_util.h"
#include "chrome/browser/sync/test/integration/sync_test.h"
#include "chrome/browser/sync/test/integration/user_events_helper.h"
#include "chrome/browser/sync/user_event_service_factory.h"
#include "components/sync/protocol/user_event_specifics.pb.h"
#include "components/sync/user_events/user_event_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

using sync_pb::UserEventSpecifics;

const int kEncryptingClientId = 0;
const int kDecryptingClientId = 1;

class TwoClientUserEventsSyncTest : public SyncTest {
 public:
  TwoClientUserEventsSyncTest() : SyncTest(TWO_CLIENT) {}
  ~TwoClientUserEventsSyncTest() override {}

  bool ExpectNoUserEvent(int index) {
    return UserEventEqualityChecker(GetSyncService(index), GetFakeServer(),
                                    /*expected_specifics=*/{})
        .Wait();
  }

  bool WaitForPassphraseRequiredState(int index, bool desired_state) {
    return PassphraseRequiredStateChecker(GetSyncService(index), desired_state)
        .Wait();
  }

  bool WaitForBookmarksToMatchVerifier() {
    return BookmarksMatchVerifierChecker().Wait();
  }

  void AddTestBookmarksToClient(int index) {
    ASSERT_TRUE(
        bookmarks_helper::AddURL(index, 0, "What are you syncing about?",
                                 GURL("https://google.com/synced-bookmark-1")));
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(TwoClientUserEventsSyncTest);
};

IN_PROC_BROWSER_TEST_F(TwoClientUserEventsSyncTest,
                       SetPassphraseAndRecordEventAndThenSetupSync) {
  ASSERT_TRUE(SetupClients());
  ASSERT_TRUE(GetClient(kEncryptingClientId)->SetupSync());

  // Set up a sync client with custom passphrase to get the data encrypted on
  // the server.
  GetSyncService(kEncryptingClientId)
      ->GetUserSettings()
      ->SetEncryptionPassphrase("hunter2");
  ASSERT_TRUE(
      PassphraseAcceptedChecker(GetSyncService(kEncryptingClientId)).Wait());

  // Record a user event on the second client before setting up sync (before
  // knowing it will be encrypted). This event should not get recorded while
  // starting up sync because the user has custom passphrase setup.
  syncer::UserEventService* event_service =
      browser_sync::UserEventServiceFactory::GetForProfile(GetProfile(0));
  event_service->RecordUserEvent(user_events_helper::CreateTestEvent(
      base::Time() + base::TimeDelta::FromMicroseconds(1)));

  // Set up sync on the second client.
  ASSERT_TRUE(
      GetClient(kDecryptingClientId)
          ->SetupSyncNoWaitForCompletion(syncer::UserSelectableTypes()));
  // The second client asks the user to provide a password for decryption.
  ASSERT_TRUE(
      PassphraseRequiredChecker(GetSyncService(kDecryptingClientId)).Wait());
  // Provide the password.
  ASSERT_TRUE(GetSyncService(kDecryptingClientId)
                  ->GetUserSettings()
                  ->SetDecryptionPassphrase("hunter2"));
  // Check it is accepted and finish the setup.
  ASSERT_TRUE(
      PassphraseAcceptedChecker(GetSyncService(kDecryptingClientId)).Wait());
  GetClient(kDecryptingClientId)->FinishSyncSetup();

  // Just checking that we don't see the recorded test event isn't very
  // convincing yet, because it may simply not have reached the server yet. So
  // let's send something else on the second client through the system that we
  // can wait on before checking. Bookmark data was picked fairly arbitrarily.
  AddTestBookmarksToClient(kDecryptingClientId);
  ASSERT_TRUE(WaitForBookmarksToMatchVerifier());

  // Finally, make sure no user event got sent to the server.
  EXPECT_TRUE(ExpectNoUserEvent(kDecryptingClientId));
}

}  // namespace
