#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <format>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using HttpsRequest = std::pair<std::wstring, std::wstring>;

enum class AssetKind { crypto, stock_perpetual };

struct Asset {
  std::string_view symbol;
  AssetKind kind;
};

constexpr std::array assets{
    Asset{"BTC", AssetKind::crypto},           Asset{"ETH", AssetKind::crypto},
    Asset{"SOL", AssetKind::crypto},           Asset{"BNB", AssetKind::crypto},
    Asset{"HYPE", AssetKind::crypto},          Asset{"ZEC", AssetKind::crypto},
    Asset{"XAUT", AssetKind::crypto},          Asset{"TSLA", AssetKind::stock_perpetual},
    Asset{"NVDA", AssetKind::stock_perpetual}, Asset{"DRAM", AssetKind::stock_perpetual},
    Asset{"SPCX", AssetKind::stock_perpetual}, Asset{"CRCL", AssetKind::stock_perpetual},
};
std::atomic_bool running{true};

struct Quote {
  double price{};
  double change24h{};
  std::chrono::steady_clock::time_point updated{};
};

class MarketStore {
public:
  void update(std::string symbol, std::string exchange, double price, double change) {
    std::scoped_lock lock{mutex_};
    quotes_[std::move(symbol)][std::move(exchange)] = {price, change,
                                                       std::chrono::steady_clock::now()};
  }
  [[nodiscard]] auto snapshot() const {
    std::scoped_lock lock{mutex_};
    return quotes_;
  }

private:
  mutable std::mutex mutex_;
  std::map<std::string, std::map<std::string, Quote>> quotes_;
};

#ifdef _WIN32
struct InternetHandle {
  HINTERNET value{};
  ~InternetHandle() {
    if (value)
      WinHttpCloseHandle(value);
  }
  InternetHandle() = default;
  explicit InternetHandle(HINTERNET handle) : value{handle} {}
  InternetHandle(const InternetHandle&) = delete;
  InternetHandle& operator=(const InternetHandle&) = delete;
};

