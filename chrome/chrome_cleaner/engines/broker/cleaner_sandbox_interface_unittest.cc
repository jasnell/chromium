// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/chrome_cleaner/engines/broker/cleaner_sandbox_interface.h"

#include <aclapi.h>

#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/message_loop/message_loop.h"
#include "base/path_service.h"
#include "base/process/kill.h"
#include "base/process/process.h"
#include "base/test/test_reg_util_win.h"
#include "base/test/test_timeouts.h"
#include "chrome/chrome_cleaner/engines/common/registry_util.h"
#include "chrome/chrome_cleaner/os/digest_verifier.h"
#include "chrome/chrome_cleaner/os/file_remover.h"
#include "chrome/chrome_cleaner/os/layered_service_provider_wrapper.h"
#include "chrome/chrome_cleaner/os/scoped_disable_wow64_redirection.h"
#include "chrome/chrome_cleaner/os/system_util.h"
#include "chrome/chrome_cleaner/os/system_util_cleaner.h"
#include "chrome/chrome_cleaner/os/task_scheduler.h"
#include "chrome/chrome_cleaner/test/file_remover_test_util.h"
#include "chrome/chrome_cleaner/test/reboot_deletion_helper.h"
#include "chrome/chrome_cleaner/test/scoped_process_protector.h"
#include "chrome/chrome_cleaner/test/test_executables.h"
#include "chrome/chrome_cleaner/test/test_file_util.h"
#include "chrome/chrome_cleaner/test/test_native_reg_util.h"
#include "chrome/chrome_cleaner/test/test_scoped_service_handle.h"
#include "chrome/chrome_cleaner/test/test_strings.h"
#include "chrome/chrome_cleaner/test/test_task_scheduler.h"
#include "chrome/chrome_cleaner/test/test_util.h"
#include "components/chrome_cleaner/public/constants/constants.h"
#include "sandbox/win/src/nt_internals.h"
#include "testing/gtest/include/gtest/gtest.h"

using chrome_cleaner::CreateEmptyFile;
using chrome_cleaner::GetWow64RedirectedSystemPath;
using chrome_cleaner::IsFileRegisteredForPostRebootRemoval;
using chrome_cleaner::ScopedTempDirNoWow64;
using chrome_cleaner::String16EmbeddedNulls;

namespace chrome_cleaner_sandbox {

namespace {

constexpr wchar_t kDirectNonRegistryPath[] = L"\\DosDevice\\C:";
constexpr wchar_t kTrickyNonRegistryPath[] =
    L"\\Registry\\Machine\\..\\..\\DosDevice\\C:";

// Similar in intent to the ScopedProcessProtector, this does not take ownership
// of a handle but twiddles the ACL on it on initialization and restores the
// ACL on de-initialization. Useful for tests that require denying access to
// something. |handle| should probably be opened with ALL_ACCESS or equivalent
// and it would be a Bad Idea to CloseHandle or similar on the handle before
// this goes out of scope.
class ScopedHandleProtector {
 public:
  explicit ScopedHandleProtector(HANDLE handle) : handle_(handle) { Protect(); }
  ~ScopedHandleProtector() { Release(); }

 private:
  void Protect() {
    // Store its existing DACL for cleanup purposes. This API function is weird:
    // the pointer placed into |original_dacl_| is actually a pointer into a
    // the structure pointed to by |original_descriptor_|. To use this, one
    // stores both, but frees ONLY the structure stuffed into
    // |original_descriptor_|.
    if (GetSecurityInfo(handle_, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                        /*ppsidOwner=*/NULL, /*ppsidOwner=*/NULL,
                        &original_dacl_, /*ppsidOwner=*/NULL,
                        &original_descriptor_) != ERROR_SUCCESS) {
      PLOG(ERROR) << "Failed to retreieve original DACL.";
      return;
    }

    // Set a new empty DACL, effectively denying all things on the process
    // object.
    ACL dacl;
    if (!InitializeAcl(&dacl, sizeof(dacl), ACL_REVISION)) {
      PLOG(ERROR) << "Failed to initialize DACL";
      return;
    }
    if (SetSecurityInfo(handle_, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                        /*psidOwner=*/NULL, /*psidGroup=*/NULL, &dacl,
                        /*pSacl=*/NULL) != ERROR_SUCCESS) {
      PLOG(ERROR) << "Failed to set new DACL.";
      return;
    }

    initialized_ = true;
  }

  void Release() {
    if (initialized_) {
      if (SetSecurityInfo(handle_, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                          /*psidOwner=*/NULL, /*psidGroup=*/NULL,
                          original_dacl_, /*pSacl=*/NULL) != ERROR_SUCCESS) {
        PLOG(ERROR) << "Failed to restore original DACL.";
      }
    }

    if (original_descriptor_) {
      ::LocalFree(original_descriptor_);
      original_dacl_ = nullptr;
      original_descriptor_ = nullptr;
    }

    initialized_ = false;
  }

