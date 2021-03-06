// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <vector>

#include <glib.h>

#include "base/string_util.h"
#include <base/stringprintf.h>
#include "base/time.h"
#include "gtest/gtest.h"
#include "update_engine/action_pipe.h"
#include "update_engine/mock_http_fetcher.h"
#include "update_engine/omaha_hash_calculator.h"
#include "update_engine/omaha_request_action.h"
#include "update_engine/omaha_request_params.h"
#include "update_engine/prefs.h"
#include "update_engine/test_utils.h"
#include "update_engine/utils.h"

using base::Time;
using base::TimeDelta;
using std::string;
using std::vector;
using testing::_;
using testing::AllOf;
using testing::DoAll;
using testing::Ge;
using testing::Le;
using testing::NiceMock;
using testing::Return;
using testing::SetArgumentPointee;

namespace chromeos_update_engine {

class OmahaRequestActionTest : public ::testing::Test {};

namespace {

MockSystemState mock_system_state;
OmahaRequestParams kDefaultTestParams(
    &mock_system_state,
    OmahaRequestParams::kOsPlatform,
    OmahaRequestParams::kOsVersion,
    "service_pack",
    "x86-generic",
    OmahaRequestParams::kAppId,
    "0.1.0.0",
    "en-US",
    "unittest",
    "OEM MODEL 09235 7471",
    "{8DA4B84F-2864-447D-84B7-C2D9B72924E7}",
    false,  // delta okay
    false,  // interactive
    "http://url",
    false, // update_disabled
    ""); // target_version_prefix);

string GetNoUpdateResponse(const string& app_id) {
  return string(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><response protocol=\"3.0\">"
      "<daystart elapsed_seconds=\"100\"/>"
      "<app appid=\"") + app_id + "\" status=\"ok\"><ping "
      "status=\"ok\"/><updatecheck status=\"noupdate\"/></app></response>";
}

string GetUpdateResponse2(const string& app_id,
                          const string& display_version,
                          const string& more_info_url,
                          const string& prompt,
                          const string& codebase,
                          const string& filename,
                          const string& hash,
                          const string& needsadmin,
                          const string& size,
                          const string& deadline,
                          const string& max_days_to_scatter) {
  string response =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><response "
      "protocol=\"3.0\">"
      "<daystart elapsed_seconds=\"100\"/>"
      "<app appid=\"" + app_id + "\" status=\"ok\">"
      "<ping status=\"ok\"/><updatecheck status=\"ok\">"
      "<urls><url codebase=\"" + codebase + "\"/></urls>"
      "<manifest version=\"" + display_version + "\">"
      "<packages><package hash=\"not-used\" name=\"" + filename +  "\" "
      "size=\"" + size + "\"/></packages>"
      "<actions><action event=\"postinstall\" "
      "DisplayVersion=\"" + display_version + "\" "
      "ChromeOSVersion=\"" + display_version + "\" "
      "MoreInfo=\"" + more_info_url + "\" Prompt=\"" + prompt + "\" "
      "IsDelta=\"true\" "
      "IsDeltaPayload=\"true\" "
      "MaxDaysToScatter=\"" + max_days_to_scatter + "\" "
      "sha256=\"" + hash + "\" "
      "needsadmin=\"" + needsadmin + "\" " +
      (deadline.empty() ? "" : ("deadline=\"" + deadline + "\" ")) +
      "/></actions></manifest></updatecheck></app></response>";
  LOG(INFO) << "Response = " << response;
  return response;
}

string GetUpdateResponse(const string& app_id,
                         const string& display_version,
                         const string& more_info_url,
                         const string& prompt,
                         const string& codebase,
                         const string& filename,
                         const string& hash,
                         const string& needsadmin,
                         const string& size,
                         const string& deadline) {
  return GetUpdateResponse2(app_id,
                            display_version,
                            more_info_url,
                            prompt,
                            codebase,
                            filename,
                            hash,
                            needsadmin,
                            size,
                            deadline,
                            "7");
}

class OmahaRequestActionTestProcessorDelegate : public ActionProcessorDelegate {
 public:
  OmahaRequestActionTestProcessorDelegate()
      : loop_(NULL),
        expected_code_(kActionCodeSuccess) {}
  virtual ~OmahaRequestActionTestProcessorDelegate() {
  }
  virtual void ProcessingDone(const ActionProcessor* processor,
                              ActionExitCode code) {
    ASSERT_TRUE(loop_);
    g_main_loop_quit(loop_);
  }

