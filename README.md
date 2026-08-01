# FeatherApi

**告别臃肿，以羽毛之力调试 API。**

FeatherApi 是一款面向 Windows 的极致轻量级 API 调试工具

![image-20260801222138326](C:\Users\user\AppData\Roaming\Typora\typora-user-images\image-20260801222138326.png)

## 为什么是 FeatherApi？

- **告别臃肿**：基于 Windows 原生 Win32 界面和系统网络栈，不依赖 Electron，也不捆绑浏览器运行时。
- **轻巧启动**：单文件原生应用形态，适合放在 U 盘、开发工具箱或 CI 工作机中使用。
- **接口管理**：支持目录、子目录、接口、请求用例和接口复制/移动/重命名。
- **完整请求编辑**：支持 GET、POST、PUT、PATCH、DELETE 等常见方法，查询参数、请求头、JSON、Raw 和表单请求均可编辑。
- **响应可读**：显示状态码、耗时、响应大小、响应头和原始/格式化后的响应体。
- **JSON 工具**：内置 JSON 校验、格式化和压缩，错误信息尽量定位到行列。
- **OpenAPI / Swagger**：可导入 OpenAPI 3 和 Swagger 2 文档，自动生成目录、请求参数与 JSON 示例。
- **本地优先**：接口数据保存到本机，不要求登录或云端账号。

## 轻量化指标

| 项目 | 当前情况 |
| --- | --- |
| 底层开发语言 | C++17 |
| UI 技术 | **Windows 原生 Win32 API + Common Controls** |
| HTTP 实现 | Windows WinHTTP |
| 第三方运行时 | 无 Electron / 无 .NET Runtime 要求 |
| 安装包大小 | **仅仅435kb** |
| 空闲内存占用 | **内存占用仅仅18.9MB** |

FeatherApi 的轻量来自架构选择：只依赖Windows系统组件,只要你是Windows系统,无需安装任何依赖即可极致享受API

## 获取与构建

### CMake

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## 数据位置

默认数据文件位于：

```text
%LOCALAPPDATA%\FeatherApi-Win32\data.json
```

## 许可证

许可证文件将在首次公开发布前补充。若你准备直接 fork 或分发，请先确认项目许可证状态。