  bool initialized_ = false;
  HANDLE handle_;
  PACL original_dacl_ = nullptr;
  PSECURITY_DESCRIPTOR original_descriptor_ = nullptr;
};

String16EmbeddedNulls FullyQualifiedKeyPathWithTrailingNull(
    const ScopedTempRegistryKey& temp_key,
    const std::vector<wchar_t>& key_name) {
  // key vectors are expected to end with NULL.
  DCHECK_EQ(key_name.back(), L'\0');

  base::string16 full_key_path(temp_key.FullyQualifiedPath());
  full_key_path += L"\\";
  // Include key_name's trailing NULL.
  full_key_path.append(key_name.begin(), key_name.end());
  return String16EmbeddedNulls(full_key_path);
}

String16EmbeddedNulls StringWithTrailingNull(const base::string16& str) {
  // string16::size() does not count the trailing null.
  return String16EmbeddedNulls(str.c_str(), str.size() + 1);
}

String16EmbeddedNulls VeryLongStringWithPrefix(
    const String16EmbeddedNulls& prefix) {
  return String16EmbeddedNulls(base::string16(prefix.CastAsWCharArray()) +
                               base::string16(kMaxRegistryParamLength, L'a'));
}

}  // namespace

class CleanerSandboxInterfaceDeleteFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    file_remover_ = std::make_unique<chrome_cleaner::FileRemover>(
        /*digest_verifier=*/nullptr, /*archiver=*/nullptr,
        chrome_cleaner::LayeredServiceProviderWrapper(),
        chrome_cleaner::FilePathSet(),
        base::BindRepeating(
            &CleanerSandboxInterfaceDeleteFileTest::RebootRequired,
            base::Unretained(this)));
  }

  void RebootRequired() { reboot_required_ = true; }
  void ClearRebootRequired() { reboot_required_ = false; }

  std::unique_ptr<chrome_cleaner::FileRemoverAPI> file_remover_;
  bool reboot_required_ = false;
  base::MessageLoop message_loop_;
};

TEST_F(CleanerSandboxInterfaceDeleteFileTest, DeleteFile_BasicFile) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath file_path = temp.GetPath().Append(L"temp_file.exe");

  ASSERT_TRUE(chrome_cleaner::CreateFileInFolder(
      file_path.DirName(), file_path.BaseName().value().c_str()));

  chrome_cleaner::VerifyRemoveNowSuccess(file_path, file_remover_.get());
  EXPECT_FALSE(base::PathExists(file_path));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, DeleteFile_UnicodePath) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath file_path =
      temp.GetPath().Append(chrome_cleaner::kValidUtf8Name);

  ASSERT_TRUE(chrome_cleaner::CreateFileInFolder(
      file_path.DirName(), file_path.BaseName().value().c_str()));

  chrome_cleaner::VerifyRemoveNowSuccess(file_path, file_remover_.get());
  EXPECT_FALSE(base::PathExists(file_path));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, DeleteFile_DirectoryTraversal) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath file_path = temp.GetPath().Append(L"temp_file.exe");

  ASSERT_TRUE(chrome_cleaner::CreateFileInFolder(
      file_path.DirName(), file_path.BaseName().value().c_str()));

  base::FilePath path_directory_traversal =
      file_path.Append(L"..").Append(file_path.BaseName());

  chrome_cleaner::VerifyRemoveNowFailure(path_directory_traversal,
                                         file_remover_.get());
  EXPECT_TRUE(base::PathExists(path_directory_traversal));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, DirectoryDeletionFails) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath dir_path = temp.GetPath().Append(L"directory.exe");
  ASSERT_TRUE(base::CreateDirectory(dir_path));

  chrome_cleaner::VerifyRemoveNowFailure(dir_path, file_remover_.get());
  EXPECT_TRUE(base::DirectoryExists(dir_path));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, NotActiveFileType) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath file_path = temp.GetPath().Append(L"temp_file.txt");
  ASSERT_TRUE(chrome_cleaner::CreateFileInFolder(
      file_path.DirName(), file_path.BaseName().value().c_str()));

  chrome_cleaner::VerifyRemoveNowSuccess(file_path, file_remover_.get());
  EXPECT_TRUE(base::PathExists(file_path));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, DeleteFile_RelativeFilePath) {
  base::FilePath file_path(L"temp_file.exe");

  chrome_cleaner::VerifyRemoveNowFailure(file_path, file_remover_.get());
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, DeleteFile_NativePath) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath file_path = temp.GetPath().Append(L"temp_file.exe");

  base::string16 native_path = L"\\??\\" + file_path.value();
  chrome_cleaner::VerifyRemoveNowFailure(base::FilePath(native_path),
                                         file_remover_.get());
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, DeleteFile_Wow64Disabled) {
  // Skip this test on 32-bit Windows because there Wow64 redirection is not
  // supported.
  if (!chrome_cleaner::IsX64Architecture())
    return;

  static constexpr wchar_t kTestFile[] = L"temp_file.exe";

  // Create a directory and a file under true System32 directory.
  ScopedTempDirNoWow64 dir_in_system32;
  ASSERT_TRUE(
      dir_in_system32.CreateEmptyFileInUniqueSystem32TempDir(kTestFile));
  const base::FilePath dir_name = dir_in_system32.GetPath().BaseName();
  const base::FilePath file_in_system32 =
      dir_in_system32.GetPath().Append(kTestFile);

  // Mirror exactly the created folder and file under SysWOW64.
  // C:\Windows\
  //       system32\
  //           dirname\  temp_file.exe
  //       SysWOW64\
  //           dirname\  temp_file.exe
  base::FilePath syswow64_path = GetWow64RedirectedSystemPath();
  base::ScopedTempDir dir_in_syswow64;
  ASSERT_TRUE(dir_in_syswow64.Set(syswow64_path.Append(dir_name)));
  ASSERT_TRUE(
      chrome_cleaner::CreateFileInFolder(dir_in_syswow64.GetPath(), kTestFile));
  const base::FilePath file_in_syswow64 =
      dir_in_syswow64.GetPath().Append(kTestFile);

  // Delete file from C:\Windows\system32\ directory and verify it is not
  // deleted from the corresponding SysWOW64 directory.
  chrome_cleaner::VerifyRemoveNowSuccess(file_in_system32, file_remover_.get());

  {
    chrome_cleaner::ScopedDisableWow64Redirection no_wow64_redirection;
    EXPECT_FALSE(base::PathExists(file_in_system32));
    EXPECT_TRUE(base::PathExists(file_in_syswow64));
  }

  // Create a subdirectory of the temp dir in System32 that looks like an
  // active file name. The corresponding path in SysWOW64 should already exist
  // from the previous test.
  {
    chrome_cleaner::ScopedDisableWow64Redirection no_wow64_redirection;
    ASSERT_TRUE(base::CreateDirectory(file_in_system32));
    ASSERT_TRUE(base::PathExists(file_in_syswow64));
  }

  // Make sure the subdirectory can't be deleted. This tests that the "is this
  // a directory" validation is not redirected to the SysWOW64 path, which is
  // actually a file.
  chrome_cleaner::VerifyRemoveNowFailure(file_in_system32, file_remover_.get());
  {
    chrome_cleaner::ScopedDisableWow64Redirection no_wow64_redirection;
    EXPECT_TRUE(base::DirectoryExists(file_in_system32));
    EXPECT_TRUE(base::PathExists(file_in_syswow64));
  }
}