  virtual void ActionCompleted(ActionProcessor* processor,
                               AbstractAction* action,
                               ActionExitCode code) {
    // make sure actions always succeed
    if (action->Type() == OmahaRequestAction::StaticType())
      EXPECT_EQ(expected_code_, code);
    else
      EXPECT_EQ(kActionCodeSuccess, code);
  }
  GMainLoop *loop_;
  ActionExitCode expected_code_;
};

gboolean StartProcessorInRunLoop(gpointer data) {
  ActionProcessor *processor = reinterpret_cast<ActionProcessor*>(data);
  processor->StartProcessing();
  return FALSE;
}
}  // namespace {}

class OutputObjectCollectorAction;

template<>
class ActionTraits<OutputObjectCollectorAction> {
 public:
  // Does not take an object for input
  typedef OmahaResponse InputObjectType;
  // On success, puts the output path on output
  typedef NoneType OutputObjectType;
};

class OutputObjectCollectorAction : public Action<OutputObjectCollectorAction> {
 public:
  OutputObjectCollectorAction() : has_input_object_(false) {}
  void PerformAction() {
    // copy input object
    has_input_object_ = HasInputObject();
    if (has_input_object_)
      omaha_response_ = GetInputObject();
    processor_->ActionComplete(this, kActionCodeSuccess);
  }
  // Should never be called
  void TerminateProcessing() {
    CHECK(false);
  }
  // Debugging/logging
  static std::string StaticType() {
    return "OutputObjectCollectorAction";
  }
  std::string Type() const { return StaticType(); }
  bool has_input_object_;
  OmahaResponse omaha_response_;
};

// Returns true iff an output response was obtained from the
// OmahaRequestAction. |prefs| may be NULL, in which case a local PrefsMock is
// used. out_response may be NULL. If |fail_http_response_code| is non-negative,
// the transfer will fail with that code. |ping_only| is passed through to the
// OmahaRequestAction constructor. out_post_data may be null; if non-null, the
// post-data received by the mock HttpFetcher is returned.
bool TestUpdateCheck(PrefsInterface* prefs,
                     OmahaRequestParams params,
                     const string& http_response,
                     int fail_http_response_code,
                     bool ping_only,
                     ActionExitCode expected_code,
                     OmahaResponse* out_response,
                     vector<char>* out_post_data) {
  GMainLoop* loop = g_main_loop_new(g_main_context_default(), FALSE);
  MockHttpFetcher* fetcher = new MockHttpFetcher(http_response.data(),
                                                 http_response.size());
  if (fail_http_response_code >= 0) {
    fetcher->FailTransfer(fail_http_response_code);
  }
  MockSystemState mock_system_state;
  if (prefs)
    mock_system_state.set_prefs(prefs);
  mock_system_state.set_request_params(&params);
  OmahaRequestAction action(&mock_system_state,
                            NULL,
                            fetcher,
                            ping_only);
  OmahaRequestActionTestProcessorDelegate delegate;
  delegate.loop_ = loop;
  delegate.expected_code_ = expected_code;

  ActionProcessor processor;
  processor.set_delegate(&delegate);
  processor.EnqueueAction(&action);

  OutputObjectCollectorAction collector_action;
  BondActions(&action, &collector_action);
  processor.EnqueueAction(&collector_action);

  g_timeout_add(0, &StartProcessorInRunLoop, &processor);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  if (collector_action.has_input_object_ && out_response)
    *out_response = collector_action.omaha_response_;
  if (out_post_data)
    *out_post_data = fetcher->post_data();
  return collector_action.has_input_object_;
}

// Tests Event requests -- they should always succeed. |out_post_data|
// may be null; if non-null, the post-data received by the mock
// HttpFetcher is returned.
void TestEvent(OmahaRequestParams params,
               OmahaEvent* event,
               const string& http_response,
               vector<char>* out_post_data) {
  GMainLoop* loop = g_main_loop_new(g_main_context_default(), FALSE);
  MockHttpFetcher* fetcher = new MockHttpFetcher(http_response.data(),
                                                 http_response.size());
  MockSystemState mock_system_state;
  mock_system_state.set_request_params(&params);
  OmahaRequestAction action(&mock_system_state, event, fetcher, false);
  OmahaRequestActionTestProcessorDelegate delegate;
  delegate.loop_ = loop;
  ActionProcessor processor;
  processor.set_delegate(&delegate);
  processor.EnqueueAction(&action);

  g_timeout_add(0, &StartProcessorInRunLoop, &processor);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  if (out_post_data)
    *out_post_data = fetcher->post_data();
}

TEST(OmahaRequestActionTest, NoUpdateTest) {
  OmahaResponse response;
  ASSERT_TRUE(
      TestUpdateCheck(NULL,  // prefs
                      kDefaultTestParams,
                      GetNoUpdateResponse(OmahaRequestParams::kAppId),
                      -1,
                      false,  // ping_only
                      kActionCodeSuccess,
                      &response,
                      NULL));
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, ValidUpdateTest) {
  OmahaResponse response;
  ASSERT_TRUE(
      TestUpdateCheck(NULL,  // prefs
                      kDefaultTestParams,
                      GetUpdateResponse(OmahaRequestParams::kAppId,
                                        "1.2.3.4",  // version
                                        "http://more/info",
                                        "true",  // prompt
                                        "http://code/base/",  // dl url
                                        "file.signed", // file name
                                        "HASH1234=",  // checksum
                                        "false",  // needs admin
                                        "123",  // size
                                        "20101020"),  // deadline
                      -1,
                      false,  // ping_only
                      kActionCodeSuccess,
                      &response,
                      NULL));
  EXPECT_TRUE(response.update_exists);
  EXPECT_TRUE(response.update_exists);
  EXPECT_EQ("1.2.3.4", response.display_version);
  EXPECT_EQ("http://code/base/file.signed", response.payload_urls[0]);
  EXPECT_EQ("http://more/info", response.more_info_url);
  EXPECT_EQ("HASH1234=", response.hash);
  EXPECT_EQ(123, response.size);
  EXPECT_FALSE(response.needs_admin);
  EXPECT_TRUE(response.prompt);
  EXPECT_EQ("20101020", response.deadline);
}

TEST(OmahaRequestActionTest, ValidUpdateBlockedByPolicyTest) {
  OmahaResponse response;
  OmahaRequestParams params = kDefaultTestParams;
  params.set_update_disabled(true);
  ASSERT_FALSE(
      TestUpdateCheck(NULL,  // prefs
                      params,
                      GetUpdateResponse(OmahaRequestParams::kAppId,
                                        "1.2.3.4",  // version
                                        "http://more/info",
                                        "true",  // prompt
                                        "http://code/base/",  // dl url
                                        "file.signed", // file name
                                        "HASH1234=",  // checksum
                                        "false",  // needs admin
                                        "123",  // size
                                        ""),  // deadline
                      -1,
                      false,  // ping_only
                      kActionCodeOmahaUpdateIgnoredPerPolicy,
                      &response,
                      NULL));
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, NoUpdatesSentWhenBlockedByPolicyTest) {
  OmahaResponse response;
  OmahaRequestParams params = kDefaultTestParams;
  params.set_update_disabled(true);
  ASSERT_TRUE(
      TestUpdateCheck(NULL,  // prefs
                      params,
                      GetNoUpdateResponse(OmahaRequestParams::kAppId),
                      -1,
                      false,  // ping_only
                      kActionCodeSuccess,
                      &response,
                      NULL));
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, WallClockBasedWaitAloneCausesScattering) {
  OmahaResponse response;
  OmahaRequestParams params = kDefaultTestParams;
  params.set_wall_clock_based_wait_enabled(true);
  params.set_update_check_count_wait_enabled(false);
  params.set_waiting_period(TimeDelta::FromDays(2));

  string prefs_dir;
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/ue_ut_prefs.XXXXXX",
                                       &prefs_dir));
  ScopedDirRemover temp_dir_remover(prefs_dir);

  Prefs prefs;
  LOG_IF(ERROR, !prefs.Init(FilePath(prefs_dir)))
      << "Failed to initialize preferences.";

  ASSERT_FALSE(
      TestUpdateCheck(&prefs,  // prefs
                      params,
                      GetUpdateResponse2(OmahaRequestParams::kAppId,
                                         "1.2.3.4",  // version
                                         "http://more/info",
                                         "true",  // prompt
                                         "http://code/base/",  // dl url
                                         "file.signed", // file name
                                         "HASH1234=",  // checksum
                                         "false",  // needs admin
                                         "123",  // size
                                         "",  // deadline
                                         "7"), // max days to scatter
                      -1,
                      false,  // ping_only
                      kActionCodeOmahaUpdateDeferredPerPolicy,
                      &response,
                      NULL));
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, NoWallClockBasedWaitCausesNoScattering) {
  OmahaResponse response;
  OmahaRequestParams params = kDefaultTestParams;
  params.set_wall_clock_based_wait_enabled(false);
  params.set_waiting_period(TimeDelta::FromDays(2));

  params.set_update_check_count_wait_enabled(true);
  params.set_min_update_checks_needed(1);
  params.set_max_update_checks_allowed(8);

  string prefs_dir;
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/ue_ut_prefs.XXXXXX",
                                       &prefs_dir));
  ScopedDirRemover temp_dir_remover(prefs_dir);

  Prefs prefs;
  LOG_IF(ERROR, !prefs.Init(FilePath(prefs_dir)))
      << "Failed to initialize preferences.";

  ASSERT_TRUE(
      TestUpdateCheck(&prefs,  // prefs
                      params,
                      GetUpdateResponse2(OmahaRequestParams::kAppId,
                                         "1.2.3.4",  // version
                                         "http://more/info",
                                         "true",  // prompt
                                         "http://code/base/",  // dl url
                                         "file.signed", // file name
                                         "HASH1234=",  // checksum
                                         "false",  // needs admin
                                         "123",  // size
                                         "",  // deadline
                                         "7"), // max days to scatter
                      -1,
                      false,  // ping_only
                      kActionCodeSuccess,
                      &response,
                      NULL));
  EXPECT_TRUE(response.update_exists);
}

