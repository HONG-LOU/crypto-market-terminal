# Crypto Market Terminal

一个使用 C++23 编写的实时加密货币行情终端。程序通过公开 HTTPS 行情接口并发接入 Binance 和 OKX，展示 BTC、ETH、SOL、BNB、XRP、DOGE 的 USDT 最新价、跨所均价、24 小时涨跌和价差。

## 特性

- Binance 与 OKX 双数据源约 1 秒刷新
- 自动重试、请求超时和数据过期提示
- 固定刷新界面，无滚屏日志干扰
- 多线程回调下的线程安全行情快照
- CMake 自动下载开源依赖，无需 API Key

## 构建

需要 Windows 10/11、CMake 3.25+ 和支持 C++23 的编译器。网络层使用 Windows 原生 WinHTTP，无第三方运行库。

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

## 运行

```powershell
.\build\crypto-market-terminal.exe
```

按 `Ctrl+C` 平稳退出。行情仅供展示，不构成投资建议。
