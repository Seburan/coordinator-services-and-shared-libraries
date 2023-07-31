/*
 * Copyright 2022 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "core/http2_server/src/http2_server.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>

#include "core/async_executor/mock/mock_async_executor.h"
#include "core/async_executor/src/async_executor.h"
#include "core/authorization_proxy/mock/mock_authorization_proxy.h"
#include "core/common/concurrent_map/src/error_codes.h"
#include "core/common/uuid/src/uuid.h"
#include "core/http2_client/src/http2_client.h"
#include "core/http2_server/mock/mock_http2_server_with_overrides.h"
#include "core/http2_server/src/error_codes.h"
#include "core/test/utils/conditional_wait.h"
#include "cpio/client_providers/metric_client_provider/mock/mock_metric_client_provider.h"

using google::scp::core::AsyncExecutor;
using google::scp::core::AuthorizationProxyInterface;
using google::scp::core::Http2Server;
using google::scp::core::HttpClient;
using google::scp::core::async_executor::mock::MockAsyncExecutor;
using google::scp::core::authorization_proxy::mock::MockAuthorizationProxy;
using google::scp::core::common::Uuid;
using google::scp::core::http2_server::mock::MockHttp2ServerWithOverrides;
using google::scp::core::test::WaitUntil;
using google::scp::cpio::client_providers::mock::MockMetricClientProvider;
using std::make_shared;
using std::promise;
using std::shared_ptr;
using std::string;
using std::to_string;
using std::chrono::milliseconds;
using std::chrono::seconds;
using testing::Return;

namespace google::scp::core::test {

class Http2ServerTest : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    // Generate a self-signed cert
    system("openssl genrsa 2048 > privatekey.pem");
    system(
        "openssl req -new -key privatekey.pem -out csr.pem -config "
        "cc/core/http2_server/test/certs/csr.conf");
    system(
        "openssl x509 -req -days 7305 -in csr.pem -signkey privatekey.pem -out "
        "public.crt");
  }
};

TEST_F(Http2ServerTest, Run) {
  string host_address("localhost");
  string port("0");

  shared_ptr<AuthorizationProxyInterface> mock_authorization_proxy =
      make_shared<MockAuthorizationProxy>();
  auto mock_metric_client = make_shared<MockMetricClientProvider>();
  shared_ptr<AsyncExecutorInterface> async_executor =
      make_shared<MockAsyncExecutor>();
  Http2Server http_server(host_address, port, 2 /* thread_pool_size */,
                          async_executor, mock_authorization_proxy);

  EXPECT_EQ(http_server.Run(), SuccessExecutionResult());
  EXPECT_EQ(http_server.Run(),
            FailureExecutionResult(errors::SC_HTTP2_SERVER_ALREADY_RUNNING));

  EXPECT_EQ(http_server.Stop(), SuccessExecutionResult());
  EXPECT_EQ(http_server.Stop(),
            FailureExecutionResult(errors::SC_HTTP2_SERVER_ALREADY_STOPPED));
}

TEST_F(Http2ServerTest, RegisterHandlers) {
  string host_address("localhost");
  string port("0");

  shared_ptr<AuthorizationProxyInterface> mock_authorization_proxy =
      make_shared<MockAuthorizationProxy>();
  auto mock_metric_client = make_shared<MockMetricClientProvider>();
  shared_ptr<AsyncExecutorInterface> async_executor =
      make_shared<MockAsyncExecutor>();
  MockHttp2ServerWithOverrides http_server(host_address, port, async_executor,
                                           mock_authorization_proxy,
                                           mock_metric_client);

  string path("/test/path");
  HttpHandler callback = [](AsyncContext<HttpRequest, HttpResponse>&) {
    return SuccessExecutionResult();
  };

  EXPECT_EQ(
      http_server.RegisterResourceHandler(HttpMethod::GET, path, callback),
      SuccessExecutionResult());

  EXPECT_EQ(
      http_server.RegisterResourceHandler(HttpMethod::GET, path, callback),
      FailureExecutionResult(errors::SC_CONCURRENT_MAP_ENTRY_ALREADY_EXISTS));
}