TEST(OmahaRequestActionTest, ZeroMaxDaysToScatterCausesNoScattering) {
  OmahaResponse response;
  OmahaRequestParams params = kDefaultTestParams;
  params.set_wall_clock_based_wait_enabled(true);
  params.set_waiting_period(TimeDelta::FromDays(2));

  params.set_update_check_count_wait_enabled(true);
  params.set_min_update_checks_needed(1);
  params.set_max_update_checks_allowed(8);

  string prefs_dir;
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/ue_ut_prefs.XXXXXX",
                                       &prefs_dir));
  ScopedDirRemover temp_dir_remover(prefs_dir);

  Prefs prefs;
  LOG_IF(ERROR, !prefs.Init(FilePath(prefs_dir)))
      << "Failed to initialize preferences.";

  ASSERT_TRUE(
      TestUpdateCheck(&prefs,  // prefs
                      params,
                      GetUpdateResponse2(OmahaRequestParams::kAppId,
                                         "1.2.3.4",  // version
                                         "http://more/info",
                                         "true",  // prompt
                                         "http://code/base/",  // dl url
                                         "file.signed", // file name
                                         "HASH1234=",  // checksum
                                         "false",  // needs admin
                                         "123",  // size
                                         "",  // deadline
                                         "0"), // max days to scatter
                      -1,
                      false,  // ping_only
                      kActionCodeSuccess,
                      &response,
                      NULL));
  EXPECT_TRUE(response.update_exists);
}


TEST(OmahaRequestActionTest, ZeroUpdateCheckCountCausesNoScattering) {
  OmahaResponse response;
  OmahaRequestParams params = kDefaultTestParams;
  params.set_wall_clock_based_wait_enabled(true);
  params.set_waiting_period(TimeDelta());

  params.set_update_check_count_wait_enabled(true);
  params.set_min_update_checks_needed(0);
  params.set_max_update_checks_allowed(0);

  string prefs_dir;
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/ue_ut_prefs.XXXXXX",
                                       &prefs_dir));
  ScopedDirRemover temp_dir_remover(prefs_dir);

  Prefs prefs;
  LOG_IF(ERROR, !prefs.Init(FilePath(prefs_dir)))
      << "Failed to initialize preferences.";

  ASSERT_TRUE(TestUpdateCheck(
                      &prefs,  // prefs
                      params,
                      GetUpdateResponse2(OmahaRequestParams::kAppId,
                                         "1.2.3.4",  // version
                                         "http://more/info",
                                         "true",  // prompt
                                         "http://code/base/",  // dl url
                                         "file.signed", // file name
                                         "HASH1234=",  // checksum
                                         "false",  // needs admin
                                         "123",  // size
                                         "",  // deadline
                                         "7"), // max days to scatter
                      -1,
                      false,  // ping_only
                      kActionCodeSuccess,
                      &response,
                      NULL));

  int64 count;
  ASSERT_TRUE(prefs.GetInt64(kPrefsUpdateCheckCount, &count));
  ASSERT_TRUE(count == 0);
  EXPECT_TRUE(response.update_exists);
}