// Tests MoveFileEx functionality. Note that this test will pollute your
// registry with removal entries for non-existent files. Standard registry
// redirection using RegOverridePredefKey unfortunately doesn't work for
// MoveFileEx. This should be mostly harmless.
TEST_F(CleanerSandboxInterfaceDeleteFileTest, DeleteFilePostReboot) {
  base::ScopedTempDir dir;
  EXPECT_TRUE(dir.CreateUniqueTempDir());

  const base::FilePath file_path(dir.GetPath().Append(L"a.exe"));
  EXPECT_TRUE(CreateEmptyFile(file_path));

  chrome_cleaner::VerifyRegisterPostRebootRemovalSuccess(file_path,
                                                         file_remover_.get());
  EXPECT_TRUE(reboot_required_);

  ClearRebootRequired();
  chrome_cleaner::VerifyRegisterPostRebootRemovalFailure(dir.GetPath(),
                                                         file_remover_.get());
  EXPECT_FALSE(reboot_required_);
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, DeleteSymlinkToFile) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath file_path = temp.GetPath().Append(L"temp_file.exe");
  ASSERT_TRUE(chrome_cleaner::CreateFileInFolder(
      file_path.DirName(), file_path.BaseName().value().c_str()));

  base::FilePath link_path = temp.GetPath().Append(L"link_file.exe");
  ASSERT_NE(0, ::CreateSymbolicLink(link_path.value().c_str(),
                                    file_path.value().c_str(), 0));

  chrome_cleaner::VerifyRemoveNowSuccess(link_path, file_remover_.get());
  EXPECT_FALSE(base::PathExists(link_path));
  EXPECT_TRUE(base::PathExists(file_path));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, DeleteSymlinkToFolderFails) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath subdir_path = temp.GetPath().Append(L"dir");
  ASSERT_TRUE(base::CreateDirectory(subdir_path));
  base::FilePath link_path = temp.GetPath().Append(L"link_file.exe");

  ASSERT_NE(0, ::CreateSymbolicLink(link_path.value().c_str(),
                                    subdir_path.value().c_str(),
                                    SYMBOLIC_LINK_FLAG_DIRECTORY));

  chrome_cleaner::VerifyRemoveNowFailure(link_path, file_remover_.get());
  EXPECT_TRUE(base::PathExists(link_path));
  EXPECT_TRUE(base::DirectoryExists(subdir_path));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, AllowTrailingWhitespace) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath file_path = temp.GetPath().Append(L"temp_file.exe");

  ASSERT_TRUE(chrome_cleaner::CreateFileInFolder(
      file_path.DirName(), file_path.BaseName().value().c_str()));

  const base::FilePath path_with_space(file_path.value() + L" ");
  chrome_cleaner::VerifyRemoveNowSuccess(path_with_space, file_remover_.get());
  EXPECT_FALSE(base::PathExists(file_path));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, DeleteAlternativeStream) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath file_path = temp.GetPath().Append(L"temp_file.txt");
  ASSERT_TRUE(chrome_cleaner::CreateFileInFolder(
      file_path.DirName(), file_path.BaseName().value().c_str()));

  constexpr char kTestContent[] = "content";
  const base::FilePath stream_path(file_path.value() + L":stream");
  chrome_cleaner::CreateFileWithContent(stream_path, kTestContent,
                                        sizeof(kTestContent));
  EXPECT_TRUE(base::PathExists(stream_path));

  chrome_cleaner::VerifyRemoveNowSuccess(stream_path, file_remover_.get());

  // The alternative stream should be gone, but the file itself should still be
  // present on the system.
  EXPECT_FALSE(base::PathExists(stream_path));
  EXPECT_TRUE(base::PathExists(file_path));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, DeleteAlternativeStreamWithType) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath file_path = temp.GetPath().Append(L"temp_file.exe");
  ASSERT_TRUE(chrome_cleaner::CreateFileInFolder(
      file_path.DirName(), file_path.BaseName().value().c_str()));

  constexpr char kTestContent[] = "content";
  const base::FilePath stream_path(file_path.value() + L":stream");
  const base::FilePath stream_type_path(stream_path.value() + L":$DATA");

  // Deleting using the full path with data type should work.
  chrome_cleaner::CreateFileWithContent(stream_type_path, kTestContent,
                                        sizeof(kTestContent));
  EXPECT_TRUE(base::PathExists(stream_path));
  EXPECT_TRUE(base::PathExists(stream_type_path));

  chrome_cleaner::VerifyRemoveNowSuccess(stream_type_path, file_remover_.get());

  EXPECT_FALSE(base::PathExists(stream_path));
  EXPECT_FALSE(base::PathExists(stream_type_path));
  EXPECT_TRUE(base::PathExists(file_path));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest,
       DeleteExecutableWithDefaultStream) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath executable_path = temp.GetPath().Append(L"temp_file.exe");
  ASSERT_TRUE(chrome_cleaner::CreateFileInFolder(
      executable_path.DirName(), executable_path.BaseName().value().c_str()));

  // ::$DATA denotes the default stream, and therefore should exist.
  const base::FilePath path_with_datatype(executable_path.value() + L"::$DATA");
  EXPECT_TRUE(base::PathExists(path_with_datatype));

  // Appending ::$DATA to file path should not prevent file extension checks.
  chrome_cleaner::VerifyRemoveNowSuccess(path_with_datatype,
                                         file_remover_.get());
  EXPECT_FALSE(base::PathExists(executable_path));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, IgnoreTextWithDefaultStream) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath file_path = temp.GetPath().Append(L"temp_file.txt");
  ASSERT_TRUE(chrome_cleaner::CreateFileInFolder(
      file_path.DirName(), file_path.BaseName().value().c_str()));

  // ::$DATA denotes the default stream, and therefore should exist.
  const base::FilePath path_with_datatype(file_path.value() + L"::$DATA");
  EXPECT_TRUE(base::PathExists(path_with_datatype));

  chrome_cleaner::VerifyRemoveNowSuccess(path_with_datatype,
                                         file_remover_.get());
  EXPECT_TRUE(base::PathExists(file_path));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, DeleteDosMzExecutables) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath file_path = temp.GetPath().Append(L"temp_file.txt");
  const char kExecutableFileContents[] = "MZ I am so executable";
  chrome_cleaner::CreateFileWithContent(file_path, kExecutableFileContents,
                                        sizeof(kExecutableFileContents));

  chrome_cleaner::VerifyRemoveNowSuccess(file_path, file_remover_.get());
  EXPECT_FALSE(base::PathExists(file_path));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, DeletesWhitelisted) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath file_path = temp.GetPath().Append(L"temp_file.txt");
  ASSERT_TRUE(chrome_cleaner::CreateFileInFolder(
      file_path.DirName(), file_path.BaseName().value().c_str()));

  chrome_cleaner::VerifyRemoveNowSuccess(file_path, file_remover_.get());
  EXPECT_TRUE(base::PathExists(file_path));

  chrome_cleaner::FilePathSet whitelist;
  whitelist.Insert(file_path);
  auto remover_with_whitelist = std::make_unique<chrome_cleaner::FileRemover>(
      /*digest_verifier=*/nullptr, /*archiver=*/nullptr,
      chrome_cleaner::LayeredServiceProviderWrapper(), whitelist,
      base::DoNothing());

  chrome_cleaner::VerifyRemoveNowSuccess(file_path,
                                         remover_with_whitelist.get());
  EXPECT_FALSE(base::PathExists(file_path));
}

