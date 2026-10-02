# Espressif MCP 服务

[English](mcp-servers.md) | [文档索引](README_CN.md)

工作区提供 Codex、Claude Code 和 Cursor 的项目级 MCP 配置。
在客户端中打开工作区根目录（或对应 Git worktree 的根目录），以便发现配置。
两个服务均使用 Streamable HTTP，无需本地 MCP 进程、ESP-IDF 环境或开发板。

## 服务及使用时机

| 服务标识 | 地址 | 用途 |
| --- | --- | --- |
| `esp-pilot` | <https://mcp.esp-pilot.espressif.com/mcp> | 查询 ADF/GMF 多媒体能力与例程、Board Manager 参考资料和相关远程技能。 |
| `esp-component-registry` | <https://components.espressif.com/mcp> | 搜索 ESP-IDF 组件并获取组件文档。 |

选择组件时，先搜索，再获取选定组件的详情。集成前，应结合工作区固定的
ESP-IDF、目标芯片、组件版本及所属源码核对结果。远程建议不能替代
[AGENTS.md](../AGENTS.md) 中的应用、构建和设备操作流程。
服务能力见 [Espressif 服务介绍](https://mcp.espressif.com/)。

## 在客户端中启用

使用仓库已提交的对应客户端配置即可，无需执行 `mcp add`，也无需复制到全局设置。
配置仅包含服务名、URL 和客户端要求的传输类型，保留客户端默认超时与工具授权行为，
不包含凭据。本次接入验证中，下述只读查询无需认证即可完成。
若服务之后要求认证，请使用客户端登录流程，将凭据保存在客户端本地凭据存储中。

### Codex

共享配置为 [`.codex/config.toml`](../.codex/config.toml)。Codex 仅加载可信项目的
项目级配置。通过客户端正常流程打开并信任工作区，然后重启会话以加载服务。
在工作区根目录检查实际生效的配置：

```sh
codex mcp list
codex mcp get esp-pilot
codex mcp get esp-component-registry
```

这些命令展示配置，不代表工具调用成功。在交互式 Codex 会话中使用 `/mcp`
查看连接状态和可用工具，再执行下述只读查询。
详见 [Codex MCP 文档](https://developers.openai.com/codex/mcp)。

### Claude Code

共享配置为 [`.mcp.json`](../.mcp.json)。在工作区根目录启动 Claude Code，
完成工作区信任和项目 MCP 服务批准提示。检查服务是否被识别：

```sh
claude mcp list
claude mcp get esp-pilot
claude mcp get esp-component-registry
```

尚未批准的项目服务可能显示为待批准。批准后，这些命令也会检查服务健康状态。
在会话中使用 `/mcp` 查看状态和工具，再执行下述只读查询。
详见 [Claude Code 项目级配置](https://code.claude.com/docs/en/mcp#project-scope)。

### Cursor

共享配置为 [`.cursor/mcp.json`](../.cursor/mcp.json)。在 Cursor 中打开工作区根目录，
完成出现的工作区信任提示，然后进入 Cursor Settings 的 MCP 设置区域。
确认两个服务均已启用并连接，查看工具列表。修改配置后，如有必要，重启连接或
重新加载窗口。在 Agent 对话中执行下述只读查询，并按客户端提示批准工具调用。
详见 [Cursor MCP 文档](https://cursor.com/docs/mcp)。

## 验证实际查询

要求已连接的客户端执行以下 MCP 调用：

| 服务 | 工具 | 参数 | 预期结果 |
| --- | --- | --- | --- |
| `esp-component-registry` | `search_components` | `{"query":"espressif/esp-gsp"}` | 返回包含 `espressif/esp-gsp` 的组件结果。 |
| `esp-pilot` | `server_info` | `{}` | 返回 ESP Pilot 服务及目录数据源信息。 |

确认客户端实际调用了对应 MCP 工具，且结果不包含协议错误或工具错误。
仅看到配置处于启用状态不能证明连接成功。这些查询作为手动冒烟检查，
CI 不依赖公共服务在线状态。

## 常见问题

- **找不到服务或显示待批准：** 核对打开的工作区根目录、项目信任状态及服务的
  批准与启用状态；修改后重启客户端会话。
- **URL 不符或服务重复：** 检查客户端实际生效的设置，确认其他配置作用域中
  是否已有同名或相同地址的服务。整理本地配置时以上述地址为准。
- **连接失败或超时：** 检查 HTTPS 网络、代理、TLS 配置及服务可用性。
  应通过 MCP 客户端测试；在浏览器中打开 `/mcp` 不会执行 MCP 初始化握手。
- **要求认证：** 使用客户端针对该服务的认证流程，不把令牌写入共享配置。
- **服务不可用：** 使用所属源码及官方组件或框架文档继续工作，并注明 MCP 检查
  未能完成。固件构建和设备操作不依赖这两个服务。