TEST(OmahaRequestActionTest, NonZeroUpdateCheckCountCausesScattering) {
  OmahaResponse response;
  OmahaRequestParams params = kDefaultTestParams;
  params.set_wall_clock_based_wait_enabled(true);
  params.set_waiting_period(TimeDelta());

  params.set_update_check_count_wait_enabled(true);
  params.set_min_update_checks_needed(1);
  params.set_max_update_checks_allowed(8);

  string prefs_dir;
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/ue_ut_prefs.XXXXXX",
                                       &prefs_dir));
  ScopedDirRemover temp_dir_remover(prefs_dir);

  Prefs prefs;
  LOG_IF(ERROR, !prefs.Init(FilePath(prefs_dir)))
      << "Failed to initialize preferences.";

  ASSERT_FALSE(TestUpdateCheck(
                      &prefs,  // prefs
                      params,
                      GetUpdateResponse2(OmahaRequestParams::kAppId,
                                         "1.2.3.4",  // version
                                         "http://more/info",
                                         "true",  // prompt
                                         "http://code/base/",  // dl url
                                         "file.signed", // file name
                                         "HASH1234=",  // checksum
                                         "false",  // needs admin
                                         "123",  // size
                                         "",  // deadline
                                         "7"), // max days to scatter
                      -1,
                      false,  // ping_only
                      kActionCodeOmahaUpdateDeferredPerPolicy,
                      &response,
                      NULL));

  int64 count;
  ASSERT_TRUE(prefs.GetInt64(kPrefsUpdateCheckCount, &count));
  ASSERT_TRUE(count > 0);
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, ExistingUpdateCheckCountCausesScattering) {
  OmahaResponse response;
  OmahaRequestParams params = kDefaultTestParams;
  params.set_wall_clock_based_wait_enabled(true);
  params.set_waiting_period(TimeDelta());

  params.set_update_check_count_wait_enabled(true);
  params.set_min_update_checks_needed(1);
  params.set_max_update_checks_allowed(8);

  string prefs_dir;
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/ue_ut_prefs.XXXXXX",
                                       &prefs_dir));
  ScopedDirRemover temp_dir_remover(prefs_dir);

  Prefs prefs;
  LOG_IF(ERROR, !prefs.Init(FilePath(prefs_dir)))
      << "Failed to initialize preferences.";

  ASSERT_TRUE(prefs.SetInt64(kPrefsUpdateCheckCount, 5));

  ASSERT_FALSE(TestUpdateCheck(
                      &prefs,  // prefs
                      params,
                      GetUpdateResponse2(OmahaRequestParams::kAppId,
                                         "1.2.3.4",  // version
                                         "http://more/info",
                                         "true",  // prompt
                                         "http://code/base/",  // dl url
                                         "file.signed", // file name
                                         "HASH1234=",  // checksum
                                         "false",  // needs admin
                                         "123",  // size
                                         "",  // deadline
                                         "7"), // max days to scatter
                      -1,
                      false,  // ping_only
                      kActionCodeOmahaUpdateDeferredPerPolicy,
                      &response,
                      NULL));

  int64 count;
  ASSERT_TRUE(prefs.GetInt64(kPrefsUpdateCheckCount, &count));
  // count remains the same, as the decrementing happens in update_attempter
  // which this test doesn't exercise.
  ASSERT_TRUE(count == 5);
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, NoOutputPipeTest) {
  const string http_response(GetNoUpdateResponse(OmahaRequestParams::kAppId));

  GMainLoop *loop = g_main_loop_new(g_main_context_default(), FALSE);

  MockSystemState mock_system_state;
  OmahaRequestParams params = kDefaultTestParams;
  mock_system_state.set_request_params(&params);
  OmahaRequestAction action(&mock_system_state, NULL,
                            new MockHttpFetcher(http_response.data(),
                                                http_response.size()),
                            false);
  OmahaRequestActionTestProcessorDelegate delegate;
  delegate.loop_ = loop;
  ActionProcessor processor;
  processor.set_delegate(&delegate);
  processor.EnqueueAction(&action);

  g_timeout_add(0, &StartProcessorInRunLoop, &processor);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  EXPECT_FALSE(processor.IsRunning());
}