TEST_F(CleanerSandboxInterfaceDeleteFileTest, RecognizedDigest) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath file_path = temp.GetPath().Append(L"temp_file.exe");
  ASSERT_TRUE(chrome_cleaner::CreateFileInFolder(
      file_path.DirName(), file_path.BaseName().value().c_str()));

  auto remover_with_digest_verifier =
      std::make_unique<chrome_cleaner::FileRemover>(
          chrome_cleaner::DigestVerifier::CreateFromFile(file_path),
          /*archiver=*/nullptr, chrome_cleaner::LayeredServiceProviderWrapper(),
          chrome_cleaner::FilePathSet(), base::DoNothing());

  chrome_cleaner::VerifyRemoveNowFailure(file_path,
                                         remover_with_digest_verifier.get());
  EXPECT_TRUE(base::PathExists(file_path));

  // The whitelist should not override the DigestVerifier.
  base::FilePath txt_file_path = temp.GetPath().Append(L"temp_file.txt");
  ASSERT_TRUE(chrome_cleaner::CreateFileInFolder(
      txt_file_path.DirName(), txt_file_path.BaseName().value().c_str()));

  chrome_cleaner::FilePathSet whitelist;
  whitelist.Insert(txt_file_path);
  auto remover_with_whitelist = std::make_unique<chrome_cleaner::FileRemover>(
      chrome_cleaner::DigestVerifier::CreateFromFile(txt_file_path),
      /*archiver=*/nullptr, chrome_cleaner::LayeredServiceProviderWrapper(),
      whitelist, base::DoNothing());

  chrome_cleaner::VerifyRemoveNowFailure(txt_file_path,
                                         remover_with_whitelist.get());
  EXPECT_TRUE(base::PathExists(txt_file_path));
}

class CleanerInterfaceRegistryTest : public ::testing::Test {
 public:
  void SetUp() override {
    std::vector<wchar_t> key_name{L'a', L'b', L'\0', L'c', L'\0'};

    ULONG disposition = 0;
    EXPECT_EQ(STATUS_SUCCESS, NativeCreateKey(temp_key_.Get(), &key_name,
                                              &subkey_handle_, &disposition));
    ASSERT_EQ(static_cast<ULONG>(REG_CREATED_NEW_KEY), disposition);
    ASSERT_NE(INVALID_HANDLE_VALUE, subkey_handle_);
    full_key_path_ = FullyQualifiedKeyPathWithTrailingNull(temp_key_, key_name);

    // Create a default and a named value.
    EXPECT_EQ(STATUS_SUCCESS,
              NativeSetValueKey(subkey_handle_, String16EmbeddedNulls(nullptr),
                                REG_SZ, value_));
    EXPECT_EQ(STATUS_SUCCESS,
              NativeSetValueKey(subkey_handle_, value_name_, REG_SZ, value_));

    default_value_should_be_normalized_ = base::BindRepeating(
        &chrome_cleaner_sandbox::DefaultShouldValueBeNormalized);
  }

