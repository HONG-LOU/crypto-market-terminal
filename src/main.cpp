#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
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

constexpr std::array<std::string_view, 6> coins{"BTC", "ETH", "SOL", "BNB", "XRP", "DOGE"};
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
        for (const auto coin : coins) {
            const auto path = std::format(L"/api/v3/ticker/24hr?symbol={}USDT", std::wstring{coin.begin(), coin.end()});
            const auto body = get_https(L"api.binance.com", path);
            if (!body) continue;
            const auto price = as_double(json_string(*body, "lastPrice"));
            const auto change = as_double(json_string(*body, "priceChangePercent"));
            if (price && change) { store_.update(std::string{coin}, "BINANCE", *price, *change); updated = true; }
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
        for (const auto coin : coins) {
            const auto path = std::format(L"/api/v5/market/ticker?instId={}-USDT", std::wstring{coin.begin(), coin.end()});
            const auto body = get_https(L"www.okx.com", path);
            if (!body) continue;
            const auto price = as_double(json_string(*body, "last"));
            const auto open = as_double(json_string(*body, "open24h"));
            if (price && open && *open != 0.0) {
                store_.update(std::string{coin}, "OKX", *price, (*price / *open - 1.0) * 100.0);
                updated = true;
            }
        }
        return updated;
    }
};

std::string color_change(double value) {
    return std::format("{}{:+.2f}%\x1b[0m", value >= 0.0 ? "\x1b[38;5;42m" : "\x1b[38;5;203m", value);
}

std::string price_string(double value) {
    if (value >= 1000.0) return std::format("{:.2f}", value);
    if (value >= 1.0) return std::format("{:.4f}", value);
    return std::format("{:.6f}", value);
}

void render(const MarketStore& store, const std::array<ExchangePoller*, 2>& pollers) {
    const auto data = store.snapshot();
    const auto now = std::chrono::steady_clock::now();
    std::ostringstream out;
    out << "\x1b[H\x1b[2J\x1b[38;5;45m  CRYPTO MARKET  \x1b[0m  LIVE / USDT\n"
        << "  ───────────────────────────────────────────────────────────────────────────\n"
        << "  ASSET       BINANCE          OKX          AVG PRICE      24H AVG    SPREAD\n"
        << "  ───────────────────────────────────────────────────────────────────────────\n";
    for (const auto coin : coins) {
        const auto row = data.find(std::string{coin});
        std::optional<Quote> binance, okx;
        if (row != data.end()) {
            if (const auto it = row->second.find("BINANCE"); it != row->second.end()) binance = it->second;
            if (const auto it = row->second.find("OKX"); it != row->second.end()) okx = it->second;
        }
        const auto cell = [&](const std::optional<Quote>& quote) {
            if (!quote) return std::string{"--"};
            return std::format("{}{}", now - quote->updated > 10s ? "\x1b[38;5;244m" : "", price_string(quote->price));
        };
        double average{}, change{}, spread{};
        int count{};
        for (const auto& quote : {binance, okx}) if (quote) { average += quote->price; change += quote->change24h; ++count; }
        if (count) { average /= count; change /= count; }
        if (binance && okx && average != 0.0) spread = std::abs(binance->price - okx->price) / average * 100.0;
        out << std::format("  \x1b[1m{:<6}\x1b[0m  {:>14}\x1b[0m  {:>14}\x1b[0m  {:>14}  {:>18}  {:>6.3f}%\n",
                           coin, cell(binance), cell(okx), count ? price_string(average) : "--",
                           count ? color_change(change) : "--", spread);
    }
    out << "  ───────────────────────────────────────────────────────────────────────────\n  ";
    for (const auto* poller : pollers) out << (poller->healthy() ? "\x1b[38;5;42m●\x1b[0m " : "\x1b[38;5;203m●\x1b[0m ") << poller->name() << "   ";
    out << "  \x1b[38;5;244mCtrl+C to exit\x1b[0m\n";
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
    std::cout << "\x1b[?25l";
    while (running.load()) { render(store, pollers); std::this_thread::sleep_for(500ms); }
    for (auto* poller : pollers) poller->stop();
    std::cout << "\x1b[?25h\x1b[0m\n";
}
