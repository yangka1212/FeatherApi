# 真实场景测试

## 一键运行

在可交互的 Windows 桌面会话中，从项目根目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-real.ps1
```

入口脚本依次构建当前源码并运行 CTest，然后执行公网 WinHTTP 测试、正式 EXE 的网络与数据测试、目录和编辑器核心流程，以及正式窗口的样式截图检查。任一步失败都会返回非零退出码。公网测试不登记进普通 CTest，离线构建无需联网。

需要 Windows x64、Visual Studio C++ x64 工具及 Windows SDK、CMake 3.20+、Ninja、Python 3.10+，并允许访问公网测试服务。端到端测试会启动实际 GUI，因此不能在没有交互桌面的 Windows Session 0 中运行。

Live HTTP 默认连接 [httpbin.org](https://httpbin.org)，它是实际远程 HTTP 服务，不是本机 mock。程序会发送 GET、POST、PUT、PATCH、DELETE，核对查询参数、自定义请求头、JSON、URL 编码表单、真实文件 multipart、重定向和 HTTP 404。也可用 `-LiveBaseUrl` 为网络测试、核心 GUI 流程和响应样式截图统一指定支持这些路径与 httpbin 格式回显的真实服务：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-real.ps1 `
  -LiveBaseUrl 'https://httpbin.example.com'
```

脚本还会通过正式 WinHTTP 代码下载并导入[官方 OpenAI OpenAPI JSON 规范](https://github.com/openai/openai-openapi/blob/main/openapi.json)，随后在正式 GUI 中再次验证在线导入和落盘。可用 `-OpenApiUrl` 指定另一份真实在线 JSON 规范；`-SkipOpenApiImport` 会明确跳过这两个导入场景。

默认端到端目标是公开的 [GitHub 仓库 API](https://api.github.com/repos/yangka1212/FeatherApi)，预期 HTTP 200 响应包含 `"full_name":"yangka1212/FeatherApi"`；脚本还会请求同域的不存在资源，检查真实 HTTP 404 响应。结果依赖该服务与当前网络状态。也可以指定自己的真实 API：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-real.ps1 `
  -Url 'https://api.example.com/health' `
  -ExpectedBodyFragment 'healthy'
```

自定义主接口返回其他状态码时可传入 `-ExpectedStatus`。可用 `-ErrorUrl`、`-ErrorStatus`、`-ErrorBodyFragment` 修改非 2xx 场景，例如：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-real.ps1 `
  -ErrorUrl 'https://api.example.com/missing' `
  -ErrorStatus 404 `
  -ErrorBodyFragment 'not found'
```

若自定义服务暂时没有稳定的非 2xx 地址，可用 `-SkipErrorResponse` 跳过生产 GUI 的该场景；脚本会明确输出 `SKIP`。这不会跳过 Live HTTP 对 httpbin `/status/404` 的检查。

若 `python` 不在 `PATH` 中，入口会尝试 Windows `py -3`；也可用 `-PythonCommand` 指定 Python 可执行文件。

## 真实测试与既有回归的边界

现有 `FeatherApiSelfTests` 使用真实临时文件和 Winsock 回环服务验证存储、WinHTTP 请求、响应截断与取消；JSON、OpenAPI 等规则在进程内检查。`FeatherApiUiTests --test` 创建真实 Win32 控件，但替换了加载、保存、关闭对话框，并手工构造网络完成消息。它适合控制器回归，不能算生产 EXE 的完整网络交互。

新增 `FeatherApiLiveHttpTests.exe` 直接调用应用的 WinHTTP 实现并访问远程 httpbin，验证请求和响应真实经过网络。它不替代窗口操作，所以后续的生产 EXE 端到端测试仍会实际启动和操作 GUI。

新增端到端测试运行生产 `FeatherApi.exe`，连接真实 HTTP 服务，检查界面响应及真实磁盘持久化。测试数据使用独立临时 `data.json`：测试器通过 `FEATHERAPI_DATA_PATH` 指定其路径，并在发送或保存前检查生产窗口是否读取了唯一的预置地址；隔离检查失败即停止，避免写入日常 `%LOCALAPPDATA%\FeatherApi-Win32\data.json`。`FEATHERAPI_DATA_PATH` 只用于明确指定测试数据路径，普通启动仍使用默认数据文件。

`scripts/core_e2e.py` 在正式窗口中操作目录、接口、请求方法、查询参数、请求头、JSON 正文和用例；把编辑后的 POST 发给真实公网接口，核对服务实际收到的内容，再检查复制、移动、重命名、删除、保存和重启后的文件结果。它还对真实延迟请求执行取消和关闭标签、最小化后恢复窗口，检查应用能继续请求。`scripts/visual_e2e.py` 在可见的正式窗口上检查主要控件的边界、可见性、间距、蓝色主按钮、保存主按钮图标与文字、方法徽标、选中下划线和空状态；还会发送真实 GET，检查非空 JSON 正文的语法色、Headers 标签和正文查找高亮。它按实际显示器 DPI 保存各场景 PNG 及 `report.json` 到 `build/real-visual/`。截图供人工复核，自动断言避免使用整图逐像素相等。

## 环境影响与仍需人工检查的项目

取消与关闭标签用例使用真实 `/delay/3` 接口。执行操作前，脚本要求请求仍处于发送状态，且 Windows TCP 连接表中有正式 EXE 所属的已建立连接；已完成的请求不会计为“请求中取消/关闭”通过。这验证客户端连接与未完成请求，不代表已经确认服务端收到完整请求内容。

所有 GUI 脚本会检查程序是否提前退出及正常关闭的退出码。视觉截图由独立辅助进程执行，默认每张最多等待 15 秒；可单独运行 `visual_e2e.py --capture-timeout 20` 调整。截图超时会终止辅助进程、记录诊断并清理测试 EXE。

受限沙箱中曾出现 WinHTTP/Schannel HTTPS 凭据初始化错误；在正常主机网络权限下，GitHub API 的 HTTPS 请求已返回 HTTP 200。这表明沙箱权限会影响网络测试结果。若默认目标失败，先确认当前会话的代理、证书、DNS、访问权限、httpbin 可用性与 GitHub API 限流情况，再用你实际使用的服务重测。

视觉脚本只把实际运行过的显示器 DPI 记为已验证。当前测试机实际 DPI 为 96；150% / 200% 缩放和多屏需要在对应真实显示设置下分别运行。托管桌面若无法对屏幕 DC 使用 BitBlt，脚本会对正式 HWND 使用 PrintWindow 捕获实际窗口渲染，并在报告中记录方式。

自动化检查无法替代中文输入法、不同 Windows 版本、复杂 hover/focus 状态和整体审美的人工复核。业务认证、代理、证书链、大文件上传与长时间请求，及安装包/便携包运行，还需在对应真实环境验证。