TEST(OmahaRequestActionTest, InvalidXmlTest) {
  OmahaResponse response;
  ASSERT_FALSE(
      TestUpdateCheck(NULL,  // prefs
                      kDefaultTestParams,
                      "invalid xml>",
                      -1,
                      false,  // ping_only
                      kActionCodeOmahaRequestXMLParseError,
                      &response,
                      NULL));
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, EmptyResponseTest) {
  OmahaResponse response;
  ASSERT_FALSE(
      TestUpdateCheck(NULL,  // prefs
                      kDefaultTestParams,
                      "",
                      -1,
                      false,  // ping_only
                      kActionCodeOmahaRequestEmptyResponseError,
                      &response,
                      NULL));
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, MissingStatusTest) {
  OmahaResponse response;
  ASSERT_FALSE(TestUpdateCheck(
      NULL,  // prefs
      kDefaultTestParams,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><response protocol=\"3.0\">"
      "<daystart elapsed_seconds=\"100\"/>"
      "<app appid=\"foo\" status=\"ok\">"
      "<ping status=\"ok\"/>"
      "<updatecheck/></app></response>",
      -1,
      false,  // ping_only
      kActionCodeOmahaResponseInvalid,
      &response,
      NULL));
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, InvalidStatusTest) {
  OmahaResponse response;
  ASSERT_FALSE(TestUpdateCheck(
      NULL,  // prefs
      kDefaultTestParams,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><response protocol=\"3.0\">"
      "<daystart elapsed_seconds=\"100\"/>"
      "<app appid=\"foo\" status=\"ok\">"
      "<ping status=\"ok\"/>"
      "<updatecheck status=\"InvalidStatusTest\"/></app></response>",
      -1,
      false,  // ping_only
      kActionCodeOmahaResponseInvalid,
      &response,
      NULL));
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, MissingNodesetTest) {
  OmahaResponse response;
  ASSERT_FALSE(TestUpdateCheck(
      NULL,  // prefs
      kDefaultTestParams,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><response protocol=\"3.0\">"
      "<daystart elapsed_seconds=\"100\"/>"
      "<app appid=\"foo\" status=\"ok\">"
      "<ping status=\"ok\"/>"
      "</app></response>",
      -1,
      false,  // ping_only
      kActionCodeOmahaResponseInvalid,
      &response,
      NULL));
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, MissingFieldTest) {
  string input_response =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><response protocol=\"3.0\">"
      "<daystart elapsed_seconds=\"100\"/>"
      "<app appid=\"xyz\" status=\"ok\">"
      "<updatecheck status=\"ok\">"
      "<urls><url codebase=\"http://missing/field/test/\"/></urls>"
      "<manifest version=\"1.0.0.0\">"
      "<packages><package hash=\"not-used\" name=\"f\" "
      "size=\"587\"/></packages>"
      "<actions><action event=\"postinstall\" "
      "DisplayVersion=\"10.2.3.4\" "
      "ChromeOSVersion=\"10.2.3.4\" "
      "Prompt=\"false\" "
      "IsDelta=\"true\" "
      "IsDeltaPayload=\"false\" "
      "sha256=\"lkq34j5345\" "
      "needsadmin=\"true\" "
      "/></actions></manifest></updatecheck></app></response>";
  LOG(INFO) << "Input Response = " << input_response;

  OmahaResponse response;
  ASSERT_TRUE(TestUpdateCheck(NULL,  // prefs
                              kDefaultTestParams,
                              input_response,
                              -1,
                              false,  // ping_only
                              kActionCodeSuccess,
                              &response,
                              NULL));
  EXPECT_TRUE(response.update_exists);
  EXPECT_EQ("10.2.3.4", response.display_version);
  EXPECT_EQ("http://missing/field/test/f", response.payload_urls[0]);
  EXPECT_EQ("", response.more_info_url);
  EXPECT_EQ("lkq34j5345", response.hash);
  EXPECT_EQ(587, response.size);
  EXPECT_TRUE(response.needs_admin);
  EXPECT_FALSE(response.prompt);
  EXPECT_TRUE(response.deadline.empty());
}

namespace {
class TerminateEarlyTestProcessorDelegate : public ActionProcessorDelegate {
 public:
  void ProcessingStopped(const ActionProcessor* processor) {
    ASSERT_TRUE(loop_);
    g_main_loop_quit(loop_);
  }
  GMainLoop *loop_;
};

gboolean TerminateTransferTestStarter(gpointer data) {
  ActionProcessor *processor = reinterpret_cast<ActionProcessor*>(data);
  processor->StartProcessing();
  CHECK(processor->IsRunning());
  processor->StopProcessing();
  return FALSE;
}
}  // namespace {}

TEST(OmahaRequestActionTest, TerminateTransferTest) {
  string http_response("doesn't matter");
  GMainLoop *loop = g_main_loop_new(g_main_context_default(), FALSE);

  MockSystemState mock_system_state;
  OmahaRequestParams params = kDefaultTestParams;
  mock_system_state.set_request_params(&params);
  OmahaRequestAction action(&mock_system_state, NULL,
                            new MockHttpFetcher(http_response.data(),
                                                http_response.size()),
                            false);
  TerminateEarlyTestProcessorDelegate delegate;
  delegate.loop_ = loop;
  ActionProcessor processor;
  processor.set_delegate(&delegate);
  processor.EnqueueAction(&action);

  g_timeout_add(0, &TerminateTransferTestStarter, &processor);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
}

TEST(OmahaRequestActionTest, XmlEncodeTest) {
  EXPECT_EQ("ab", XmlEncode("ab"));
  EXPECT_EQ("a&lt;b", XmlEncode("a<b"));
  EXPECT_EQ("foo-&#x3A9;", XmlEncode("foo-\xce\xa9"));
  EXPECT_EQ("&lt;&amp;&gt;", XmlEncode("<&>"));
  EXPECT_EQ("&amp;lt;&amp;amp;&amp;gt;", XmlEncode("&lt;&amp;&gt;"));

  vector<char> post_data;

  // Make sure XML Encode is being called on the params
  MockSystemState mock_system_state;
  OmahaRequestParams params(&mock_system_state,
                            OmahaRequestParams::kOsPlatform,
                            OmahaRequestParams::kOsVersion,
                            "testtheservice_pack>",
                            "x86 generic<id",
                            OmahaRequestParams::kAppId,
                            "0.1.0.0",
                            "en-US",
                            "unittest_track&lt;",
                            "<OEM MODEL>",
                            "{8DA4B84F-2864-447D-84B7-C2D9B72924E7}",
                            false,  // delta okay
                            false,  // interactive
                            "http://url",
                            false,   // update_disabled
                            "");  // target_version_prefix
  OmahaResponse response;
  ASSERT_FALSE(
      TestUpdateCheck(NULL,  // prefs
                      params,
                      "invalid xml>",
                      -1,
                      false,  // ping_only
                      kActionCodeOmahaRequestXMLParseError,
                      &response,
                      &post_data));
  // convert post_data to string
  string post_str(&post_data[0], post_data.size());
  EXPECT_NE(post_str.find("testtheservice_pack&gt;"), string::npos);
  EXPECT_EQ(post_str.find("testtheservice_pack>"), string::npos);
  EXPECT_NE(post_str.find("x86 generic&lt;id"), string::npos);
  EXPECT_EQ(post_str.find("x86 generic<id"), string::npos);
  EXPECT_NE(post_str.find("unittest_track&amp;lt;"), string::npos);
  EXPECT_EQ(post_str.find("unittest_track&lt;"), string::npos);
  EXPECT_NE(post_str.find("&lt;OEM MODEL&gt;"), string::npos);
  EXPECT_EQ(post_str.find("<OEM MODEL>"), string::npos);
}