  void TearDown() override {
    if (subkey_handle_ != INVALID_HANDLE_VALUE) {
      NativeDeleteKey(subkey_handle_);
      EXPECT_TRUE(::CloseHandle(subkey_handle_));
    }
  }

 protected:
  ScopedTempRegistryKey temp_key_;
  HANDLE subkey_handle_;
  String16EmbeddedNulls full_key_path_;
  String16EmbeddedNulls value_name_{L'f', L'o', L'o', L'\0'};
  String16EmbeddedNulls value_{L'b', L'a', L'r', L'\0'};
  String16EmbeddedNulls valid_changed_value_{L'b', L'a', L'\0'};
  ShouldNormalizeRegistryValue default_value_should_be_normalized_;
};

TEST_F(CleanerInterfaceRegistryTest, NtDeleteRegistryKey_Success) {
  EXPECT_TRUE(SandboxNtDeleteRegistryKey(full_key_path_));

  HANDLE deleted_key_handle = INVALID_HANDLE_VALUE;
  EXPECT_EQ(
      STATUS_OBJECT_NAME_NOT_FOUND,
      NativeOpenKey(nullptr, full_key_path_, KEY_READ, &deleted_key_handle));
}

TEST_F(CleanerInterfaceRegistryTest, NtDeleteRegistryKey_NullKey) {
  EXPECT_FALSE(SandboxNtDeleteRegistryKey(String16EmbeddedNulls(nullptr)));
}

TEST_F(CleanerInterfaceRegistryTest, NtDeleteRegistryKey_LongKey) {
  String16EmbeddedNulls long_key = VeryLongStringWithPrefix(full_key_path_);
  EXPECT_FALSE(SandboxNtDeleteRegistryKey(long_key));
}

TEST_F(CleanerInterfaceRegistryTest, NtDeleteRegistryKey_KeyMissingTerminator) {
  String16EmbeddedNulls no_terminating_null_key(
      full_key_path_.CastAsWCharArray(), full_key_path_.size() - 1);
  EXPECT_FALSE(SandboxNtDeleteRegistryKey(no_terminating_null_key));
}

// TODO(veranika): This test is failing on win10 bots. Fix and re-enable it.
TEST_F(CleanerInterfaceRegistryTest,
       DISABLED_NtDeleteRegistryKey_AccessDenied) {
  {
    // Protect the key, expect deletion to fail.
    ScopedHandleProtector protector(subkey_handle_);
    EXPECT_FALSE(SandboxNtDeleteRegistryKey(full_key_path_));
  }

  // Key should now be unprotected and deletion should succeed.
  EXPECT_TRUE(SandboxNtDeleteRegistryKey(full_key_path_));

  // Shouldn't be able to do that twice.
  EXPECT_FALSE(SandboxNtDeleteRegistryKey(full_key_path_));
}

TEST_F(CleanerInterfaceRegistryTest, NtDeleteRegistryKey_NonRegistryPath) {
  EXPECT_FALSE(SandboxNtDeleteRegistryKey(
      StringWithTrailingNull(kDirectNonRegistryPath)));
  EXPECT_FALSE(SandboxNtDeleteRegistryKey(
      StringWithTrailingNull(kTrickyNonRegistryPath)));
}

TEST_F(CleanerInterfaceRegistryTest, NtDeleteRegistryValue_Success) {
  EXPECT_TRUE(SandboxNtDeleteRegistryValue(full_key_path_, value_name_));
}

TEST_F(CleanerInterfaceRegistryTest, NtDeleteRegistryValue_NullKey) {
  EXPECT_FALSE(SandboxNtDeleteRegistryValue(String16EmbeddedNulls(nullptr),
                                            value_name_));
}

TEST_F(CleanerInterfaceRegistryTest, NtDeleteRegistryValue_NullValue) {
  EXPECT_FALSE(SandboxNtDeleteRegistryValue(full_key_path_,
                                            String16EmbeddedNulls(nullptr)));
}

TEST_F(CleanerInterfaceRegistryTest, NtDeleteRegistryValue_LongKey) {
  String16EmbeddedNulls long_key = VeryLongStringWithPrefix(full_key_path_);
  EXPECT_FALSE(SandboxNtDeleteRegistryValue(long_key, value_name_));
}

TEST_F(CleanerInterfaceRegistryTest, NtDeleteRegistryValue_LongValue) {
  String16EmbeddedNulls very_long_name = VeryLongStringWithPrefix(value_name_);
  EXPECT_FALSE(SandboxNtDeleteRegistryValue(full_key_path_, very_long_name));
}

TEST_F(CleanerInterfaceRegistryTest,
       NtDeleteRegistryValue_KeyMissingTerminator) {
  String16EmbeddedNulls no_terminating_null_key(
      full_key_path_.CastAsWCharArray(), full_key_path_.size() - 1);
  EXPECT_FALSE(
      SandboxNtDeleteRegistryValue(no_terminating_null_key, value_name_));
}

TEST_F(CleanerInterfaceRegistryTest,
       NtDeleteRegistryValue_ValueMissingTerminator) {
  String16EmbeddedNulls no_terminating_null_name(value_name_.CastAsWCharArray(),
                                                 value_name_.size() - 1);
  EXPECT_FALSE(
      SandboxNtDeleteRegistryValue(full_key_path_, no_terminating_null_name));
}