TEST_F(Http2ServerTest, HandleHttp2Request) {
  string host_address("localhost");
  string port("0");

  auto mock_authorization_proxy = make_shared<MockAuthorizationProxy>();
  shared_ptr<AuthorizationProxyInterface> authorization_proxy =
      mock_authorization_proxy;
  ON_CALL(*mock_authorization_proxy, Authorize)
      .WillByDefault(Return(SuccessExecutionResult()));

  auto mock_metric_client = make_shared<MockMetricClientProvider>();
  shared_ptr<AsyncExecutorInterface> async_executor =
      make_shared<MockAsyncExecutor>();
  MockHttp2ServerWithOverrides http_server(host_address, port, async_executor,
                                           authorization_proxy,
                                           mock_metric_client);

  HttpHandler callback = [](AsyncContext<HttpRequest, HttpResponse>&) {
    return SuccessExecutionResult();
  };

  AsyncContext<NgHttp2Request, NgHttp2Response> ng_http2_context(
      make_shared<NgHttp2Request>(nghttp2::asio_http2::server::request()),
      [](AsyncContext<NgHttp2Request, NgHttp2Response>&) {});

  http_server.HandleHttp2Request(ng_http2_context, callback);
  shared_ptr<MockHttp2ServerWithOverrides::Http2SynchronizationContext>
      sync_context;
  EXPECT_EQ(http_server.GetActiveRequests().Find(ng_http2_context.request->id,
                                                 sync_context),
            SuccessExecutionResult());
  EXPECT_EQ(sync_context->failed.load(), false);
  EXPECT_EQ(sync_context->pending_callbacks.load(), 2);
  bool function_exist = false;
  if (ng_http2_context.request->on_request_body_received) {
    function_exist = true;
  }
  EXPECT_EQ(function_exist, true);
}

TEST_F(Http2ServerTest, HandleHttp2RequestFailed) {
  string host_address("localhost");
  string port("0");

  auto mock_authorization_proxy = make_shared<MockAuthorizationProxy>();
  shared_ptr<AuthorizationProxyInterface> authorization_proxy =
      mock_authorization_proxy;
  ON_CALL(*mock_authorization_proxy, Authorize)
      .WillByDefault(Return(FailureExecutionResult(123)));
  auto mock_metric_client = make_shared<MockMetricClientProvider>();
  shared_ptr<AsyncExecutorInterface> async_executor =
      make_shared<MockAsyncExecutor>();
  MockHttp2ServerWithOverrides http_server(host_address, port, async_executor,
                                           authorization_proxy,
                                           mock_metric_client);

  HttpHandler callback = [](AsyncContext<HttpRequest, HttpResponse>&) {
    return SuccessExecutionResult();
  };

  bool should_continue = false;
  AsyncContext<NgHttp2Request, NgHttp2Response> ng_http2_context(
      make_shared<NgHttp2Request>(nghttp2::asio_http2::server::request()),
      [&](AsyncContext<NgHttp2Request, NgHttp2Response>&) {
        should_continue = true;
      });

  http_server.HandleHttp2Request(ng_http2_context, callback);
  http_server.OnHttp2Cleanup(ng_http2_context.parent_activity_id,
                             ng_http2_context.request->id, 0);

  shared_ptr<MockHttp2ServerWithOverrides::Http2SynchronizationContext>
      sync_context;
  EXPECT_EQ(
      http_server.GetActiveRequests().Find(ng_http2_context.request->id,
                                           sync_context),
      FailureExecutionResult(errors::SC_CONCURRENT_MAP_ENTRY_DOES_NOT_EXIST));

  WaitUntil([&]() { return should_continue; });
}