TEST(OmahaRequestActionTest, XmlDecodeTest) {
  OmahaResponse response;
  ASSERT_TRUE(
      TestUpdateCheck(NULL,  // prefs
                      kDefaultTestParams,
                      GetUpdateResponse(OmahaRequestParams::kAppId,
                                        "1.2.3.4",  // version
                                        "testthe&lt;url",  // more info
                                        "true",  // prompt
                                        "testthe&amp;codebase/",  // dl url
                                        "file.signed", // file name
                                        "HASH1234=", // checksum
                                        "false",  // needs admin
                                        "123",  // size
                                        "&lt;20110101"),  // deadline
                      -1,
                      false,  // ping_only
                      kActionCodeSuccess,
                      &response,
                      NULL));

  EXPECT_EQ(response.more_info_url, "testthe<url");
  EXPECT_EQ(response.payload_urls[0], "testthe&codebase/file.signed");
  EXPECT_EQ(response.deadline, "<20110101");
}

TEST(OmahaRequestActionTest, ParseIntTest) {
  OmahaResponse response;
  ASSERT_TRUE(
      TestUpdateCheck(NULL,  // prefs
                      kDefaultTestParams,
                      GetUpdateResponse(OmahaRequestParams::kAppId,
                                        "1.2.3.4",  // version
                                        "theurl",  // more info
                                        "true",  // prompt
                                        "thecodebase/",  // dl url
                                        "file.signed", // file name
                                        "HASH1234=", // checksum
                                        "false",  // needs admin
                                        // overflows int32:
                                        "123123123123123",  // size
                                        "deadline"),
                      -1,
                      false,  // ping_only
                      kActionCodeSuccess,
                      &response,
                      NULL));

  EXPECT_EQ(response.size, 123123123123123ll);
}

TEST(OmahaRequestActionTest, FormatUpdateCheckOutputTest) {
  vector<char> post_data;
  NiceMock<PrefsMock> prefs;
  EXPECT_CALL(prefs, GetString(kPrefsPreviousVersion, _))
      .WillOnce(DoAll(SetArgumentPointee<1>(string("")), Return(true)));
  EXPECT_CALL(prefs, SetString(kPrefsPreviousVersion, _)).Times(1);
  ASSERT_FALSE(TestUpdateCheck(&prefs,
                               kDefaultTestParams,
                               "invalid xml>",
                               -1,
                               false,  // ping_only
                               kActionCodeOmahaRequestXMLParseError,
                               NULL,  // response
                               &post_data));
  // convert post_data to string
  string post_str(&post_data[0], post_data.size());
  EXPECT_NE(post_str.find(
      "        <ping active=\"1\"></ping>\n"
      "        <updatecheck targetversionprefix=\"\"></updatecheck>\n"),
      string::npos);
  EXPECT_NE(post_str.find("hardware_class=\"OEM MODEL 09235 7471\""),
            string::npos);
  EXPECT_NE(post_str.find("bootid=\"{8DA4B84F-2864-447D-84B7-C2D9B72924E7}\""),
            string::npos);
}


TEST(OmahaRequestActionTest, FormatUpdateDisabledOutputTest) {
  vector<char> post_data;
  NiceMock<PrefsMock> prefs;
  EXPECT_CALL(prefs, GetString(kPrefsPreviousVersion, _))
      .WillOnce(DoAll(SetArgumentPointee<1>(string("")), Return(true)));
  EXPECT_CALL(prefs, SetString(kPrefsPreviousVersion, _)).Times(1);
  OmahaRequestParams params = kDefaultTestParams;
  params.set_update_disabled(true);
  ASSERT_FALSE(TestUpdateCheck(&prefs,
                               params,
                               "invalid xml>",
                               -1,
                               false,  // ping_only
                               kActionCodeOmahaRequestXMLParseError,
                               NULL,  // response
                               &post_data));
  // convert post_data to string
  string post_str(&post_data[0], post_data.size());
  EXPECT_NE(post_str.find(
      "        <ping active=\"1\"></ping>\n"
      "        <updatecheck targetversionprefix=\"\"></updatecheck>\n"),
      string::npos);
  EXPECT_NE(post_str.find("hardware_class=\"OEM MODEL 09235 7471\""),
            string::npos);
  EXPECT_NE(post_str.find("bootid=\"{8DA4B84F-2864-447D-84B7-C2D9B72924E7}\""),
            string::npos);
}

TEST(OmahaRequestActionTest, FormatSuccessEventOutputTest) {
  vector<char> post_data;
  TestEvent(kDefaultTestParams,
            new OmahaEvent(OmahaEvent::kTypeUpdateDownloadStarted),
            "invalid xml>",
            &post_data);
  // convert post_data to string
  string post_str(&post_data[0], post_data.size());
  string expected_event = StringPrintf(
      "        <event eventtype=\"%d\" eventresult=\"%d\"></event>\n",
      OmahaEvent::kTypeUpdateDownloadStarted,
      OmahaEvent::kResultSuccess);
  EXPECT_NE(post_str.find(expected_event), string::npos);
  EXPECT_EQ(post_str.find("ping"), string::npos);
  EXPECT_EQ(post_str.find("updatecheck"), string::npos);
}

TEST(OmahaRequestActionTest, FormatErrorEventOutputTest) {
  vector<char> post_data;
  TestEvent(kDefaultTestParams,
            new OmahaEvent(OmahaEvent::kTypeDownloadComplete,
                           OmahaEvent::kResultError,
                           kActionCodeError),
            "invalid xml>",
            &post_data);
  // convert post_data to string
  string post_str(&post_data[0], post_data.size());
  string expected_event = StringPrintf(
      "        <event eventtype=\"%d\" eventresult=\"%d\" "
      "errorcode=\"%d\"></event>\n",
      OmahaEvent::kTypeDownloadComplete,
      OmahaEvent::kResultError,
      kActionCodeError);
  EXPECT_NE(post_str.find(expected_event), string::npos);
  EXPECT_EQ(post_str.find("updatecheck"), string::npos);
}