TEST_F(CleanerInterfaceRegistryTest,
       NtDeleteRegistryValue_ValueNameHasEmbeddedNull) {
  String16EmbeddedNulls value_name{L'f', L'o', L'\0', L'o', L'\0'};

  EXPECT_EQ(STATUS_SUCCESS,
            NativeSetValueKey(subkey_handle_, value_name, REG_SZ, value_));

  EXPECT_TRUE(SandboxNtDeleteRegistryValue(full_key_path_, value_name));

  EXPECT_FALSE(
      NativeQueryValueKey(subkey_handle_, value_name, nullptr, nullptr));
}

TEST_F(CleanerInterfaceRegistryTest, NtDeleteRegistryValue_NonRegistryPath) {
  EXPECT_FALSE(SandboxNtDeleteRegistryValue(
      StringWithTrailingNull(kDirectNonRegistryPath), value_name_));
  EXPECT_FALSE(SandboxNtDeleteRegistryValue(
      StringWithTrailingNull(kTrickyNonRegistryPath), value_name_));
}

TEST_F(CleanerInterfaceRegistryTest, NtChangeRegistryValue_Success) {
  EXPECT_TRUE(SandboxNtChangeRegistryValue(
      full_key_path_, value_name_, valid_changed_value_,
      default_value_should_be_normalized_));

  DWORD type = 0;
  String16EmbeddedNulls actual_value;
  EXPECT_TRUE(
      NativeQueryValueKey(subkey_handle_, value_name_, &type, &actual_value));
  EXPECT_EQ(REG_SZ, type);
  EXPECT_EQ(valid_changed_value_, actual_value);
}

TEST_F(CleanerInterfaceRegistryTest, NtChangeRegistryValue_NullKey) {
  EXPECT_FALSE(SandboxNtChangeRegistryValue(
      String16EmbeddedNulls(nullptr), value_name_, valid_changed_value_,
      default_value_should_be_normalized_));
}

TEST_F(CleanerInterfaceRegistryTest, NtChangeRegistryValue_NullValue) {
  EXPECT_TRUE(SandboxNtChangeRegistryValue(
      full_key_path_, value_name_, String16EmbeddedNulls(nullptr),
      default_value_should_be_normalized_));

  String16EmbeddedNulls actual_value;
  EXPECT_TRUE(
      NativeQueryValueKey(subkey_handle_, value_name_, nullptr, &actual_value));
  EXPECT_EQ(0U, actual_value.size());
}

TEST_F(CleanerInterfaceRegistryTest, NtChangeRegistryValue_LongKey) {
  String16EmbeddedNulls long_key = VeryLongStringWithPrefix(full_key_path_);
  EXPECT_FALSE(
      SandboxNtChangeRegistryValue(long_key, value_name_, valid_changed_value_,
                                   default_value_should_be_normalized_));
}

TEST_F(CleanerInterfaceRegistryTest, NtChangeRegistryValue_LongValueName) {
  String16EmbeddedNulls very_long_name = VeryLongStringWithPrefix(value_name_);
  EXPECT_FALSE(SandboxNtChangeRegistryValue(
      full_key_path_, very_long_name, valid_changed_value_,
      default_value_should_be_normalized_));
}

TEST_F(CleanerInterfaceRegistryTest, NtChangeRegistryValue_LongValue) {
  String16EmbeddedNulls very_long_value = VeryLongStringWithPrefix(value_);
  EXPECT_FALSE(
      SandboxNtChangeRegistryValue(full_key_path_, value_name_, very_long_value,
                                   default_value_should_be_normalized_));
}

TEST_F(CleanerInterfaceRegistryTest,
       NtChangeRegistryValue_KeyMissingTerminator) {
  String16EmbeddedNulls no_terminating_null_key(
      full_key_path_.CastAsWCharArray(), full_key_path_.size() - 1);
  EXPECT_FALSE(SandboxNtChangeRegistryValue(
      no_terminating_null_key, value_name_, valid_changed_value_,
      default_value_should_be_normalized_));
}

TEST_F(CleanerInterfaceRegistryTest,
       NtChangeRegistryValue_ValueNameMissingTerminator) {
  String16EmbeddedNulls no_terminating_null_name(value_name_.CastAsWCharArray(),
                                                 value_name_.size() - 1);
  EXPECT_FALSE(SandboxNtChangeRegistryValue(
      full_key_path_, no_terminating_null_name, valid_changed_value_,
      default_value_should_be_normalized_));
}

TEST_F(CleanerInterfaceRegistryTest, NtChangeRegistryValue_NoNullTerminator) {
  String16EmbeddedNulls no_terminating_null_value(
      valid_changed_value_.CastAsWCharArray(), valid_changed_value_.size());
  EXPECT_TRUE(SandboxNtChangeRegistryValue(
      full_key_path_, value_name_, no_terminating_null_value,
      default_value_should_be_normalized_));
}

TEST_F(CleanerInterfaceRegistryTest, NtChangeRegistryValue_MissingValue) {
  String16EmbeddedNulls absent_value_name{L'f', L'o', L'\0', L'o', L'\0'};

  EXPECT_FALSE(SandboxNtChangeRegistryValue(
      full_key_path_, absent_value_name, valid_changed_value_,
      default_value_should_be_normalized_));
}

