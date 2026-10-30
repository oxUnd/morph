# Setup MCP presets

Reviewed on 2026-10-30. Setup uses a small set of integrations that add capabilities
beyond Morph's existing tools. It does not install filesystem, shell, image
generation or video generation MCP servers to duplicate the built-in tools.

| Role | Defaults |
| --- | --- |
| General assistant | Exa |
| Software developer | Exa, Playwright; macOS Computer Use on macOS |
| Photographer | Exa; macOS Computer Use on macOS |
| Photo editor | Exa; macOS Computer Use on macOS |
| Writer | Exa |
| Designer | Exa, Figma desktop (manual connection); macOS Computer Use on macOS |
| Video creator | Exa; macOS Computer Use on macOS |
| Researcher | Exa, Playwright |

## Sources and requirements

- **Exa:** [official MCP documentation](https://exa.ai/docs/get-started/exa-mcp)
  documents `https://mcp.exa.ai/mcp`, keyless search/fetch and an optional advanced
  search tool. The default uses basic search and fetch without account setup.
  Keyless access has provider rate limits and network restrictions; it is not an
  unlimited or production-authenticated service. Existing authenticated or
  customized Exa definitions take precedence over the default.
- **Playwright:** [Microsoft's MCP server](https://github.com/microsoft/playwright-mcp)
  supports browser automation using accessibility snapshots. The preset pins
  `@playwright/mcp@0.0.83`, with `npx -y` as the launcher. Node.js 18+ is required;
  the browser may need installation after connecting. Setup checks for `npx`,
  not the complete browser runtime, and never runs the installer. Development
  uses it for browser inspection/testing; research uses it for interactive pages.
- **macOS Computer Use:** [upstream installation and requirements](https://github.com/Sur-Cai/macos-computer-use-kit)
  specify `uvx`, a compatible Python runtime, macOS and Accessibility/Screen
  Recording permissions. The preset pins `macos-computer-use-kit==0.3.2`, whose
  [package metadata](https://pypi.org/project/macos-computer-use-kit/0.3.2/)
  requires Python 3.10+. When the shipped Morph adapter is available, Python 3
  launches it and it wraps the pinned upstream server; otherwise the preset uses
  `uvx` directly. This provides desktop inspection and interaction for development
  and existing photographic/video applications without asserting that Photoshop,
  Lightroom or a video editor is installed. See the [adapter](macos-launch-adapter.md).
- **Figma desktop:** [Figma's official setup guide](https://developers.figma.com/docs/figma-mcp-server/local-server-installation/)
  documents `http://127.0.0.1:3845/mcp`. It requires an eligible Figma account and
  enabling the server in the running desktop application. The preset starts with
  `auto_connect=false`; after enabling it, use `/mcp connect figma-desktop`.
  The remote server's OAuth flow is a separate integration and is not emulated
  with a personal access token.

The browser and computer-use package versions are deliberate pins, not automatic
latest-version upgrades. Review upstream compatibility before changing them.
Setup stores no additional MCP API keys and makes no live capability checks.

## Domain-specific alternatives

Photography and post-production can involve different editors and catalog
systems. Their default desktop integration works with the user's installed
application; built-in vision and image tools remain available for analysis and
requested edits. A product-specific bridge should be added only when that
application and its prerequisites are known.

For example, the [DaVinci Resolve community bridge](https://github.com/samuelgursky/davinci-resolve-mcp)
depends on Resolve and its scripting integration. It is not a generic substitute
for a video model and is not installed into every video creator's configuration.
Similarly, [Adobe's Photoshop API authentication](https://developer.adobe.com/firefly-services/docs/photoshop/getting-started/)
requires an account-specific integration; desktop access does not imply a
configured cloud API. These can be configured explicitly through `morph mcp add`
when the user's editing environment is known.

## Preservation and lifecycle

Defaults are added in memory before final schema validation and the existing
atomic setup save. Cancellation creates no MCP configuration. Existing server
names, matching endpoints and known package aliases are retained without changing
their authentication, launch arguments or connection preference. Existing MCP
servers stay in their original order and new servers are appended. Re-running
setup is idempotent for the same defaults. Switching roles does not delete a
previously selected integration or upgrade its package.

Array-of-table configurations retain their blocks and comments. Existing inline
arrays are extended without serializing the other entries. A full server list
fails saving with a capacity error rather than discarding an existing server.
The final configuration is still backed up and published under the setup lock.

The Save screen shows role MCP recommendations and the approval/host defaults.
Actual connection occurs after restarting Morph; manually connected entries can
be managed through `/mcp`. Read-only auto-approval uses registered metadata,
including MCP annotations. Computer or browser actions that mutate state retain
their normal approval behavior. Built-in media exemptions do not turn third-party
editing tools into read-only operations or grant filesystem permissions.
