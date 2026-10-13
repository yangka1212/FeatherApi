# SignPath 签名接入

当前状态：已准备 MIT 许可证、EXE 版本信息、签名政策和 GitHub Actions；
尚未获得 SignPath 批准，也没有配置签名凭据。现有 v2.0.0 发行文件未签名。

## 申请资料

- Project name: FeatherApi
- Repository / homepage: https://github.com/yangka1212/FeatherApi
- Downloads: https://github.com/yangka1212/FeatherApi/releases
- License: https://github.com/yangka1212/FeatherApi/blob/main/LICENSE
- Code signing policy: https://github.com/yangka1212/FeatherApi/blob/main/docs/code-signing.md
- Privacy policy: https://github.com/yangka1212/FeatherApi/blob/main/docs/code-signing.md#privacy-policy
- Build system: GitHub Actions, GitHub-hosted Windows runners
- Tagline: A lightweight native Windows API client for composing HTTP requests and inspecting responses.
- Description: FeatherApi is an MIT-licensed desktop API debugging tool for Windows. It provides a native interface for organizing requests, editing headers and request bodies, inspecting responses, and importing OpenAPI/Swagger definitions. Workspaces are stored locally, and network requests are initiated by the user.

申请入口：https://signpath.org/apply.html 。账号姓名和通知邮箱由维护者填写。
项目影响力一栏应如实引用仓库、发行历史、下载统计或真实的社区讨论，不能编造用户量或背书。
维护者在 GitHub 和 SignPath 开启双重验证，并阅读申请页中的条款和隐私授权后提交。

## 审核通过后配置

1. 在 SignPath 关联本 GitHub 仓库与 GitHub.com 构建来源；按官方接入要求安装其 GitHub App。
2. 创建项目及生产签名策略，设置手工批准者，并选择基金会提供的证书。
3. 配置签名输入：外层是 GitHub artifact 的 ZIP，内含 `FeatherApi.exe`；
   对该 PE 文件启用 Authenticode 签名和时间戳，约束产品名称为 `FeatherApi`，
   并检查产品版本。签名输出保留 `FeatherApi.exe` 路径。
4. 在仓库 **Settings → Secrets and variables → Actions** 添加：

| 类型 | 名称 | 内容 |
| --- | --- | --- |
| Secret | `SIGNPATH_API_TOKEN` | 具有提交权限的 SignPath API token |
| Variable | `SIGNPATH_ORGANIZATION_ID` | SignPath 分配的组织 ID |
| Variable | `SIGNPATH_PROJECT_SLUG` | 实际项目 slug |
| Variable | `SIGNPATH_SIGNING_POLICY_SLUG` | 实际生产签名策略 slug |

不要把 token 写进源码、提交记录或申请资料。标识符以 SignPath 实际配置为准。

## 构建与发行

`Windows build` 会在 main 推送、拉取请求或手动触发时编译、运行离线测试，
并生成名称含 `unsigned` 的 EXE、ZIP 和校验和。ZIP 包含 EXE 与 LICENSE。

首次签名需要使用包含这些准备文件的新版本提交和新 tag，旧 v2.0.0 tag
没有这些文件，不适合作为首次签名的输入。先更新 `resources.rc` 的数字和字符串版本，
例如准备 v2.0.1 时都改为 `2.0.1.0`，提交后再打 tag；不要移动已发布 tag。

将工作流合入 `main` 后，以已经推送的版本 tag 作为工作流运行的 ref。
例如在已登录 GitHub CLI 的终端运行：

```powershell
gh workflow run windows-sign.yml --repo yangka1212/FeatherApi --ref v2.0.1
```

不再单独填写 tag 输入参数；发行版本直接取自运行的 tag。
工作流只接受 tag 触发，检出该次运行记录的固定提交，并在构建前核对
`HEAD` 与 `GITHUB_SHA` 一致，确保 SignPath 来源记录对应实际构建的源码。
SignPath 生产签名策略也应允许这个版本 tag 对应的来源 ref。
随后工作流编译测试、核对 EXE 版本并提交签名申请。
维护者在 SignPath 手工批准后，工作流校验有效签名和时间戳，生成校验和，
最后创建 GitHub Release 草稿。维护者补全更新说明并检查附件后发布草稿。
缺少配置、版本不匹配或签名无效时会停止，不会退回未签名的正式发行包。

本地验证打包命令：

```powershell
./scripts/build.ps1 -Action Test -Configuration Release
./scripts/package-release.ps1
```

官方参考：[项目条件](https://signpath.org/terms.html)、
[GitHub 接入](https://docs.signpath.io/trusted-build-systems/github)。