TEST_F(CleanerInterfaceRegistryTest, NtChangeRegistryValue_OtherValidType) {
  String16EmbeddedNulls reference_value_name{L'f', L'o', L'\0', L'o', L'\0'};

  EXPECT_EQ(STATUS_SUCCESS,
            NativeSetValueKey(subkey_handle_, reference_value_name,
                              REG_EXPAND_SZ, value_));

  EXPECT_TRUE(SandboxNtChangeRegistryValue(
      full_key_path_, reference_value_name, valid_changed_value_,
      default_value_should_be_normalized_));

  DWORD type = 0;
  EXPECT_TRUE(NativeQueryValueKey(subkey_handle_, reference_value_name, &type,
                                  nullptr));
  EXPECT_EQ(REG_EXPAND_SZ, type);
}

TEST_F(CleanerInterfaceRegistryTest, NtChangeRegistryValue_InvalidType) {
  String16EmbeddedNulls value_name{L'f', L'o', L'\0', L'o', L'\0'};

  EXPECT_EQ(STATUS_SUCCESS,
            NativeSetValueKey(subkey_handle_, value_name, REG_BINARY, value_));

  EXPECT_FALSE(SandboxNtChangeRegistryValue(
      full_key_path_, value_name, valid_changed_value_,
      default_value_should_be_normalized_));

  DWORD type = 0;
  EXPECT_TRUE(NativeQueryValueKey(subkey_handle_, value_name, &type, nullptr));
  EXPECT_EQ(REG_BINARY, type);
}

TEST_F(CleanerInterfaceRegistryTest, NtChangeRegistryValue_NullName) {
  EXPECT_EQ(STATUS_SUCCESS,
            NativeSetValueKey(subkey_handle_, String16EmbeddedNulls(nullptr),
                              REG_SZ, value_));

  EXPECT_TRUE(SandboxNtChangeRegistryValue(
      full_key_path_, String16EmbeddedNulls(nullptr), valid_changed_value_,
      default_value_should_be_normalized_));

  DWORD type = 0;
  String16EmbeddedNulls actual_value;
  EXPECT_TRUE(NativeQueryValueKey(
      subkey_handle_, String16EmbeddedNulls(nullptr), &type, &actual_value));
  EXPECT_EQ(REG_SZ, type);
  EXPECT_EQ(valid_changed_value_, actual_value);
}

// TODO(veranika): This test is failing on win10 bots. Fix and re-enable it.
TEST_F(CleanerInterfaceRegistryTest,
       DISABLED_NtChangeRegistryValue_AccessDenied) {
  {
    // Protect the key, expect modification to fail.
    ScopedHandleProtector protector(subkey_handle_);
    EXPECT_FALSE(SandboxNtChangeRegistryValue(
        full_key_path_, value_, valid_changed_value_,
        default_value_should_be_normalized_));
  }

  EXPECT_TRUE(
      SandboxNtChangeRegistryValue(full_key_path_, value_, valid_changed_value_,
                                   default_value_should_be_normalized_));
}

TEST_F(CleanerInterfaceRegistryTest, NtChangeRegistryValue_NonRegistryPath) {
  EXPECT_FALSE(SandboxNtChangeRegistryValue(
      StringWithTrailingNull(kDirectNonRegistryPath), value_name_,
      valid_changed_value_, default_value_should_be_normalized_));
  EXPECT_FALSE(SandboxNtChangeRegistryValue(
      StringWithTrailingNull(kTrickyNonRegistryPath), value_name_,
      valid_changed_value_, default_value_should_be_normalized_));
}

TEST_F(CleanerInterfaceRegistryTest, NtChangeRegistryValue_AllowNormalization) {
  String16EmbeddedNulls value = StringWithTrailingNull(L"f o q b,ab");
  EXPECT_EQ(STATUS_SUCCESS,
            NativeSetValueKey(subkey_handle_, value_name_, REG_SZ, value));

  String16EmbeddedNulls normalized_value =
      StringWithTrailingNull(L"f,o,q,b,ab");
  EXPECT_FALSE(SandboxNtChangeRegistryValue(
      full_key_path_, value_name_, normalized_value,
      default_value_should_be_normalized_));

  String16EmbeddedNulls normalized_shortened_value =
      StringWithTrailingNull(L"f,o,b,ab");
  EXPECT_FALSE(SandboxNtChangeRegistryValue(
      full_key_path_, value_name_, normalized_shortened_value,
      default_value_should_be_normalized_));

  // Switch to allow every value to be normalized.
  ShouldNormalizeRegistryValue normalize_all_values =
      base::BindRepeating([](const String16EmbeddedNulls&,
                             const String16EmbeddedNulls&) { return true; });
  EXPECT_TRUE(SandboxNtChangeRegistryValue(
      full_key_path_, value_name_, normalized_value, normalize_all_values));

  EXPECT_TRUE(SandboxNtChangeRegistryValue(full_key_path_, value_name_,
                                           normalized_shortened_value,
                                           normalize_all_values));
}

TEST(CleanerSandboxInterface, DeleteService_NotExisting) {
  EXPECT_TRUE(SandboxDeleteService(
      chrome_cleaner::RandomUnusedServiceNameForTesting().c_str()));
}

TEST(CleanerSandboxInterface, DeleteService_Success) {
  ASSERT_TRUE(chrome_cleaner::EnsureNoTestServicesRunning());

  chrome_cleaner::TestScopedServiceHandle service_handle;
  ASSERT_TRUE(service_handle.InstallService());
  service_handle.Close();

  EXPECT_TRUE(SandboxDeleteService(service_handle.service_name()));

  EXPECT_FALSE(chrome_cleaner::DoesServiceExist(service_handle.service_name()));
}