TEST(OmahaRequestActionTest, IsEventTest) {
  string http_response("doesn't matter");
  MockSystemState mock_system_state;
  OmahaRequestParams params = kDefaultTestParams;
  mock_system_state.set_request_params(&params);
  OmahaRequestAction update_check_action(
      &mock_system_state,
      NULL,
      new MockHttpFetcher(http_response.data(),
                          http_response.size()),
      false);
  EXPECT_FALSE(update_check_action.IsEvent());

  params = kDefaultTestParams;
  mock_system_state.set_request_params(&params);
  OmahaRequestAction event_action(
      &mock_system_state,
      new OmahaEvent(OmahaEvent::kTypeUpdateComplete),
      new MockHttpFetcher(http_response.data(),
                          http_response.size()),
      false);
  EXPECT_TRUE(event_action.IsEvent());
}

TEST(OmahaRequestActionTest, FormatDeltaOkayOutputTest) {
  for (int i = 0; i < 2; i++) {
    bool delta_okay = i == 1;
    const char* delta_okay_str = delta_okay ? "true" : "false";
    vector<char> post_data;
    MockSystemState mock_system_state;
    OmahaRequestParams params(&mock_system_state,
                              OmahaRequestParams::kOsPlatform,
                              OmahaRequestParams::kOsVersion,
                              "service_pack",
                              "x86-generic",
                              OmahaRequestParams::kAppId,
                              "0.1.0.0",
                              "en-US",
                              "unittest_track",
                              "OEM MODEL REV 1234",
                              "{88DC1453-ABB2-45F5-A622-1808F18E1B61}",
                              delta_okay,
                              false,  // interactive
                              "http://url",
                              false, // update_disabled
                              "");   // target_version_prefix
    ASSERT_FALSE(TestUpdateCheck(NULL,  // prefs
                                 params,
                                 "invalid xml>",
                                 -1,
                                 false,  // ping_only
                                 kActionCodeOmahaRequestXMLParseError,
                                 NULL,
                                 &post_data));
    // convert post_data to string
    string post_str(&post_data[0], post_data.size());
    EXPECT_NE(post_str.find(StringPrintf(" delta_okay=\"%s\"", delta_okay_str)),
              string::npos)
        << "i = " << i;
  }
}

TEST(OmahaRequestActionTest, FormatInteractiveOutputTest) {
  for (int i = 0; i < 2; i++) {
    bool interactive = i == 1;
    const char* interactive_str = interactive ? "ondemandupdate" : "scheduler";
    vector<char> post_data;
    MockSystemState mock_system_state;
    OmahaRequestParams params(&mock_system_state,
                              OmahaRequestParams::kOsPlatform,
                              OmahaRequestParams::kOsVersion,
                              "service_pack",
                              "x86-generic",
                              OmahaRequestParams::kAppId,
                              "0.1.0.0",
                              "en-US",
                              "unittest_track",
                              "OEM MODEL REV 1234",
                              "{88DC1453-ABB2-45F5-A622-1808F18E1B61}",
                              true,  // delta_okay
                              interactive,
                              "http://url",
                              false, // update_disabled
                              "");   // target_version_prefix
    ASSERT_FALSE(TestUpdateCheck(NULL,  // prefs
                                 params,
                                 "invalid xml>",
                                 -1,
                                 false,  // ping_only
                                 kActionCodeOmahaRequestXMLParseError,
                                 NULL,
                                 &post_data));
    // convert post_data to string
    string post_str(&post_data[0], post_data.size());
    EXPECT_NE(post_str.find(StringPrintf("installsource=\"%s\"",
                                         interactive_str)),
              string::npos)
        << "i = " << i;
  }
}

TEST(OmahaRequestActionTest, OmahaEventTest) {
  OmahaEvent default_event;
  EXPECT_EQ(OmahaEvent::kTypeUnknown, default_event.type);
  EXPECT_EQ(OmahaEvent::kResultError, default_event.result);
  EXPECT_EQ(kActionCodeError, default_event.error_code);

  OmahaEvent success_event(OmahaEvent::kTypeUpdateDownloadStarted);
  EXPECT_EQ(OmahaEvent::kTypeUpdateDownloadStarted, success_event.type);
  EXPECT_EQ(OmahaEvent::kResultSuccess, success_event.result);
  EXPECT_EQ(kActionCodeSuccess, success_event.error_code);

  OmahaEvent error_event(OmahaEvent::kTypeUpdateDownloadFinished,
                         OmahaEvent::kResultError,
                         kActionCodeError);
  EXPECT_EQ(OmahaEvent::kTypeUpdateDownloadFinished, error_event.type);
  EXPECT_EQ(OmahaEvent::kResultError, error_event.result);
  EXPECT_EQ(kActionCodeError, error_event.error_code);
}

TEST(OmahaRequestActionTest, PingTest) {
  for (int ping_only = 0; ping_only < 2; ping_only++) {
    NiceMock<PrefsMock> prefs;
    // Add a few hours to the day difference to test no rounding, etc.
    vector<char> post_data;
    ASSERT_TRUE(
        TestUpdateCheck(&prefs,
                        kDefaultTestParams,
                        GetNoUpdateResponse(OmahaRequestParams::kAppId),
                        -1,
                        ping_only,
                        kActionCodeSuccess,
                        NULL,
                        &post_data));
    string post_str(&post_data[0], post_data.size());
    EXPECT_NE(post_str.find("<ping active=\"1\"></ping>"),
              string::npos);
    if (ping_only) {
      EXPECT_EQ(post_str.find("updatecheck"), string::npos);
      EXPECT_EQ(post_str.find("previousversion"), string::npos);
    } else {
      EXPECT_NE(post_str.find("updatecheck"), string::npos);
      EXPECT_NE(post_str.find("previousversion"), string::npos);
    }
  }
}

