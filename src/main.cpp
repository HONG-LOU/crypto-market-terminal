#include <windows.h>
#include <winhttp.h>

#include <algorithm>
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

using namespace std::chrono_literals;

namespace {

enum class AssetKind { crypto, stock_perpetual };

struct Asset {
    std::string_view symbol;
    AssetKind kind;
};

constexpr std::array assets{
    Asset{"BTC", AssetKind::crypto}, Asset{"ETH", AssetKind::crypto},
    Asset{"SOL", AssetKind::crypto}, Asset{"BNB", AssetKind::crypto},
    Asset{"XRP", AssetKind::crypto}, Asset{"DOGE", AssetKind::crypto},
    Asset{"TSLA", AssetKind::stock_perpetual}, Asset{"NVDA", AssetKind::stock_perpetual},
    Asset{"AAPL", AssetKind::stock_perpetual}, Asset{"AMZN", AssetKind::stock_perpetual},
    Asset{"META", AssetKind::stock_perpetual}, Asset{"MSFT", AssetKind::stock_perpetual},
    Asset{"COIN", AssetKind::stock_perpetual}, Asset{"MSTR", AssetKind::stock_perpetual},
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
        quotes_[std::move(symbol)][std::move(exchange)] = {price, change, std::chrono::steady_clock::now()};
    }
    [[nodiscard]] auto snapshot() const {
        std::scoped_lock lock{mutex_};
        return quotes_;
    }
private:
    mutable std::mutex mutex_;
    std::map<std::string, std::map<std::string, Quote>> quotes_;
};

