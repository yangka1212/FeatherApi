# FeatherApi

**告别臃肿，以羽毛之力调试 API。**<br>
**Say goodbye to bloat and debug APIs with the power of a feather.**

FeatherApi 是一款面向 Windows 的极致轻量级 API 调试工具。<br>
FeatherApi is an ultra-lightweight API debugging tool for Windows.

![FeatherApi overview](docs/images/featherapi-overview.png)

## 为什么是 FeatherApi？ / Why FeatherApi?

- **告别臃肿**：基于 Windows 原生 Win32 界面和系统网络栈，不依赖 Electron，也不捆绑浏览器运行时。<br>
  **No bloat**: Built with the native Windows Win32 UI and system networking stack, without Electron or a bundled browser runtime.
- **轻巧启动**：单文件原生应用形态，适合放在 U 盘、开发工具箱或 CI 工作机中使用。<br>
  **Fast and portable**: A native single-file application that is convenient to carry on a USB drive, in a developer toolkit, or on a CI workstation.
- **接口管理**：支持目录、子目录、接口、请求用例和接口复制、移动、重命名。<br>
  **API organization**: Manage folders, subfolders, APIs, request examples, and copy, move, or rename APIs.
- **完整请求编辑**：支持 GET、POST、PUT、PATCH、DELETE 等常见方法，查询参数、请求头、JSON、Raw 和表单请求均可编辑。<br>
  **Complete request editing**: Supports common methods such as GET, POST, PUT, PATCH, and DELETE, with editable query parameters, headers, JSON, raw, and form requests.
- **响应可读**：显示状态码、耗时、响应大小、响应头和原始或格式化后的响应体。<br>
  **Readable responses**: View the status code, elapsed time, response size, response headers, and raw or formatted response body.
- **JSON 工具**：内置 JSON 校验、格式化和压缩，错误信息尽量定位到行列。<br>
  **JSON tools**: Built-in JSON validation, formatting, and minification, with errors located as precisely as possible by line and column.
- **OpenAPI / Swagger**：可导入 OpenAPI 3 和 Swagger 2 文档，自动生成目录、请求参数与 JSON 示例。<br>
  **OpenAPI / Swagger**: Import OpenAPI 3 and Swagger 2 documents to automatically generate folders, request parameters, and JSON examples.
- **本地优先**：接口数据保存到本机，不要求登录或云端账号。<br>
  **Local-first**: API data is stored locally, with no login or cloud account required.

## 轻量化指标 / Lightweight Metrics

| 项目 / Item | 当前情况 / Current status |
| --- | --- |
| 底层开发语言 / Core language | C++17 |
| UI 技术 / UI technology | **Windows 原生 Win32 API + Common Controls** |
| HTTP 实现 / HTTP implementation | Windows WinHTTP |
| 第三方运行时 / Third-party runtime | No Electron / No .NET Runtime required |
| 安装包大小 / Package size | **约 435 KB / Approximately 435 KB** |
| 空闲内存占用 / Idle memory usage | **约 18.9 MB / Approximately 18.9 MB** |

FeatherApi 的轻量来自架构选择：只依赖 Windows 系统组件，无需安装额外依赖即可使用。<br>
FeatherApi stays lightweight by relying only on Windows system components, so no additional dependencies are required.

## 获取与构建 / Download and Build

### 下载发行版 / Download a Release

Windows x64 用户可以从 [最新发行版 / latest release](https://github.com/yangka1212/FeatherApi/releases/latest) 下载可直接运行的 ZIP 包。

Windows x64 users can download the portable ZIP package from the [latest release](https://github.com/yangka1212/FeatherApi/releases/latest).

### CMake

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## 数据位置 / Data Location

默认数据文件位于：<br>
The default data file is located at:

```text
%LOCALAPPDATA%\FeatherApi-Win32\data.json
```

## 许可证 / License

许可证文件将在首次公开发布前补充。若你准备直接 fork 或分发，请先确认项目许可证状态。<br>
The license file will be added before the first public release. If you plan to fork or redistribute this project, please confirm its license status first.