TEST(OmahaRequestActionTest, ActivePingTest) {
  NiceMock<PrefsMock> prefs;
  vector<char> post_data;
  ASSERT_TRUE(
      TestUpdateCheck(&prefs,
                      kDefaultTestParams,
                      GetNoUpdateResponse(OmahaRequestParams::kAppId),
                      -1,
                      false,  // ping_only
                      kActionCodeSuccess,
                      NULL,
                      &post_data));
  string post_str(&post_data[0], post_data.size());
  EXPECT_NE(post_str.find("<ping active=\"1\"></ping>"),
            string::npos);
}

TEST(OmahaRequestActionTest, NoElapsedSecondsTest) {
  NiceMock<PrefsMock> prefs;
  ASSERT_TRUE(
      TestUpdateCheck(&prefs,
                      kDefaultTestParams,
                      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><response "
                      "protocol=\"3.0\"><daystart blah=\"200\"/>"
                      "<app appid=\"foo\" status=\"ok\"><ping status=\"ok\"/>"
                      "<updatecheck status=\"noupdate\"/></app></response>",
                      -1,
                      false,  // ping_only
                      kActionCodeSuccess,
                      NULL,
                      NULL));
}

TEST(OmahaRequestActionTest, BadElapsedSecondsTest) {
  NiceMock<PrefsMock> prefs;
  ASSERT_TRUE(
      TestUpdateCheck(&prefs,
                      kDefaultTestParams,
                      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><response "
                      "protocol=\"3.0\"><daystart elapsed_seconds=\"x\"/>"
                      "<app appid=\"foo\" status=\"ok\"><ping status=\"ok\"/>"
                      "<updatecheck status=\"noupdate\"/></app></response>",
                      -1,
                      false,  // ping_only
                      kActionCodeSuccess,
                      NULL,
                      NULL));
}

TEST(OmahaRequestActionTest, NetworkFailureTest) {
  OmahaResponse response;
  ASSERT_FALSE(
      TestUpdateCheck(NULL,  // prefs
                      kDefaultTestParams,
                      "",
                      501,
                      false,  // ping_only
                      static_cast<ActionExitCode>(
                          kActionCodeOmahaRequestHTTPResponseBase + 501),
                      &response,
                      NULL));
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, NetworkFailureBadHTTPCodeTest) {
  OmahaResponse response;
  ASSERT_FALSE(
      TestUpdateCheck(NULL,  // prefs
                      kDefaultTestParams,
                      "",
                      1500,
                      false,  // ping_only
                      static_cast<ActionExitCode>(
                          kActionCodeOmahaRequestHTTPResponseBase + 999),
                      &response,
                      NULL));
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, TestUpdateFirstSeenAtGetsPersistedFirstTime) {
  OmahaResponse response;
  OmahaRequestParams params = kDefaultTestParams;
  params.set_wall_clock_based_wait_enabled(true);
  params.set_waiting_period(TimeDelta().FromDays(1));
  params.set_update_check_count_wait_enabled(false);

  string prefs_dir;
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/ue_ut_prefs.XXXXXX",
                                       &prefs_dir));
  ScopedDirRemover temp_dir_remover(prefs_dir);

  Prefs prefs;
  LOG_IF(ERROR, !prefs.Init(FilePath(prefs_dir)))
      << "Failed to initialize preferences.";

  ASSERT_FALSE(TestUpdateCheck(
                      &prefs,  // prefs
                      params,
                      GetUpdateResponse2(OmahaRequestParams::kAppId,
                                         "1.2.3.4",  // version
                                         "http://more/info",
                                         "true",  // prompt
                                         "http://code/base/",  // dl url
                                         "file.signed", // file name
                                         "HASH1234=",  // checksum
                                         "false",  // needs admin
                                         "123",  // size
                                         "",  // deadline
                                         "7"), // max days to scatter
                      -1,
                      false,  // ping_only
                      kActionCodeOmahaUpdateDeferredPerPolicy,
                      &response,
                      NULL));

  int64 timestamp = 0;
  ASSERT_TRUE(prefs.GetInt64(kPrefsUpdateFirstSeenAt, &timestamp));
  ASSERT_TRUE(timestamp > 0);
  EXPECT_FALSE(response.update_exists);
}

TEST(OmahaRequestActionTest, TestUpdateFirstSeenAtGetsUsedIfAlreadyPresent) {
  OmahaResponse response;
  OmahaRequestParams params = kDefaultTestParams;
  params.set_wall_clock_based_wait_enabled(true);
  params.set_waiting_period(TimeDelta().FromDays(1));
  params.set_update_check_count_wait_enabled(false);

  string prefs_dir;
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/ue_ut_prefs.XXXXXX",
                                       &prefs_dir));
  ScopedDirRemover temp_dir_remover(prefs_dir);

  Prefs prefs;
  LOG_IF(ERROR, !prefs.Init(FilePath(prefs_dir)))
      << "Failed to initialize preferences.";

  // Set the timestamp to a very old value such that it exceeds the
  // waiting period set above.
  Time t1;
  Time::FromString("1/1/2012", &t1);
  ASSERT_TRUE(prefs.SetInt64(kPrefsUpdateFirstSeenAt, t1.ToInternalValue()));
  ASSERT_TRUE(TestUpdateCheck(
                      &prefs,  // prefs
                      params,
                      GetUpdateResponse2(OmahaRequestParams::kAppId,
                                         "1.2.3.4",  // version
                                         "http://more/info",
                                         "true",  // prompt
                                         "http://code/base/",  // dl url
                                         "file.signed", // file name
                                         "HASH1234=",  // checksum
                                         "false",  // needs admin
                                         "123",  // size
                                         "",  // deadline
                                         "7"), // max days to scatter
                      -1,
                      false,  // ping_only
                      kActionCodeSuccess,
                      &response,
                      NULL));

  EXPECT_TRUE(response.update_exists);

  // Make sure the timestamp t1 is unchanged showing that it was reused.
  int64 timestamp = 0;
  ASSERT_TRUE(prefs.GetInt64(kPrefsUpdateFirstSeenAt, &timestamp));
  ASSERT_TRUE(timestamp == t1.ToInternalValue());
}

}  // namespace chromeos_update_engine
