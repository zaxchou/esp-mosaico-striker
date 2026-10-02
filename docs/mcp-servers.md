# Espressif MCP services

[简体中文](mcp-servers_CN.md) | [Documentation index](README.md)

This workspace includes project-scoped MCP configuration for Codex, Claude Code
and Cursor. Open the workspace root (or the root of its Git worktree) in your
client so it can discover the configuration. The servers use Streamable HTTP;
no local MCP process, ESP-IDF installation or board is needed.

## Services and when to use them

| Server identifier | Endpoint | Use |
| --- | --- | --- |
| `esp-pilot` | <https://mcp.esp-pilot.espressif.com/mcp> | Find ADF/GMF multimedia capabilities and examples, Board Manager references and relevant remote skills. |
| `esp-component-registry` | <https://components.espressif.com/mcp> | Search for ESP-IDF components and retrieve component documentation. |

For component selection, search first, then retrieve the selected component's
details. Check results against this workspace's pinned ESP-IDF, target and
component versions and the owning source before integration. Remote recommendations
do not replace the application, build or device workflows in [AGENTS.md](../AGENTS.md).
See the [Espressif service overview](https://mcp.espressif.com/) for service capabilities.

## Enable in your client

Use the committed configuration for your client; there is no need to run an
`mcp add` command or copy these entries into global settings. Configuration files
contain only server names, URLs and the transport where required. They retain
client defaults for timeouts and tool authorization and contain no credentials.
The read-only checks below succeeded without authentication when this integration
was verified. If a service later requests authentication, use the client's login
flow and keep credentials in its local credential store.

### Codex

The shared configuration is [`.codex/config.toml`](../.codex/config.toml).
Codex loads project configuration only for trusted projects. Open and trust this
workspace through the client's normal flow, then restart the session to load the
servers. From the workspace root, check the effective configuration:

```sh
codex mcp list
codex mcp get esp-pilot
codex mcp get esp-component-registry
```

These commands show configuration, not successful tool calls. In an interactive
Codex session, use `/mcp` to inspect connection status and available tools, then
run the read-only checks below. See [Codex MCP documentation](https://developers.openai.com/codex/mcp).

### Claude Code

The shared configuration is [`.mcp.json`](../.mcp.json). Start Claude Code at the
workspace root and complete its workspace trust and project MCP approval prompts.
Check discovery with:

```sh
claude mcp list
claude mcp get esp-pilot
claude mcp get esp-component-registry
```

Unapproved project servers can appear as pending approval. After approval, these
commands also check server health. Use `/mcp` in the session to inspect status and
tools, then run the read-only checks below. See [Claude Code project scope](https://code.claude.com/docs/en/mcp#project-scope).

### Cursor

The shared configuration is [`.cursor/mcp.json`](../.cursor/mcp.json). Open this
workspace root in Cursor, complete any workspace trust prompts, and open the MCP
section of Cursor Settings. Confirm both servers are enabled and connected and
inspect their tool lists. Restart the connection or reload the window after
configuration changes if necessary. Run the read-only checks below in Agent chat,
following the client's tool approval prompts. See [Cursor MCP documentation](https://cursor.com/docs/mcp).

## Verify useful queries

Ask the connected client to perform these specific MCP calls:

| Server | Tool | Arguments | Expected result |
| --- | --- | --- | --- |
| `esp-component-registry` | `search_components` | `{"query":"espressif/esp-gsp"}` | Component results including `espressif/esp-gsp`. |
| `esp-pilot` | `server_info` | `{}` | ESP Pilot service and catalog source metadata. |

Confirm the client actually invokes the named MCP tool and receives a result
without a protocol error or tool error. An enabled configuration entry alone is
not a connection check. These queries are manual smoke checks; CI does not depend
on the public servers being online.

## Troubleshooting

- **Server missing or pending:** check the opened workspace root, project trust
  and server approval/enablement. Restart the client session after changes.
- **Unexpected URL or duplicate server:** inspect the effective client settings
  for an existing server with the same identifier or endpoint in another scope.
  Use the endpoints above when resolving the local configuration.
- **Connection failure or timeout:** check HTTPS access, proxy/TLS configuration
  and service availability. Test through the MCP client; opening `/mcp` in a
  browser does not perform the MCP initialization handshake.
- **Authentication requested:** follow the client's authentication flow for that
  server. Do not put tokens in the shared configuration.
- **Service unavailable:** continue with the owning source and official component
  or framework documentation, and report that the MCP check could not complete.
  Firmware builds and device operations do not depend on these services.