TEST_F(Http2ServerTest, OnHttp2PendingCallbackFailure) {
  string host_address("localhost");
  string port("0");
  nghttp2::asio_http2::server::request ng_request;

  auto mock_authorization_proxy = make_shared<MockAuthorizationProxy>();
  shared_ptr<AuthorizationProxyInterface> authorization_proxy =
      mock_authorization_proxy;
  auto mock_metric_client = make_shared<MockMetricClientProvider>();
  shared_ptr<AsyncExecutorInterface> async_executor =
      make_shared<MockAsyncExecutor>();
  MockHttp2ServerWithOverrides http_server(host_address, port, async_executor,
                                           authorization_proxy,
                                           mock_metric_client);

  HttpHandler callback = [](AsyncContext<HttpRequest, HttpResponse>&) {
    return SuccessExecutionResult();
  };

  bool should_continue = false;
  AsyncContext<NgHttp2Request, NgHttp2Response> ng_http2_context(
      make_shared<NgHttp2Request>(ng_request),
      [&](AsyncContext<NgHttp2Request, NgHttp2Response>&) {
        should_continue = true;
      });

  auto sync_context =
      make_shared<MockHttp2ServerWithOverrides::Http2SynchronizationContext>();
  sync_context->failed = false;
  sync_context->pending_callbacks = 2;
  sync_context->http2_context = ng_http2_context;
  sync_context->http_handler = callback;

  auto pair = make_pair(ng_http2_context.request->id, sync_context);
  EXPECT_EQ(http_server.GetActiveRequests().Insert(pair, sync_context),
            SuccessExecutionResult());

  auto callback_execution_result = FailureExecutionResult(1234);
  auto request_id = ng_http2_context.request->id;
  http_server.OnHttp2PendingCallback(callback_execution_result, request_id);
  WaitUntil([&]() { return should_continue; });

  EXPECT_EQ(http_server.GetActiveRequests().Find(request_id, sync_context),
            SuccessExecutionResult());
  EXPECT_EQ(sync_context->failed.load(), true);

  http_server.OnHttp2PendingCallback(callback_execution_result, request_id);
  http_server.OnHttp2Cleanup(sync_context->http2_context.parent_activity_id,
                             request_id, 0);
  EXPECT_EQ(
      http_server.GetActiveRequests().Find(request_id, sync_context),
      FailureExecutionResult(errors::SC_CONCURRENT_MAP_ENTRY_DOES_NOT_EXIST));
}

TEST_F(Http2ServerTest, OnHttp2PendingCallbackHttpHandlerFailure) {
  string host_address("localhost");
  string port("0");

  auto mock_authorization_proxy = make_shared<MockAuthorizationProxy>();
  shared_ptr<AuthorizationProxyInterface> authorization_proxy =
      mock_authorization_proxy;
  shared_ptr<AsyncExecutorInterface> async_executor =
      make_shared<MockAsyncExecutor>();
  auto mock_metric_client = make_shared<MockMetricClientProvider>();
  MockHttp2ServerWithOverrides http_server(host_address, port, async_executor,
                                           authorization_proxy,
                                           mock_metric_client);

  HttpHandler callback = [](AsyncContext<HttpRequest, HttpResponse>&) {
    return FailureExecutionResult(12345);
  };

  bool should_continue = false;
  AsyncContext<NgHttp2Request, NgHttp2Response> ng_http2_context(
      make_shared<NgHttp2Request>(nghttp2::asio_http2::server::request()),
      [&](AsyncContext<NgHttp2Request, NgHttp2Response>& http2_context) {
        EXPECT_EQ(http2_context.result, FailureExecutionResult(12345));
        should_continue = true;
      });

  auto sync_context =
      make_shared<MockHttp2ServerWithOverrides::Http2SynchronizationContext>();
  sync_context->failed = false;
  sync_context->pending_callbacks = 1;
  sync_context->http2_context = ng_http2_context;
  sync_context->http_handler = callback;

  auto pair = make_pair(ng_http2_context.request->id, sync_context);
  EXPECT_EQ(http_server.GetActiveRequests().Insert(pair, sync_context),
            SuccessExecutionResult());

  auto callback_execution_result = SuccessExecutionResult();
  auto request_id = ng_http2_context.request->id;
  http_server.OnHttp2PendingCallback(callback_execution_result, request_id);
  WaitUntil([&]() { return should_continue; });
}

