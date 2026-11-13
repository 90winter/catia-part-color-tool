# Catia 给 Part 随机赋予颜色

CATIA V5 CATPart 批量赋色工具的源码发布版。程序按顶层 Solid Body 的几何特征生成稳定颜色，并提供图形界面和静默批处理入口。

## 源码结构

- `Source/UI`：Windows 桌面界面与批处理调度源码。
- `Source/Worker`：CAA 独立 Worker 源码与构建描述。

## 构建要求

- UI：Visual Studio 2012 x64 C++ 工具链。
- Worker：已授权安装的 CATIA V5 B26 CAA/RADE 环境、对应 TCK profile，以及 Visual Studio 2012 x64 工具链。

CATIA SDK、运行时、DLL 和 CATIA 环境模板不随仓库发布。首次构建 Worker 前，请在 `Source/Worker/workspace_template/` 中放入你有权使用的本机 CAA 模板文件：`CATIAV5Level.lvl` 和 `Install_config_win_b64`。这两个文件可能含 Dassault Systèmes 专有内容或本机路径，勿提交到公开仓库。`build_worker.ps1` 的 `CatiaDir`、`TckProfile` 和 `WorkspaceDir` 参数可按本机安装覆盖。

在 `Source/UI` 目录运行 `build.ps1` 可构建界面程序。Worker 准备好本机模板后，在 `Source/Worker` 目录运行 `build_worker.ps1`。Worker 构建和部署需要完整的本地 CAA/RADE 配置。

## 使用说明

此仓库只发布源码，不包含可执行文件或运行时依赖。将程序用于 CATPart 前，请先在隔离副本上验证，并保留模型备份。

