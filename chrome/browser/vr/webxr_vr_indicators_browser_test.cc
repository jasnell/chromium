// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <vector>

#include "base/bind_helpers.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/vr/test/ui_utils.h"
#include "chrome/browser/vr/test/webxr_vr_browser_test.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"

// Browser tests for indicators shown at various phases of an immersive session.

namespace vr {

namespace {

struct TestIndicatorSetting {
  TestIndicatorSetting(ContentSettingsType setting_type,
                       ContentSetting setting,
                       UserFriendlyElementName name,
                       bool visibility)
      : content_setting_type(setting_type),
        content_setting(setting),
        element_name(name),
        element_visibility(visibility) {}
  ContentSettingsType content_setting_type;
  ContentSetting content_setting;
  UserFriendlyElementName element_name;
  bool element_visibility;
};

struct TestContentSettings {
  TestContentSettings(ContentSettingsType setting_type, ContentSetting setting)
      : content_setting_type(setting_type), content_setting(setting) {}
  ContentSettingsType content_setting_type;
  ContentSetting content_setting;
};

// Helpers
std::vector<TestContentSettings> ExtractFrom(
    const std::vector<TestIndicatorSetting>& test_indicator_settings) {
  std::vector<TestContentSettings> test_content_settings;
  std::transform(test_indicator_settings.begin(), test_indicator_settings.end(),
                 std::back_inserter(test_content_settings),
                 [](const TestIndicatorSetting& s) -> TestContentSettings {
                   return {s.content_setting_type, s.content_setting};
                 });
  return test_content_settings;
}

void SetMultipleContentSetting(
    WebXrVrBrowserTestStandard* t,
    const std::vector<TestContentSettings>& test_settings) {
  HostContentSettingsMap* host_content_settings_map =
      HostContentSettingsMapFactory::GetForProfile(Profile::FromBrowserContext(
          t->GetCurrentWebContents()->GetBrowserContext()));
  for (const TestContentSettings& s : test_settings)
    host_content_settings_map->SetContentSettingDefaultScope(
        t->GetCurrentWebContents()->GetLastCommittedURL(),
        t->GetCurrentWebContents()->GetLastCommittedURL(),
        s.content_setting_type, std::string(), s.content_setting);
}

void LoadGenericPageChangeDefaultPermissionAndEnterVr(
    WebXrVrBrowserTestStandard* t,
    const std::vector<TestContentSettings>& test_settings) {
  t->LoadUrlAndAwaitInitialization(
      t->GetEmbeddedServerUrlForHtmlTestFile("generic_webxr_page"));
  SetMultipleContentSetting(t, test_settings);
  t->EnterSessionWithUserGestureOrFail();
}

// Tests that indicators are displayed in the headset when a device becomes
// in-use.
void TestIndicatorOnAccessForContentType(
    WebXrVrBrowserTestStandard* t,
    ContentSettingsType content_setting_type,
    const std::string& script,
    UserFriendlyElementName element_name) {
  // Enter VR while the content setting is CONTENT_SETTING_ASK to suppress
  // its corresponding indicator from initially showing up.
  LoadGenericPageChangeDefaultPermissionAndEnterVr(
      t, {{content_setting_type, CONTENT_SETTING_ASK}});

  // Now, change the setting to allow so the in-use indicator shows up on device
  // usage.
  SetMultipleContentSetting(t, {{content_setting_type, CONTENT_SETTING_ALLOW}});
  t->RunJavaScriptOrFail(script);

  auto utils = UiUtils::Create();
  // Check if the location indicator shows.
  utils->PerformActionAndWaitForVisibilityStatus(element_name, true,
                                                 base::DoNothing::Once());

  t->EndSessionOrFail();
}

// Tests indicators on entering immersive session.
void TestForInitialIndicatorForContentType(
    WebXrVrBrowserTestStandard* t,
    const std::vector<TestIndicatorSetting>& test_indicator_settings) {
  DCHECK(!test_indicator_settings.empty());
  // Enter VR while the content setting is CONTENT_SETTING_ASK to suppress
  // its corresponding indicator from initially showing up.
  LoadGenericPageChangeDefaultPermissionAndEnterVr(
      t, ExtractFrom(test_indicator_settings));

  auto utils = UiUtils::Create();
  // Check if the location indicator shows.
  for (const TestIndicatorSetting& t : test_indicator_settings)
    utils->PerformActionAndWaitForVisibilityStatus(
        t.element_name, t.element_visibility, base::DoNothing::Once());

  t->EndSessionOrFail();
}

}  // namespace

// Tests for indicators when they become in-use
IN_PROC_BROWSER_TEST_F(WebXrVrBrowserTestStandard, TestLocationInUseIndicator) {
  // Asking for location seems to work without any hardware/machine specific
  // enabling/capability (unlike microphone, camera). Hence, this test.
  TestIndicatorOnAccessForContentType(
      this, CONTENT_SETTINGS_TYPE_GEOLOCATION,
      "navigator.geolocation.getCurrentPosition( ()=>{}, ()=>{} )",
      UserFriendlyElementName::kWebXrLocationPermissionIndicator);
}

IN_PROC_BROWSER_TEST_F(WebXrVrBrowserTestStandard,
                       DISABLED_TestMicrophoneInUseIndicator) {
  TestIndicatorOnAccessForContentType(
      this, CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC,
      "navigator.getUserMedia( {audio : true},  ()=>{}, ()=>{} )",
      UserFriendlyElementName::kWebXrAudioIndicator);
}

IN_PROC_BROWSER_TEST_F(WebXrVrBrowserTestStandard,
                       DISABLED_TestCameraInUseIndicator) {
  TestIndicatorOnAccessForContentType(
      this, CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA,
      "navigator.getUserMedia( {video : true},  ()=>{}, ()=>{} )",
      UserFriendlyElementName::kWebXrVideoPermissionIndicator);
}

// Single indicator tests on entering immersive session
IN_PROC_BROWSER_TEST_F(WebXrVrBrowserTestStandard,
                       TestLocationIndicatorWhenPermissionInitiallyAllowed) {
  TestForInitialIndicatorForContentType(
      this,
      {{CONTENT_SETTINGS_TYPE_GEOLOCATION, CONTENT_SETTING_ALLOW,
        UserFriendlyElementName::kWebXrLocationPermissionIndicator, true}});
}

IN_PROC_BROWSER_TEST_F(WebXrVrBrowserTestStandard,
                       TestLocationIndicatorWhenPermissionInitiallyBlocked) {
  TestForInitialIndicatorForContentType(
      this,
      {{CONTENT_SETTINGS_TYPE_GEOLOCATION, CONTENT_SETTING_BLOCK,
        UserFriendlyElementName::kWebXrLocationPermissionIndicator, false}});
}

IN_PROC_BROWSER_TEST_F(WebXrVrBrowserTestStandard,
                       TestLocationIndicatorWhenUserAskedToPrompt) {
  TestForInitialIndicatorForContentType(
      this,
      {{CONTENT_SETTINGS_TYPE_GEOLOCATION, CONTENT_SETTING_ASK,
        UserFriendlyElementName::kWebXrLocationPermissionIndicator, false}});
}

// Indicator combination tests on entering immersive session
IN_PROC_BROWSER_TEST_F(WebXrVrBrowserTestStandard,
                       TestMultipleInitialIndicators_NoDevicesAllowed) {
  TestForInitialIndicatorForContentType(
      this,
      {
          {CONTENT_SETTINGS_TYPE_GEOLOCATION, CONTENT_SETTING_ASK,
           UserFriendlyElementName::kWebXrLocationPermissionIndicator, false},
          {CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC, CONTENT_SETTING_ASK,
           UserFriendlyElementName::kWebXrAudioIndicator, false},
          {CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA, CONTENT_SETTING_BLOCK,
           UserFriendlyElementName::kWebXrVideoPermissionIndicator, false},
      });
}

IN_PROC_BROWSER_TEST_F(WebXrVrBrowserTestStandard,
                       TestMultipleInitialIndicators_OneDeviceAllowed) {
  TestForInitialIndicatorForContentType(
      this,
      {
          {CONTENT_SETTINGS_TYPE_GEOLOCATION, CONTENT_SETTING_ASK,
           UserFriendlyElementName::kWebXrLocationPermissionIndicator, false},
          {CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC, CONTENT_SETTING_ALLOW,
           UserFriendlyElementName::kWebXrAudioIndicator, true},
          {CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA, CONTENT_SETTING_BLOCK,
           UserFriendlyElementName::kWebXrVideoPermissionIndicator, false},
      });
}

IN_PROC_BROWSER_TEST_F(WebXrVrBrowserTestStandard,
                       TestMultipleInitialIndicators_TwoDevicesAllowed) {
  TestForInitialIndicatorForContentType(
      this,
      {
          {CONTENT_SETTINGS_TYPE_GEOLOCATION, CONTENT_SETTING_ALLOW,
           UserFriendlyElementName::kWebXrLocationPermissionIndicator, true},
          {CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC, CONTENT_SETTING_BLOCK,
           UserFriendlyElementName::kWebXrAudioIndicator, false},
          {CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA, CONTENT_SETTING_ALLOW,
           UserFriendlyElementName::kWebXrVideoPermissionIndicator, true},
      });
}

IN_PROC_BROWSER_TEST_F(WebXrVrBrowserTestStandard,
                       TestMultipleInitialIndicators_ThreeDevicesAllowed) {
  TestForInitialIndicatorForContentType(
      this,
      {
          {CONTENT_SETTINGS_TYPE_GEOLOCATION, CONTENT_SETTING_ALLOW,
           UserFriendlyElementName::kWebXrLocationPermissionIndicator, true},
          {CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC, CONTENT_SETTING_ALLOW,
           UserFriendlyElementName::kWebXrAudioIndicator, true},
          {CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA, CONTENT_SETTING_ALLOW,
           UserFriendlyElementName::kWebXrVideoPermissionIndicator, true},
      });
}

}  // namespace vr