TEST_F(Http2ServerTest,
       ShouldFailToInitWhenTlsContextPrivateKeyFileDoesNotExist) {
  string host_address("localhost");
  string port("0");

  shared_ptr<AuthorizationProxyInterface> mock_authorization_proxy =
      make_shared<MockAuthorizationProxy>();
  shared_ptr<AsyncExecutorInterface> async_executor =
      make_shared<MockAsyncExecutor>();
  size_t thread_pool_size = 2;
  bool use_tls = true;
  string private_key_file("/file/that/dos/not/exist.pem");
  string certificate_chain_file("./public.crt");

  Http2Server http_server(host_address, port, thread_pool_size, async_executor,
                          mock_authorization_proxy, nullptr /* metric_client */,
                          use_tls, private_key_file, certificate_chain_file);

  EXPECT_EQ(http_server.Init(),
            FailureExecutionResult(
                errors::SC_HTTP2_SERVER_FAILED_TO_INITIALIZE_TLS_CONTEXT));
}

TEST_F(Http2ServerTest,
       ShouldFailToInitWhenTlsContextCertificateChainFileDoesNotExist) {
  string host_address("localhost");
  string port("0");

  shared_ptr<AuthorizationProxyInterface> mock_authorization_proxy =
      make_shared<MockAuthorizationProxy>();
  shared_ptr<AsyncExecutorInterface> async_executor =
      make_shared<MockAsyncExecutor>();
  size_t thread_pool_size = 2;
  bool use_tls = true;
  string private_key_file("./privatekey.pem");
  string certificate_chain_file("/file/that/dos/not/exist.crt");

  Http2Server http_server(host_address, port, thread_pool_size, async_executor,
                          mock_authorization_proxy, nullptr /* metric_client */,
                          use_tls, private_key_file, certificate_chain_file);

  EXPECT_EQ(http_server.Init(),
            FailureExecutionResult(
                errors::SC_HTTP2_SERVER_FAILED_TO_INITIALIZE_TLS_CONTEXT));
}

TEST_F(Http2ServerTest,
       ShouldInitCorrectlyWhenPrivateKeyAndCertChainFilesExist) {
  string host_address("localhost");
  string port("0");

  shared_ptr<AuthorizationProxyInterface> mock_authorization_proxy =
      make_shared<MockAuthorizationProxy>();
  shared_ptr<AsyncExecutorInterface> async_executor =
      make_shared<MockAsyncExecutor>();
  size_t thread_pool_size = 2;
  bool use_tls = true;
  string private_key_file("./privatekey.pem");
  string certificate_chain_file("./public.crt");

  Http2Server http_server(host_address, port, thread_pool_size, async_executor,
                          mock_authorization_proxy, nullptr /* metric_client */,
                          use_tls, private_key_file, certificate_chain_file);

  EXPECT_EQ(http_server.Init(), SuccessExecutionResult());
}

TEST_F(Http2ServerTest, ShouldInitCorrectlyRunAndStopWhenTlsIsEnabled) {
  string host_address("localhost");
  string port("0");

  shared_ptr<AuthorizationProxyInterface> mock_authorization_proxy =
      make_shared<MockAuthorizationProxy>();
  shared_ptr<AsyncExecutorInterface> async_executor =
      make_shared<MockAsyncExecutor>();
  size_t thread_pool_size = 2;
  bool use_tls = true;
  string private_key_file("./privatekey.pem");
  string certificate_chain_file("./public.crt");

  Http2Server http_server(host_address, port, thread_pool_size, async_executor,
                          mock_authorization_proxy, nullptr /* metric_client */,
                          use_tls, private_key_file, certificate_chain_file);

  EXPECT_EQ(http_server.Init(), SuccessExecutionResult());
  EXPECT_EQ(http_server.Run(), SuccessExecutionResult());
  EXPECT_EQ(http_server.Stop(), SuccessExecutionResult());
}

