#include "steam/backend/native_http_ps4.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>

extern "C" {
int sceSysmoduleLoadModuleInternal(std::uint32_t id);
int sceSysmoduleGetModuleHandleInternal(std::uint32_t id, std::int32_t* handle);
int sceKernelDlsym(std::int32_t handle, const char* symbol, void** address);
int sceNetPoolCreate(const char* name, int size, int flags);
}

namespace playden::steam::internal {
namespace {

constexpr std::uint32_t kHttpModule = 0x8000000a;
constexpr std::uint32_t kSslModule = 0x8000000b;
constexpr int kHttp11 = 2;
constexpr int kGet = 0;
constexpr int kPost = 1;
constexpr int kPut = 4;
constexpr int kHeaderOverwrite = 0;
constexpr std::size_t kReadChunk = 4096;

struct HttpApi {
  int (*ssl_init)(std::size_t) = nullptr;
  int (*http_init)(int, int, std::size_t) = nullptr;
  int (*create_template)(int, const char*, int, int) = nullptr;
  int (*delete_template)(int) = nullptr;
  int (*resolve_timeout)(int, std::uint32_t) = nullptr;
  int (*connect_timeout)(int, std::uint32_t) = nullptr;
  int (*send_timeout)(int, std::uint32_t) = nullptr;
  void (*recv_timeout)(int, std::uint32_t) = nullptr;
  int (*create_connection)(int, const char*, int) = nullptr;
  int (*delete_connection)(int) = nullptr;
  int (*create_request)(int, int, const char*, std::uint64_t) = nullptr;
  int (*delete_request)(int) = nullptr;
  int (*add_header)(int, const char*, const char*, int) = nullptr;
  int (*send)(int, const void*, std::size_t) = nullptr;
  int (*status)(int, int*) = nullptr;
  int (*get_headers)(int, char**, std::size_t*) = nullptr;
  int (*read)(int, void*, std::uint32_t) = nullptr;
  // Optional: absent on some firmware. Without it libSceHttp refuses any response whose
  // headers exceed its small default (0x80431073, seen on GitHub's 5.6 KB redirects).
  int (*header_max)(int, std::size_t) = nullptr;
};

HttpApi api;
std::once_flag init_once;
bool initialized = false;
int net_pool = -1;
int ssl_context = -1;
int http_context = -1;

bool BindModule(std::uint32_t module_id, int* out_handle) {
  int handle = -1;
  const int load = sceSysmoduleLoadModuleInternal(module_id);
  if (sceSysmoduleGetModuleHandleInternal(module_id, &handle) != 0 || handle < 0) {
    (void)load;
    return false;
  }
  *out_handle = handle;
  return true;
}

template <typename T>
bool Bind(int module_handle, const char* name, T* slot) {
  void* address = nullptr;
  if (sceKernelDlsym(module_handle, name, &address) != 0 || !address) return false;
  static_assert(sizeof(address) == sizeof(*slot));
  std::memcpy(slot, &address, sizeof(address));
  return true;
}

bool BindApi() {
  int ssl_handle = -1;
  int http_handle = -1;
  if (!BindModule(kSslModule, &ssl_handle) || !BindModule(kHttpModule, &http_handle))
    return false;
  return Bind(ssl_handle, "sceSslInit", &api.ssl_init) &&
         Bind(http_handle, "sceHttpInit", &api.http_init) &&
         Bind(http_handle, "sceHttpCreateTemplate", &api.create_template) &&
         Bind(http_handle, "sceHttpDeleteTemplate", &api.delete_template) &&
         Bind(http_handle, "sceHttpSetResolveTimeOut", &api.resolve_timeout) &&
         Bind(http_handle, "sceHttpSetConnectTimeOut", &api.connect_timeout) &&
         Bind(http_handle, "sceHttpSetSendTimeOut", &api.send_timeout) &&
         Bind(http_handle, "sceHttpSetRecvTimeOut", &api.recv_timeout) &&
         Bind(http_handle, "sceHttpCreateConnectionWithURL", &api.create_connection) &&
         Bind(http_handle, "sceHttpDeleteConnection", &api.delete_connection) &&
         Bind(http_handle, "sceHttpCreateRequestWithURL", &api.create_request) &&
         Bind(http_handle, "sceHttpDeleteRequest", &api.delete_request) &&
         Bind(http_handle, "sceHttpAddRequestHeader", &api.add_header) &&
         Bind(http_handle, "sceHttpSendRequest", &api.send) &&
         Bind(http_handle, "sceHttpGetStatusCode", &api.status) &&
         Bind(http_handle, "sceHttpGetAllResponseHeaders", &api.get_headers) &&
         Bind(http_handle, "sceHttpReadData", &api.read) &&
         (Bind(http_handle, "sceHttpSetResponseHeaderMaxSize", &api.header_max), true);
}

void InitializeOnce() {
  if (!BindApi()) return;
  net_pool = sceNetPoolCreate("playden-http", 16 * 1024, 0);
  if (net_pool < 0) return;
  ssl_context = api.ssl_init(256 * 1024);
  if (ssl_context < 0) return;
  http_context = api.http_init(net_pool, ssl_context, 64 * 1024);
  initialized = http_context >= 0;
}

void Trace(const std::function<void(const std::string&)>& trace,
           const std::string& line) {
  if (!trace) return;
  try {
    trace("scehttp " + line);
  } catch (...) {
  }
}

std::string HostFromUrl(const std::string& url) {
  const auto scheme = url.find("://");
  if (scheme == std::string::npos) return "unknown";
  const auto start = scheme + 3;
  const auto end = url.find_first_of("/:?#", start);
  return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool ParseHeaders(char* raw, std::size_t size,
                  std::vector<std::pair<std::string, std::string>>* headers) {
  if (!raw || size > kMaxHttpResponseHeaderBytes) return false;
  std::string_view bytes(raw, size);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto eol = bytes.find("\r\n", offset);
    if (eol == std::string_view::npos) break;
    auto line = bytes.substr(offset, eol - offset);
    offset = eol + 2;
    if (line.empty() || line.rfind("HTTP/", 0) == 0) continue;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    auto name = line.substr(0, colon);
    auto value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
      value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
      value.remove_suffix(1);
    if (headers->size() >= kMaxHttpResponseHeaderCount) return false;
    headers->emplace_back(name, value);
  }
  return true;
}

}  // namespace

bool InitializeNativeHttpPs4() {
  std::call_once(init_once, InitializeOnce);
  return initialized;
}

HttpResponse PerformNativeHttpPs4(
    const HttpRequest& request, const std::atomic<bool>& cancel,
    const std::function<void(const std::string&)>& trace) {
  using Clock = std::chrono::steady_clock;
  HttpResponse response;
  // HttpClient validates this too, but keep the native boundary safe for
  // callers that invoke it directly or follow a URL outside HttpClient.
  if (request.url.rfind("https://", 0) != 0 &&
      !(request.allow_plain_http && request.url.rfind("http://", 0) == 0)) {
    response.error = HttpError::kInvalidRequest;
    return response;
  }
  if (!InitializeNativeHttpPs4()) {
    response.error = HttpError::kInitializationFailed;
    return response;
  }
  if (cancel.load(std::memory_order_acquire)) {
    response.error = HttpError::kCanceled;
    return response;
  }

  const auto start = Clock::now();
  const auto timeout_us = static_cast<std::uint32_t>(request.timeout_ms * 1000u);
  const char* method = request.method == HttpMethod::kGet ? "GET" :
                       request.method == HttpMethod::kPut ? "PUT" : "POST";
  Trace(trace, std::string(method) + " start host=" + HostFromUrl(request.url) +
                   " timeout=" + std::to_string(request.timeout_ms) + "ms max_response=" +
                   std::to_string(request.max_response_bytes));

  int template_id = api.create_template(http_context, "Playden/1.0", kHttp11, 1);
  int connection_id = -1;
  int request_id = -1;
  int native_error = 0;
  bool response_complete = false;
  if (template_id < 0) native_error = template_id;
  if (!native_error) {
    if (api.resolve_timeout(template_id, timeout_us) < 0 ||
        api.connect_timeout(template_id, timeout_us) < 0 ||
        api.send_timeout(template_id, timeout_us) < 0)
      native_error = -1;
    else
      api.recv_timeout(template_id, timeout_us);
  }
  // Accept the same header size that ParseHeaders accepts.
  if (!native_error && api.header_max) {
    const int result = api.header_max(template_id, kMaxHttpResponseHeaderBytes);
    if (result < 0)
      Trace(trace, "response header limit " + std::to_string(kMaxHttpResponseHeaderBytes) +
                       " refused rc=" + std::to_string(result));
  }
  if (!native_error && !cancel.load(std::memory_order_acquire)) {
    connection_id = api.create_connection(template_id, request.url.c_str(), 0);
    if (connection_id < 0) native_error = connection_id;
  }
  const int method_id = request.method == HttpMethod::kGet ? kGet :
                        request.method == HttpMethod::kPut ? kPut : kPost;
  if (!native_error && !cancel.load(std::memory_order_acquire)) {
    request_id = api.create_request(connection_id, method_id, request.url.c_str(),
                                    request.body.size());
    if (request_id < 0) native_error = request_id;
  }
  for (const auto& header : request.headers) {
    if (native_error || cancel.load(std::memory_order_acquire)) break;
    const int result = api.add_header(request_id, header.first.c_str(),
                                      header.second.c_str(), kHeaderOverwrite);
    if (result < 0) native_error = result;
  }
  if (!native_error && !cancel.load(std::memory_order_acquire)) {
    native_error = api.send(request_id,
                            request.body.empty() ? nullptr : request.body.data(),
                            request.body.size());
    if (native_error >= 0) {
      const auto first = std::chrono::duration_cast<std::chrono::milliseconds>(
          Clock::now() - start).count();
      response.timing.first_byte_ms = static_cast<std::uint32_t>(first);
      int status = 0;
      const int status_result = api.status(request_id, &status);
      if (status_result < 0) native_error = status_result;
      else response.status_code = status;
    }
  }
  if (!native_error && request_id >= 0) {
    char* raw_headers = nullptr;
    std::size_t header_size = 0;
    const int result = api.get_headers(request_id, &raw_headers, &header_size);
    if (result < 0 || !ParseHeaders(raw_headers, header_size, &response.headers))
      native_error = result < 0 ? result : -1;
  }
  if (!native_error && request_id >= 0) {
    if (request.preallocate_response && request.max_response_bytes <= 2 * 1024 * 1024)
      response.body.reserve(request.max_response_bytes);
    std::array<std::uint8_t, kReadChunk> chunk{};
    while (!cancel.load(std::memory_order_acquire)) {
      const int n = api.read(request_id, chunk.data(), chunk.size());
      if (n < 0) {
        native_error = n;
        break;
      }
      if (n == 0) {
        response_complete = true;
        break;
      }
      const auto bytes = static_cast<std::size_t>(n);
      if (bytes > request.max_response_bytes - response.body.size()) {
        response.error = HttpError::kResponseTooLarge;
        break;
      }
      response.body.insert(response.body.end(), chunk.begin(), chunk.begin() + n);
    }
  }
  if (request_id >= 0) api.delete_request(request_id);
  if (connection_id >= 0) api.delete_connection(connection_id);
  if (template_id >= 0) api.delete_template(template_id);

  response.timing.total_ms = static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
  response.timing.body_bytes = response.body.size();
  if (cancel.load(std::memory_order_acquire)) response.error = HttpError::kCanceled;
  else if (response.error != HttpError::kResponseTooLarge) {
    response.transport_code = native_error;
    if (native_error < 0 || !response_complete) response.error = HttpError::kProtocolError;
    else if (response.status_code == 0) response.error = HttpError::kProtocolError;
    else response.error = HttpError::kNone;
  }
  if (response.error != HttpError::kNone) response.status_code = 0;
  Trace(trace, std::string(method) + " complete native=" +
                   std::to_string(native_error) + " status=" +
                   std::to_string(response.status_code) + " request_body=" +
                   std::to_string(request.body.size()) + " response_headers=" +
                   std::to_string(response.headers.size()) + " response_body=" +
                   std::to_string(response.body.size()) + " total=" +
                   std::to_string(response.timing.total_ms) + "ms");
  return response;
}

}  // namespace playden::steam::internal
