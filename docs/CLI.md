# Caesium Image Compressor - 命令行使用文档

Caesium 在启动后监听一个本机命名管道，命令行（CLI）可以直接操作正在运行的软件：
添加/移除图片、设置压缩选项、启动/取消压缩、查询进度与结果。所有操作都会**实时同步到软件界面**。

## 目录

- [快速开始](#快速开始)
- [安装与配置](#安装与配置)
- [基础用法](#基础用法)
- [命令参考](#命令参考)
  - [`add`](#add-添加文件或文件夹)
  - [`remove`](#remove-移除列表项)
  - [`clear`](#clear-清空列表)
  - [`list`](#list-查看列表)
  - [`status`](#status-查询状态)
  - [`compress`](#compress-压缩)
  - [`cancel`](#cancel-取消压缩)
  - [`settings`](#settings-读写压缩选项)
  - [`path`](#path-管理-path)
  - [`show` / `quit`](#show--quit)
  - [`shell`](#shell-交互式会话)
  - [`help` / `--version`](#help--version)
  - [`--raw`](#raw-发送原始命令)
- [全局选项](#全局选项)
- [退出码](#退出码)
- [机器可读输出（AI / 脚本）](#机器可读输出ai--脚本)
- [典型示例](#典型示例)
- [常见问题](#常见问题)

---

## 快速开始

```bash
# 软件没开也没关系：CLI 会自动启动软件，等它就绪后执行命令
caesium-cli compress ./photos -q 80 -o ./out --wait
```

软件运行期间：

```bash
caesium-cli status            # 查看当前状态
caesium-cli list              # 查看图片列表
caesium-cli settings set jpeg_quality 85
caesium-cli compress --wait    # 用当前界面上的选项开始压缩
caesium-cli quit               # 关闭软件
```

> 默认情况下，若软件未运行，CLI 会**自动启动**它并等待就绪后执行命令。用 `--no-launch` 可关闭该行为。

---

## 安装与配置

CLI 就是可执行程序本身，区别只是启动参数。能调用它的前提是命令行能找到它。

### 方式一：安装版（推荐）

安装器（setup.iss）会自动：
1. 安装 `caesium-cli.cmd` 到应用目录；
2. 把应用目录写入**用户 PATH**。

装完重开一个终端，即可直接使用 `caesium-cli`。

### 方式二：绿色版 / 开发目录

把应用目录加入用户 PATH：

```powershell
[Environment]::SetEnvironmentVariable("Path", $env:Path + ";<应用目录>", "User")
```

或者使用设置界面：**设置 → 命令行 (CLI) → 添加到 PATH**（也可用 `caesium-cli path add`）。

改完 PATH 后**重开终端**生效。

### 方式三：不改 PATH

- 在 exe 所在目录内使用：`.\caesium-cli status`
- 或输入完整路径：`"<路径>\Caesium Image Compressor.exe" --cli status`

### 关于 `caesium-cli.cmd`

Windows 上 exe 是 GUI 子系统程序，直接在 PowerShell 中调用可能不会被等待。`caesium-cli.cmd`（与 exe 同目录）解决了这个问题，会自动等待命令结束并透传退出码。**它必须和 exe 放在同一目录**；"添加到 PATH" 时软件会自动生成它。

- Windows（cmd / PowerShell）：`caesium-cli <命令> ...`
- Linux / macOS：直接使用 `caesium-image-compressor --cli <命令> ...`（原生阻塞，无需包装脚本）

---

## 基础用法

```bash
# 统一格式
caesium-cli <命令> [参数...] [选项...]

# 全局选项可放在任意位置
caesium-cli status --json
caesium-cli --json status
```

- 路径参数支持文件、文件夹（文件夹按设置自动递归扫描支持的图片）。
- 所有"更改软件状态"的命令都会让软件界面同步更新。

---

## 命令参考

### `add` 添加文件或文件夹

```bash
caesium-cli add <路径...>
```

把文件 / 文件夹中的图片加入列表。返回导入数量。

```
Imported 42 image(s).
```

### `remove` 移除列表项

```bash
caesium-cli remove <路径...>
```

按完整路径精确匹配移除。

### `clear` 清空列表

```bash
caesium-cli clear
```

### `list` 查看列表

```bash
caesium-cli list
caesium-cli list --json
```

人类可读表格（文件名过长时以 `...` 结尾表示被截断，格式单独一列，方便看清扩展名）：

```
Name                             Format  Status      Original     Saved
----------------------------------------------------------------------------
a1.png                           PNG     waiting          6.29 KB        0%
spine_xinshouyindao_shou图集(256...  PNG     waiting         20.27 KB        0%
```

### `status` 查询状态

```bash
caesium-cli status
caesium-cli status --wait     # 持续刷新直到压缩结束
caesium-cli status --json
```

显示是否在压缩、进度、各项状态（等待 / 压缩中 / 已压缩 / 错误）与汇总。

### `compress` 压缩

```bash
# 一条龙：添加路径 + 设置选项 + 开始压缩
caesium-cli compress ./photos -q 80 --format jpg -o ./out --wait

# 用当前界面上的选项压缩现有列表
caesium-cli compress --wait

# 仅重试失败的项
caesium-cli compress --only-failed --wait

# 只预览将要执行的操作，不实际压缩
caesium-cli compress ./photos -q 80 --dry-run

# 不等待，立即返回（进度用 status 查询）
caesium-cli compress
```

`--wait` 会阻塞直到压缩完成，显示进度并打印汇总：

```
Compression finished.
  Images:      42
  Original:    120.5 MB
  Compressed:  48.2 MB
  Saved:       72.3 MB (60.0%)
```

**压缩选项**（与设置界面对应）：

| 选项 | 说明 |
|---|---|
| `-q, --quality <0-100>` | 同时设置 JPEG/PNG/WebP 质量 |
| `--jpeg-quality` `--png-quality` `--webp-quality` | 单独设置某种格式质量 |
| `--format <jpg\|png\|webp\|tiff\|same>` | 输出格式（`same`=保持原格式） |
| `-o, --output <目录>` | 输出文件夹 |
| `--suffix <文本>` | 输出文件后缀 |
| `--mode <quality\|size>` | 压缩模式 |
| `--fit <none\|dimensions\|percentage\|short_edge\|long_edge\|fixed_width\|fixed_height>` | 缩放方式 |
| `--width <px>` `--height <px>` `--edge <px>` | 缩放尺寸 |
| `--lossless` | 无损压缩 |
| `--keep-metadata` `--keep-dates` | 保留元数据 / 日期 |
| `--keep-structure` | 保留目录结构 |
| `--same-folder-as-input` | 输出到输入目录 |
| `--skip-if-bigger` | 压缩后变大则跳过 |
| `--cache-originals` | 缓存原始文件以便还原 |
| `--delete-original-format-file` | 转换格式后删除原文件 |
| `--keep-aspect-ratio` `--do-not-enlarge` | 保持比例 / 不放大 |
| `--jpeg-progressive` | 渐进式 JPEG |
| `--max-output-size <n>` | 目标输出大小 |
| `--max-output-size-unit <bytes\|kb\|mb\|percentage>` | 大小单位 |
| `--only-failed` | 只重新压缩失败项 |
| `--wait` | 等待完成并打印汇总 |
| `--dry-run` | 只预览，不执行 |

### `cancel` 取消压缩

```bash
caesium-cli cancel
```

### `settings` 读写压缩选项

```bash
# 读取全部选项
caesium-cli settings get

# 读取单个选项
caesium-cli settings get jpeg_quality

# 写入选项
caesium-cli settings set jpeg_quality 85
caesium-cli settings set format png
caesium-cli settings set output_folder "D:\out"
```

选项键与取值：

| 键 | 取值示例 |
|---|---|
| `format` | `same` `jpg` `png` `webp` `tiff` |
| `mode` | `quality` `size` |
| `lossless` `keep_metadata` `keep_structure` | `true` / `false` |
| `jpeg_quality` `png_quality` `webp_quality` | `0`-`100` |
| `png_optimization_level` | `1`-`6` |
| `jpeg_progressive` | `true` / `false` |
| `tiff_method` | `uncompressed` `lzw` `deflate` `packbits` |
| `tiff_deflate_level` | `1`-`3` |
| `max_output_size` `max_output_size_unit` | 数值 + `bytes`/`kb`/`mb`/`percentage` |
| `fit_to` | `none` `dimensions` `percentage` `short_edge` `long_edge` `fixed_width` `fixed_height` |
| `width` `height` `edge` | 数值（像素或百分比） |
| `keep_aspect_ratio` `do_not_enlarge` | `true` / `false` |
| `output_folder` `output_suffix` | 文本 |
| `same_folder_as_input` `skip_if_bigger` `cache_originals` | `true` / `false` |
| `delete_original_format_file` | `true` / `false` |
| `keep_dates` `keep_creation_date` `keep_last_modified_date` `keep_last_access_date` | `true` / `false` |

写入选项会同步更新设置界面和持久化配置。

### `path` 管理 PATH

```bash
caesium-cli path status     # 查看应用目录是否在用户 PATH 上
caesium-cli path add        # 添加到 PATH（缺失 caesium-cli.cmd 会自动生成）
caesium-cli path remove     # 从 PATH 移除
```

此命令**不需要软件运行**，直接读写注册表（Windows）。修改后重开终端生效。

### `show` / `quit`

```bash
caesium-cli show    # 把主窗口带到前台
caesium-cli quit    # 关闭软件（软件未运行时不自动启动）
```

### `shell` 交互式会话

```bash
caesium-cli shell
```

进入 REPL，可连续执行命令，支持 Tab 补全与历史：

```
Caesium CLI shell. Type 'help' for commands, 'exit' to quit.
caesium> add ./photos
caesium> settings set jpeg_quality 85
caesium> compress --wait
caesium> status
caesium> exit
```

### `help` / `--version`

```bash
caesium-cli help
caesium-cli --version
```

### `--raw` 发送原始命令

```bash
caesium-cli --raw '{"method":"import","params":{"paths":["C:/a.jpg"]}}'
caesium-cli --raw '{"method":"ping","params":{}}'
```

任意内部方法，供脚本 / AI 扩展使用。

---

## 全局选项

| 选项 | 说明 |
|---|---|
| `--json` | 机器可读的 JSON 输出 |
| `--no-launch` | 软件未运行时**不**自动启动，直接报错（退出码 2） |
| `--raw <json>` | 发送原始 JSON 命令 |

---

## 退出码

| 退出码 | 含义 |
|---|---|
| `0` | 成功 |
| `1` | 操作失败（例如：未设置输出目录、没有图片、压缩正在进行中、无法修改 PATH） |
| `2` | 软件未运行（且未自动启动，例如使用了 `--no-launch`） |
| `3` | 用法错误 / 未知命令 |

命令行可直接判断执行结果：

```bash
caesium-cli status
if %ERRORLEVEL% NEQ 0 ( echo 失败 )   # cmd
echo $LASTEXITCODE                     # PowerShell
```

---

## 机器可读输出（AI / 脚本）

加 `--json` 即可获得结构化结果，便于 AI 或脚本解析：

```bash
caesium-cli status --json
```

```json
{
    "ok": true,
    "result": {
        "compressing": false,
        "images": 2,
        "counts": {
            "compressed": 0,
            "error": 0,
            "uncompressed": 2,
            "compressing": 0,
            "warning": 0
        },
        "progress": {
            "current": 0,
            "total": 0
        },
        "items": [
            {
                "path": "C:/a.png",
                "name": "a.png",
                "status": "uncompressed",
                "original_size": 6438,
                "compressed_size": 6438,
                "ratio": 1.0,
                "saved": "0%",
                "compressed_path": ""
            }
        ]
    }
}
```

失败时输出：

```json
{
    "ok": false,
    "code": "NO_OUTPUT_FOLDER",
    "message": "Please select an output folder first."
}
```

常见错误码：`NOT_RUNNING`、`COMPRESS_IN_PROGRESS`、`NO_IMAGES`、`NO_OUTPUT_FOLDER`、`NO_IMAGES_SELECTED`、`NO_PATHS`、`NOT_COMPRESSING`、`UNKNOWN_KEY`、`UNKNOWN_METHOD`。

### 输出编码说明

- **CLI 的所有输出（包括 `--json`）默认使用系统的本地编码**（中文 Windows 为 GBK），因此在中文字符串路径的控制台里直接查看、包括 `--json`，都能正常显示中文，且不会切换代码页导致闪屏。
- **AI / 脚本需要严格 UTF-8 时**，在执行命令前设置环境变量 `CAESIUM_CLI_UTF8=1`，输出即改为 UTF-8：
  ```bash
  # cmd
  set CAESIUM_CLI_UTF8=1
  caesium-cli list --json

  # PowerShell
  $env:CAESIUM_CLI_UTF8 = "1"
  caesium-cli list --json

  # 单次调用（PowerShell）
  $env:CAESIUM_CLI_UTF8 = "1"; caesium-cli list --json; Remove-Item Env:CAESIUM_CLI_UTF8
  ```
- **`--json` / `--raw` 中文件名和路径不会被截断**，是完整内容（人类可读表格为了排版会截断文件名）。
- 确认数据完整性：`caesium-cli list --json > list.json` 后查看文件即可。

---

## 典型示例

```bash
# 整目录压缩为 JPG、质量 80、输出到 ./out，等待结果
caesium-cli compress ./photos -q 80 --format jpg -o ./out --wait

# 转换格式并限制大小（WebP，目标 < 500 KB）
caesium-cli compress ./assets --format webp --max-output-size 500 --max-output-size-unit kb --wait

# 批量缩放：所有图片缩放到最长边 1920
caesium-cli compress ./photos --fit long_edge --edge 1920 --wait

# 只压缩已失败的图片
caesium-cli compress --only-failed --wait

# 先添加，再微调选项，再压缩
caesium-cli add ./a.jpg ./b.png
caesium-cli settings set keep_metadata false
caesium-cli settings set output_suffix "_min"
caesium-cli compress --wait

# 脚本判断结果
caesium-cli compress ./x --wait --json > result.json

# 打开软件主窗口
caesium-cli show

# 彻底关闭软件
caesium-cli quit
```

---

## 常见问题

**问：提示 "Caesium Image Compressor is not running."？**
软件没在运行且使用了 `--no-launch`，或自动启动失败。去掉 `--no-launch` 即可让 CLI 自动启动软件。

**问：在 PowerShell 里直接调用 exe 时输出/等待异常？**
exe 是 GUI 子系统程序。请使用与 exe 同目录的 `caesium-cli.cmd`，或把应用目录加入 PATH 后使用 `caesium-cli`。

**问：`caesium-cli` 提示找不到命令？**
应用目录不在 PATH。运行 `caesium-cli path add`（或软件内 设置 → 命令行 (CLI) → 添加到 PATH），然后重开终端。

**问：压缩时收到 `COMPRESS_IN_PROGRESS`？**
压缩进行中不能再次添加/压缩/修改列表。先 `caesium-cli cancel` 或等它结束。

**问：设置界面的语言？**
设置对话框内的命令行 (CLI) 部分跟随软件语言（支持中文）。其他语言的翻译文件尚未收录这些新字符串时，会显示英文原文。