static int GenerateRandomIntInRange(int min, int max) {
  std::random_device randomdevice;
  std::mt19937 random_number_engine(randomdevice());
  std::uniform_int_distribution<int> uniform_distribution(min, max);

  return uniform_distribution(random_number_engine);
}

void SubmitUntilSuccess(HttpClient& http_client,
                        AsyncContext<HttpRequest, HttpResponse>& context) {
  ExecutionResult execution_result = RetryExecutionResult(123);
  while (execution_result.status == ExecutionStatus::Retry) {
    execution_result = http_client.PerformRequest(context);
    std::this_thread::sleep_for(milliseconds(50));
  }
  EXPECT_EQ(execution_result, SuccessExecutionResult());
}

TEST_F(Http2ServerTest, ShouldHandleRequestProperlyWhenTlsIsEnabled) {
  string host_address("localhost");
  int random_port = GenerateRandomIntInRange(8000, 60000);
  string port = to_string(random_port);
  shared_ptr<MockAuthorizationProxy> mock_authorization_proxy =
      make_shared<MockAuthorizationProxy>();
  ON_CALL(*mock_authorization_proxy, Authorize)
      .WillByDefault([](auto& context) {
        context.response = make_shared<AuthorizationProxyResponse>();
        context.response->authorized_metadata.authorized_domain =
            make_shared<string>(
                context.request->authorization_metadata.claimed_identity);
        context.result = SuccessExecutionResult();

        context.Finish();
        return SuccessExecutionResult();
      });
  shared_ptr<AuthorizationProxyInterface> authorization_proxy =
      mock_authorization_proxy;
  shared_ptr<AsyncExecutorInterface> async_executor =
      make_shared<AsyncExecutor>(8, 10, true);

  size_t thread_pool_size = 2;
  bool use_tls = true;
  string private_key_file("./privatekey.pem");
  string certificate_chain_file("./public.crt");
  string test_path("/test");

  // Start the server
  Http2Server http_server(host_address, port, thread_pool_size, async_executor,
                          authorization_proxy, nullptr /* metric_client */,
                          use_tls, private_key_file, certificate_chain_file);

  HttpHandler handler_callback =
      [](AsyncContext<HttpRequest, HttpResponse>& context) {
        context.result = SuccessExecutionResult();
        context.response->body = BytesBuffer("hello, world with TLS\r\n");
        context.Finish();
        return SuccessExecutionResult();
      };
  http_server.RegisterResourceHandler(HttpMethod::GET, test_path,
                                      handler_callback);

  EXPECT_EQ(http_server.Init(), SuccessExecutionResult());
  EXPECT_EQ(http_server.Run(), SuccessExecutionResult());

  // Start the client
  HttpClient http_client(async_executor);
  http_client.Init();
  http_client.Run();
  async_executor->Init();
  async_executor->Run();

  // Send request to server
  auto request = make_shared<HttpRequest>();
  request->method = HttpMethod::GET;
  request->path = make_shared<string>("https://localhost:" + port + test_path);
  promise<void> done;
  AsyncContext<HttpRequest, HttpResponse> context(
      move(request), [&](AsyncContext<HttpRequest, HttpResponse>& context) {
        EXPECT_EQ(context.result, SuccessExecutionResult());
        const auto& bytes = *context.response->body.bytes;
        EXPECT_EQ(string(bytes.begin(), bytes.end()),
                  "hello, world with TLS\r\n");
        done.set_value();
      });
  SubmitUntilSuccess(http_client, context);

  // Wait for request to be done.
  done.get_future().get();
  http_client.Stop();
  http_server.Stop();
  async_executor->Stop();
}
}  // namespace google::scp::core::test