struct InternetHandle {
    HINTERNET value{};
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : value{handle} {}
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

std::optional<std::string> get_https(std::wstring_view host, std::wstring_view path) {
    InternetHandle session{WinHttpOpen(L"CryptoMarketTerminal/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value) return std::nullopt;
    WinHttpSetTimeouts(session.value, 3'000, 3'000, 3'000, 5'000);
    InternetHandle connection{WinHttpConnect(session.value, host.data(), INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.value) return std::nullopt;
    InternetHandle request{WinHttpOpenRequest(connection.value, L"GET", path.data(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE)};
    if (!request.value || !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) return std::nullopt;
    DWORD status{}, status_size = sizeof(status);
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) return std::nullopt;
    std::string body;
    for (;;) {
        DWORD available{};
        if (!WinHttpQueryDataAvailable(request.value, &available) || available == 0) break;
        const auto offset = body.size();
        body.resize(offset + available);
        DWORD read{};
        if (!WinHttpReadData(request.value, body.data() + offset, available, &read)) return std::nullopt;
        body.resize(offset + read);
    }
    return body;
}

std::optional<std::string_view> json_string(std::string_view object, std::string_view key) {
    const auto marker = std::format("\"{}\":\"", key);
    const auto start = object.find(marker);
    if (start == std::string_view::npos) return std::nullopt;
    const auto value_start = start + marker.size();
    const auto end = object.find('"', value_start);
    if (end == std::string_view::npos) return std::nullopt;
    return object.substr(value_start, end - value_start);
}

std::optional<double> as_double(std::optional<std::string_view> value) {
    if (!value) return std::nullopt;
    double result{};
    const auto [ptr, error] = std::from_chars(value->data(), value->data() + value->size(), result);
    if (error != std::errc{} || ptr != value->data() + value->size()) return std::nullopt;
    return result;
}

class ExchangePoller {
public:
    ExchangePoller(std::string name, MarketStore& store) : store_{store}, name_{std::move(name)} {}
    virtual ~ExchangePoller() { stop(); }
    void start() { worker_ = std::jthread([this](std::stop_token stop) { run(stop); }); }
    void stop() { if (worker_.joinable()) { worker_.request_stop(); worker_.join(); } }
    [[nodiscard]] bool healthy() const { return healthy_.load(); }
    [[nodiscard]] const std::string& name() const { return name_; }
protected:
    virtual bool poll() = 0;
    MarketStore& store_;
private:
    void run(std::stop_token stop) {
        while (!stop.stop_requested() && running.load()) {
            healthy_.store(poll());
            for (int i = 0; i < 10 && !stop.stop_requested() && running.load(); ++i) std::this_thread::sleep_for(100ms);
        }
    }
    std::string name_;
    std::jthread worker_;
    std::atomic_bool healthy_{false};
};

class BinancePoller final : public ExchangePoller {
public:
    explicit BinancePoller(MarketStore& store) : ExchangePoller{"BINANCE", store} {}
private:
    bool poll() override {
        bool updated = false;
        for (const auto& asset : assets) {
            const std::wstring symbol{asset.symbol.begin(), asset.symbol.end()};
            const auto path = asset.kind == AssetKind::crypto
                ? std::format(L"/api/v3/ticker/24hr?symbol={}USDT", symbol)
                : std::format(L"/fapi/v1/ticker/24hr?symbol={}USDT", symbol);
            const auto host = asset.kind == AssetKind::crypto ? L"api.binance.com" : L"fapi.binance.com";
            const auto body = get_https(host, path);
            if (!body) continue;
            const auto price = as_double(json_string(*body, "lastPrice"));
            const auto change = as_double(json_string(*body, "priceChangePercent"));
            if (price && change) { store_.update(std::string{asset.symbol}, "BINANCE", *price, *change); updated = true; }
        }
        return updated;
    }
};

class OkxPoller final : public ExchangePoller {
public:
    explicit OkxPoller(MarketStore& store) : ExchangePoller{"OKX", store} {}
private:
    bool poll() override {
        bool updated = false;
        for (const auto& asset : assets) {
            const std::wstring symbol{asset.symbol.begin(), asset.symbol.end()};
            const auto path = asset.kind == AssetKind::crypto
                ? std::format(L"/api/v5/market/ticker?instId={}-USDT", symbol)
                : std::format(L"/api/v5/market/ticker?instId={}-USDT-SWAP", symbol);
            const auto body = get_https(L"www.okx.com", path);
            if (!body) continue;
            const auto price = as_double(json_string(*body, "last"));
            const auto open = as_double(json_string(*body, "open24h"));
            if (price && open && *open != 0.0) {
                store_.update(std::string{asset.symbol}, "OKX", *price, (*price / *open - 1.0) * 100.0);
                updated = true;
            }
        }
        return updated;
    }
};

std::string color_change(double value) {
    return std::format("{}{:>+7.2f}%\x1b[0m", value >= 0.0 ? "\x1b[38;5;42m" : "\x1b[38;5;203m", value);
}

std::string price_string(double value) { return std::format("{:.2f}", value); }

void render(const MarketStore& store, const std::array<ExchangePoller*, 2>& pollers) {
    const auto data = store.snapshot();
    const auto now = std::chrono::steady_clock::now();
    std::ostringstream out;
    out << "\x1b[H\x1b[J\x1b[38;5;45m  GLOBAL MARKET  \x1b[0m  LIVE / USDT\n"
        << "  ─────────────────────────────────────────────────────\n"
        << "  ASSET     TYPE              PRICE        24H     STATUS\n"
        << "  ─────────────────────────────────────────────────────\n";
    for (const auto& asset : assets) {
        const auto row = data.find(std::string{asset.symbol});
        std::optional<Quote> quote;
        if (row != data.end()) {
            if (const auto it = row->second.find("BINANCE"); it != row->second.end() && now - it->second.updated <= 10s) quote = it->second;
            if (!quote) if (const auto it = row->second.find("OKX"); it != row->second.end()) quote = it->second;
        }
        const bool stale = quote && now - quote->updated > 10s;
        const auto type = asset.kind == AssetKind::crypto ? "CRYPTO" : "STOCK PERP";
        const auto price = quote ? price_string(quote->price) : "--";
        const auto change = quote ? color_change(quote->change24h) : std::string{"      --"};
        const auto status = !quote ? "\x1b[38;5;244m    WAIT\x1b[0m" : stale ? "\x1b[38;5;214m   STALE\x1b[0m" : "\x1b[38;5;42m    LIVE\x1b[0m";
        out << std::format("  \x1b[1m{:<6}\x1b[0m  {:<10}  {:>14}  {}  {}\n",
                           asset.symbol, type, price, change, status);
    }
    out << "  ─────────────────────────────────────────────────────\n  DATA ";
    const bool any_healthy = std::ranges::any_of(pollers, [](const auto* poller) { return poller->healthy(); });
    out << (any_healthy ? "\x1b[38;5;42m● ONLINE\x1b[0m" : "\x1b[38;5;203m● RECONNECTING\x1b[0m")
        << "   \x1b[38;5;244mCtrl+C to exit\x1b[0m";
    std::cout << out.str() << std::flush;
}

void configure_terminal() {
    SetConsoleOutputCP(CP_UTF8);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode{};
    if (GetConsoleMode(output, &mode)) SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

} // namespace

int main() {
    configure_terminal();
    std::signal(SIGINT, [](int) { running.store(false); });
    std::signal(SIGTERM, [](int) { running.store(false); });
    MarketStore store;
    BinancePoller binance{store};
    OkxPoller okx{store};
    std::array<ExchangePoller*, 2> pollers{&binance, &okx};
    for (auto* poller : pollers) poller->start();
    std::cout << "\x1b[?1049h\x1b[?25l\x1b[2J";
    while (running.load()) { render(store, pollers); std::this_thread::sleep_for(500ms); }
    for (auto* poller : pollers) poller->stop();
    std::cout << "\x1b[?25h\x1b[?1049l\x1b[0m" << std::flush;
}