std::optional<std::string> get_https(std::wstring_view host, std::wstring_view path) {
  InternetHandle session{WinHttpOpen(L"CryptoMarketTerminal/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.value)
    return std::nullopt;
  WinHttpSetTimeouts(session.value, 3'000, 3'000, 3'000, 5'000);
  InternetHandle connection{
      WinHttpConnect(session.value, host.data(), INTERNET_DEFAULT_HTTPS_PORT, 0)};
  if (!connection.value)
    return std::nullopt;
  InternetHandle request{WinHttpOpenRequest(connection.value, L"GET", path.data(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE)};
  if (!request.value ||
      !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                          0, 0, 0) ||
      !WinHttpReceiveResponse(request.value, nullptr))
    return std::nullopt;
  DWORD status{}, status_size = sizeof(status);
  WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
  if (status != 200)
    return std::nullopt;
  std::string body;
  for (;;) {
    DWORD available{};
    if (!WinHttpQueryDataAvailable(request.value, &available) || available == 0)
      break;
    const auto offset = body.size();
    body.resize(offset + available);
    DWORD read{};
    if (!WinHttpReadData(request.value, body.data() + offset, available, &read))
      return std::nullopt;
    body.resize(offset + read);
  }
  return body;
}

std::vector<std::optional<std::string>> get_https_batch(
    const std::vector<HttpsRequest>& requests) {
  std::vector<std::optional<std::string>> responses;
  responses.reserve(requests.size());
  for (const auto& [host, path] : requests)
    responses.push_back(get_https(host, path));
  return responses;
}
#else
size_t append_response(char* data, size_t size, size_t count, void* output) {
  const auto bytes = size * count;
  static_cast<std::string*>(output)->append(data, bytes);
  return bytes;
}

struct CurlMultiHandle {
  CURLM* value{curl_multi_init()};
  CurlMultiHandle() = default;
  ~CurlMultiHandle() {
    if (value)
      curl_multi_cleanup(value);
  }
  CurlMultiHandle(const CurlMultiHandle&) = delete;
  CurlMultiHandle& operator=(const CurlMultiHandle&) = delete;
};

struct CurlTransfer {
  size_t index{};
  CURL* handle{};
  std::string url;
  std::string body;
  CURLcode result{CURLE_FAILED_INIT};
};

std::vector<std::optional<std::string>> get_https_batch(
    const std::vector<HttpsRequest>& requests) {
  std::vector<std::optional<std::string>> responses(requests.size());
  thread_local CurlMultiHandle multi;
  if (!multi.value)
    return responses;

  std::vector<CurlTransfer> transfers;
  transfers.reserve(requests.size());
  for (size_t index = 0; index < requests.size(); ++index) {
    const auto& [host, path] = requests[index];
    transfers.push_back({index, curl_easy_init(),
                         "https://" + std::string{host.begin(), host.end()} +
                             std::string{path.begin(), path.end()},
                         {}, CURLE_FAILED_INIT});
    auto& transfer = transfers.back();
    if (!transfer.handle)
      continue;
    curl_easy_setopt(transfer.handle, CURLOPT_URL, transfer.url.c_str());
    curl_easy_setopt(transfer.handle, CURLOPT_USERAGENT, "CryptoMarketTerminal/1.0");
    curl_easy_setopt(transfer.handle, CURLOPT_CONNECTTIMEOUT_MS, 3'000L);
    curl_easy_setopt(transfer.handle, CURLOPT_TIMEOUT_MS, 5'000L);
    curl_easy_setopt(transfer.handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(transfer.handle, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(transfer.handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(transfer.handle, CURLOPT_WRITEFUNCTION, append_response);
    curl_easy_setopt(transfer.handle, CURLOPT_WRITEDATA, &transfer.body);
    curl_easy_setopt(transfer.handle, CURLOPT_PRIVATE, &transfer);
    if (curl_multi_add_handle(multi.value, transfer.handle) != CURLM_OK) {
      curl_easy_cleanup(transfer.handle);
      transfer.handle = nullptr;
    }
  }

  int active{};
  auto multi_result = curl_multi_perform(multi.value, &active);
  while (multi_result == CURLM_OK && active > 0) {
    int descriptors{};
    multi_result = curl_multi_poll(multi.value, nullptr, 0, 1'000, &descriptors);
    if (multi_result == CURLM_OK)
      multi_result = curl_multi_perform(multi.value, &active);
  }

  int remaining{};
  while (auto* message = curl_multi_info_read(multi.value, &remaining)) {
    if (message->msg != CURLMSG_DONE)
      continue;
    CurlTransfer* transfer{};
    curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &transfer);
    if (transfer)
      transfer->result = message->data.result;
  }

  for (auto& transfer : transfers) {
    if (!transfer.handle)
      continue;
    long status{};
    curl_easy_getinfo(transfer.handle, CURLINFO_RESPONSE_CODE, &status);
    if (transfer.result == CURLE_OK && status == 200)
      responses[transfer.index] = std::move(transfer.body);
    curl_multi_remove_handle(multi.value, transfer.handle);
    curl_easy_cleanup(transfer.handle);
  }
  return responses;
}
#endif

std::optional<std::string_view> json_string(std::string_view object, std::string_view key) {
  const auto marker = std::format("\"{}\":\"", key);
  const auto start = object.find(marker);
  if (start == std::string_view::npos)
    return std::nullopt;
  const auto value_start = start + marker.size();
  const auto end = object.find('"', value_start);
  if (end == std::string_view::npos)
    return std::nullopt;
  return object.substr(value_start, end - value_start);
}

std::optional<double> as_double(std::optional<std::string_view> value) {
  if (!value)
    return std::nullopt;
  double result{};
  const auto [ptr, error] = std::from_chars(value->data(), value->data() + value->size(), result);
  if (error != std::errc{} || ptr != value->data() + value->size())
    return std::nullopt;
  return result;
}

class ExchangePoller {
public:
  explicit ExchangePoller(MarketStore& store) : store_{store} {}
  virtual ~ExchangePoller() { stop(); }
  void start() {
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
  }
  void stop() {
    if (worker_.joinable()) {
      worker_.request_stop();
      worker_.join();
    }
  }
protected:
  virtual void poll() = 0;
  MarketStore& store_;

private:
  void run(std::stop_token stop) {
    while (!stop.stop_requested() && running.load()) {
      poll();
      for (int i = 0; i < 10 && !stop.stop_requested() && running.load(); ++i)
        std::this_thread::sleep_for(100ms);
    }
  }
  std::jthread worker_;
};

class BinancePoller final : public ExchangePoller {
public:
  explicit BinancePoller(MarketStore& store) : ExchangePoller{store} {}

private:
  void poll() override {
    std::vector<HttpsRequest> requests;
    requests.reserve(assets.size());
    for (const auto& asset : assets) {
      const std::wstring symbol{asset.symbol.begin(), asset.symbol.end()};
      const auto path = asset.kind == AssetKind::crypto
                            ? std::format(L"/api/v3/ticker/24hr?symbol={}USDT", symbol)
                            : std::format(L"/fapi/v1/ticker/24hr?symbol={}USDT", symbol);
      const auto host = asset.kind == AssetKind::crypto ? L"api.binance.com" : L"fapi.binance.com";
      requests.emplace_back(host, path);
    }
    const auto responses = get_https_batch(requests);
    for (size_t index = 0; index < assets.size(); ++index) {
      const auto& asset = assets[index];
      const auto& body = responses[index];
      if (!body)
        continue;
      const auto price = as_double(json_string(*body, "lastPrice"));
      const auto change = as_double(json_string(*body, "priceChangePercent"));
      if (price && change)
        store_.update(std::string{asset.symbol}, "BINANCE", *price, *change);
    }
  }
};

class OkxPoller final : public ExchangePoller {
public:
  explicit OkxPoller(MarketStore& store) : ExchangePoller{store} {}

private:
  void poll() override {
    std::vector<HttpsRequest> requests;
    requests.reserve(assets.size());
    for (const auto& asset : assets) {
      const std::wstring symbol{asset.symbol.begin(), asset.symbol.end()};
      const auto path = asset.kind == AssetKind::crypto
                            ? std::format(L"/api/v5/market/ticker?instId={}-USDT", symbol)
                            : std::format(L"/api/v5/market/ticker?instId={}-USDT-SWAP", symbol);
      requests.emplace_back(L"www.okx.com", path);
    }
    const auto responses = get_https_batch(requests);
    for (size_t index = 0; index < assets.size(); ++index) {
      const auto& asset = assets[index];
      const auto& body = responses[index];
      if (!body)
        continue;
      const auto price = as_double(json_string(*body, "last"));
      const auto open = as_double(json_string(*body, "open24h"));
      if (price && open && *open != 0.0)
        store_.update(std::string{asset.symbol}, "OKX", *price, (*price / *open - 1.0) * 100.0);
    }
  }
};

std::string color_change(double value) {
  return std::format("{}{:>+7.2f}%\x1b[0m", value >= 0.0 ? "\x1b[38;5;42m" : "\x1b[38;5;203m",
                     value);
}

std::string price_string(double value) { return std::format("{:.2f}", value); }

void render(const MarketStore& store) {
  const auto data = store.snapshot();
  const auto now = std::chrono::steady_clock::now();
  std::ostringstream out;
  out << "\x1b[H\x1b[J\x1b[38;5;45m  GLOBAL MARKET  \x1b[0m  LIVE / USDT\n"
      << "  ─────────────────────────────────────────────────────\n"
      << "  ASSET     TYPE              PRICE        24H     STATUS\n"
      << "  ─────────────────────────────────────────────────────\n";
  bool any_fresh = false;
  for (const auto& asset : assets) {
    const auto row = data.find(std::string{asset.symbol});
    std::optional<Quote> quote;
    if (row != data.end()) {
      if (const auto it = row->second.find("BINANCE");
          it != row->second.end() && now - it->second.updated <= 10s)
        quote = it->second;
      if (!quote)
        if (const auto it = row->second.find("OKX"); it != row->second.end())
          quote = it->second;
    }
    const bool stale = quote && now - quote->updated > 10s;
    any_fresh = any_fresh || (quote && !stale);
    const auto type = asset.kind == AssetKind::crypto ? "CRYPTO" : "STOCK PERP";
    const auto price = quote ? price_string(quote->price) : "--";
    const auto change = quote ? color_change(quote->change24h) : std::string{"      --"};
    const auto status = !quote  ? "\x1b[38;5;244m    WAIT\x1b[0m"
                        : stale ? "\x1b[38;5;214m   STALE\x1b[0m"
                                : "\x1b[38;5;42m    LIVE\x1b[0m";
    out << std::format("  \x1b[1m{:<6}\x1b[0m  {:<10}  {:>14}  {}  {}\n", asset.symbol, type, price,
                       change, status);
  }
  out << "  ─────────────────────────────────────────────────────\n  DATA ";
  out << (any_fresh ? "\x1b[38;5;42m● ONLINE\x1b[0m"
                    : "\x1b[38;5;203m● RECONNECTING\x1b[0m")
      << "   \x1b[38;5;244mCtrl+C to exit\x1b[0m";
  std::cout << out.str() << std::flush;
}

void configure_terminal() {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode{};
  if (GetConsoleMode(output, &mode))
    SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

} // namespace

int main() {
#ifndef _WIN32
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    std::cerr << "Failed to initialize libcurl\n";
    return 1;
  }
#endif
  configure_terminal();
  std::signal(SIGINT, [](int) { running.store(false); });
  std::signal(SIGTERM, [](int) { running.store(false); });
  MarketStore store;
  BinancePoller binance{store};
  OkxPoller okx{store};
  std::array<ExchangePoller*, 2> pollers{&binance, &okx};
  for (auto* poller : pollers)
    poller->start();
  std::cout << "\x1b[?1049h\x1b[?25l\x1b[2J";
  while (running.load()) {
    render(store);
    std::this_thread::sleep_for(500ms);
  }
  for (auto* poller : pollers)
    poller->stop();
  std::cout << "\x1b[?25h\x1b[?1049l\x1b[0m" << std::flush;
#ifndef _WIN32
  curl_global_cleanup();
#endif
}