TEST(CleanerSandboxInterface, DeleteService_Running) {
  ASSERT_TRUE(chrome_cleaner::EnsureNoTestServicesRunning());

  chrome_cleaner::TestScopedServiceHandle service_handle;
  ASSERT_TRUE(service_handle.InstallService());
  ASSERT_TRUE(service_handle.StartService());
  service_handle.Close();

  EXPECT_TRUE(SandboxDeleteService(service_handle.service_name()));

  EXPECT_FALSE(chrome_cleaner::DoesServiceExist(service_handle.service_name()));
}

TEST(CleanerSandboxInterface, DeleteService_HandleHeld) {
  ASSERT_TRUE(chrome_cleaner::EnsureNoTestServicesRunning());

  chrome_cleaner::TestScopedServiceHandle service_handle;
  ASSERT_TRUE(service_handle.InstallService());
  ASSERT_TRUE(service_handle.StartService());

  // SandboxDeleteService should succeed because even though there is still a
  // handle to the service, it has been scheduled for deletion.
  EXPECT_TRUE(SandboxDeleteService(service_handle.service_name()));

  // Make sure that after the handle is closed the service is deleted.
  // Note: Before this handle is closed, OpenService may or may not provide
  // a valid handle (pre Win10 1703 a valid handle is returned), so
  // DoesServiceExists will provide different results.
  service_handle.Close();
  EXPECT_TRUE(
      chrome_cleaner::WaitForServiceDeleted(service_handle.service_name()));
  EXPECT_FALSE(chrome_cleaner::DoesServiceExist(service_handle.service_name()));
}

class CleanerSandboxInterface_WithTaskScheduler : public ::testing::Test {
 public:
  void SetUp() override {
    chrome_cleaner::TaskScheduler::SetMockDelegateForTesting(
        &test_task_scheduler_);
  }

  void TearDown() override {
    chrome_cleaner::TaskScheduler::SetMockDelegateForTesting(nullptr);
  }

 protected:
  chrome_cleaner::TestTaskScheduler test_task_scheduler_;
};

TEST_F(CleanerSandboxInterface_WithTaskScheduler, DeleteTask_Existing) {
  chrome_cleaner::TaskScheduler::TaskInfo task_info;
  ASSERT_TRUE(RegisterTestTask(&test_task_scheduler_, &task_info));

  EXPECT_TRUE(SandboxDeleteTask(task_info.name.c_str()));
}

TEST_F(CleanerSandboxInterface_WithTaskScheduler, DeleteTask_Missing) {
  EXPECT_TRUE(SandboxDeleteTask(L"NonExistentTaskName"));
}

TEST(CleanerSandboxInterface, TerminateProcessTest) {
  // Note that this test will fail under the debugger since the debugged test
  // process will inherit the SeDebugPrivilege which allows the test to get
  // an ALL_ACCESS handle.
  if (::IsDebuggerPresent()) {
    LOG(ERROR) << "TerminateProcessTest skipped when running in debugger.";
    return;
  }

  base::Process test_process =
      chrome_cleaner::LongRunningProcess(/*command_line=*/nullptr);
  ASSERT_TRUE(test_process.IsValid());

  // Set up the process protector.
  chrome_cleaner::ScopedProcessProtector process_protector(test_process.Pid());
  EXPECT_TRUE(process_protector.Initialized());

  // We should no longer be able to kill it.
  EXPECT_EQ(SandboxTerminateProcess(test_process.Pid()),
            TerminateProcessResult::kFailed);

  // Double check the process is still around.
  DWORD exit_code = 420042;
  EXPECT_EQ(TRUE, ::GetExitCodeProcess(test_process.Handle(), &exit_code));
  EXPECT_EQ(STILL_ACTIVE, exit_code);

  // Unprotect the process and kill it.
  process_protector.Release();
  EXPECT_EQ(SandboxTerminateProcess(test_process.Pid()),
            TerminateProcessResult::kSuccess);

  // Check the process actually exits.
  int killed_process_exit_code = 0;
  test_process.WaitForExitWithTimeout(TestTimeouts::action_timeout(),
                                      &killed_process_exit_code);
  EXPECT_EQ(1, killed_process_exit_code);

  if (killed_process_exit_code != 1) {
    // Clean up just in case.
    test_process.Terminate(2, false);
  }
}

TEST(CleanerSandboxInterface, TerminateProcessTest_ChromeProcess) {
  base::CommandLine test_process_cmd(base::CommandLine::NO_PROGRAM);
  base::Process test_process =
      chrome_cleaner::LongRunningProcess(&test_process_cmd);
  ASSERT_TRUE(test_process.IsValid());

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  base::CommandLine original_command_line = *command_line;

  command_line->AppendSwitchPath(chrome_cleaner::kChromeExePathSwitch,
                                 test_process_cmd.GetProgram());

  EXPECT_EQ(SandboxTerminateProcess(test_process.Pid()),
            TerminateProcessResult::kDenied);

  // Make sure the process is actually still running.
  DWORD exit_code = 4711;
  EXPECT_EQ(TRUE, ::GetExitCodeProcess(test_process.Handle(), &exit_code));
  EXPECT_EQ(STILL_ACTIVE, exit_code);

  test_process.Terminate(0, false);
  *command_line = original_command_line;
}

TEST(CleanerSandboxInterface, TerminateProcessTest_RestrictedProcesses) {
  EXPECT_EQ(SandboxTerminateProcess(::GetCurrentProcessId()),
            TerminateProcessResult::kDenied);

  // 0 is System Idle Process and we shouldn't be able to terminate it.
  EXPECT_EQ(SandboxTerminateProcess(0), TerminateProcessResult::kFailed);
}

}  // namespace chrome_cleaner_sandbox
