# Caesium Image Compressor - 命令行使用文档

Caesium 启动后会监听一个本机命名管道，命令行（CLI）可以直接操作正在运行的软件：
添加/移除图片、设置压缩选项、启动/取消压缩、查询进度与结果。所有操作都会**实时同步到软件界面**。

CLI 的设计目标是：
- **改设置不需要给文件路径**——选项单独调，压缩时再指定图片；
- **压缩时用 ID、路径或当前选中**都可以，和 `select` 的规则统一；
- **改错了会明确报错**，不会静默忽略。

## 目录

- [快速开始](#快速开始)
- [命令速查表](#命令速查表)
- [安装与配置](#安装与配置)
- [基础概念](#基础概念)
  - [三种方式指定图片](#三种方式指定图片)
  - [位置参数规则](#位置参数规则)
  - [全局选项](#全局选项)
  - [退出码](#退出码)
- [命令参考](#命令参考)
  - [列表管理](#列表管理)
    - [`add`](#add-添加文件或文件夹)
    - [`remove`](#remove-移除列表项)
    - [`clear`](#clear-清空列表)
    - [`list`](#list-查看列表)
  - [选择与定位](#选择与定位)
    - [`select` / `deselect`](#select--deselect-多选--定位)
  - [压缩控制](#压缩控制)
    - [`compress`](#compress-压缩)
    - [`cancel`](#cancel-取消压缩)
    - [`status`](#status-查询状态)
  - [预览](#预览)
    - [`preview`](#preview-预览单图压缩体积)
  - [设置](#设置)
    - [`settings`](#settings-读写设置)
  - [还原](#还原)
    - [`restore`](#restore-还原图片)
  - [其他](#其他)
    - [`path`](#path-管理-path)
    - [`show` / `quit`](#show--quit)
    - [`shell`](#shell-交互式会话)
    - [帮助与版本](#帮助与版本)
    - [`--raw`](#raw-发送原始命令)
- [压缩选项参考](#压缩选项参考)
- [设置键参考](#设置键参考)
- [机器可读输出（AI / 脚本）](#机器可读输出ai--脚本)
- [典型示例](#典型示例)
- [常见问题](#常见问题)

---

## 快速开始

```bash
# 一条龙：软件没开也没关系，CLI 会自动启动它，等就绪后执行命令
caesium-cli compress ./photos -q 80 -o ./out --wait
```

分步操作（改设置不需要给路径）：

```bash
caesium-cli add ./photos               # 1. 添加图片
caesium-cli settings set fit_to long_edge edge 1920   # 2. 单独调整选项
caesium-cli compress --wait            # 3. 用当前选项压缩
caesium-cli status                     # 4. 查看结果
caesium-cli quit                       # 5. 关闭软件
```

> 默认情况下，若软件未运行，CLI 会**自动启动**它并等待就绪后执行命令。用 `--no-launch` 可关闭该行为。

---

## 命令速查表

| 命令 | 作用 | 常用示例 |
|---|---|---|
| `add <路径...>` | 添加文件/文件夹到列表 | `add ./photos` |
| `remove <ID/路径...>` | 移除列表项 | `remove 3 5` |
| `clear` | 清空列表 | `clear` |
| `list` | 查看列表（含 ID） | `list --json` |
| `select <ID/路径...>` | 多选 / 高亮定位 | `select 2 5` |
| `deselect <ID/路径...>` | 取消选中 | `deselect 2` |
| `compress [ID/路径...]` | 压缩图片 | `compress 2 5 --wait` |
| `cancel` | 取消压缩 | `cancel` |
| `status` | 状态 / 进度 | `status --wait` |
| `preview <ID/路径>` | 预览压缩后体积 | `preview 3` |
| `restore [ID/路径...]` | 还原原始文件 | `restore 2 5` |
| `settings get/set` | 读写设置 | `settings set jpeg_quality 85` |
| `path add/remove/status` | 管理 PATH | `path add` |
| `show` / `quit` | 显示窗口 / 退出 | `quit` |
| `shell` | 交互式会话 | `shell` |
| `help` | 显示帮助 | `help` |

---

## 安装与配置

CLI 就是可执行程序本身，区别只是启动参数。能调用它的前提是命令行能找到它。

### 方式一：安装版（推荐）

安装器（setup.iss）会自动：
1. 安装 `caesium-cli.cmd` 到应用目录（供在应用目录内使用 `.\caesium-cli`）；
2. 在 `%LOCALAPPDATA%\CaesiumImageCompressor\bin` 写入单文件 wrapper 和 `app-path.txt` 指针；
3. 把**这个专用 bin 文件夹**写入**用户 PATH**（不会把整个应用目录加进去）。

装完重开一个终端，即可直接使用 `caesium-cli`。卸载时安装器会自动删除该 bin 文件夹并清理 PATH 条目。

### 方式二：绿色版 / 开发目录

推荐使用软件设置：**设置 → 命令行 (CLI) → 添加到 PATH**（也可用 `caesium-cli path add`）。

软件**不会把整个应用目录加入 PATH**，而是在
`%LOCALAPPDATA%\CaesiumImageCompressor\bin` 里只放一个 `caesium-cli.cmd` 单文件，
然后把**这个专用 bin 文件夹**写入用户 PATH。该 wrapper 通过 `app-path.txt` 指针文件
定位当前 exe 位置，每次启动软件都会刷新它，所以软件移动到新目录后 PATH 条目不重复、
不残留旧位置；再次点击"添加到 PATH"还会自动清理旧位置残留的条目。

也可以手动加入用户 PATH：

```powershell
[Environment]::SetEnvironmentVariable("Path", $env:Path + ";<bin 文件夹>", "User")
```

改完 PATH 后**重开终端**生效。

### 方式三：不改 PATH

- 在 exe 所在目录内使用：`.\caesium-cli status`
- 或输入完整路径：`"<路径>\Caesium Image Compressor.exe" --cli status`

### 关于 `caesium-cli.cmd`

Windows 上 exe 是 GUI 子系统程序，直接在 PowerShell 中调用可能不会被等待。`caesium-cli.cmd`
解决了这个问题，会自动等待命令结束并透传退出码。

- 绿色版 / 开发目录：把 `caesium-cli.cmd` 放在 exe 同目录，运行 `.\caesium-cli <命令> ...`；
- 使用"添加到 PATH"后：软件在专用 bin 文件夹里生成的 `caesium-cli.cmd` 会自动读取
  `app-path.txt` 找到当前 exe，因此软件移动到新位置后无需重新添加到 PATH。

- Windows（cmd / PowerShell）：`caesium-cli <命令> ...`
- Linux / macOS：直接使用 `caesium-image-compressor --cli <命令> ...`（原生阻塞，无需包装脚本）

---

## 基础概念

### 三种方式指定图片

`compress`、`select`、`remove`、`restore` 都支持用下面三种方式指定图片：

| 方式 | 写法 | 说明 |
|---|---|---|
| **按 ID** | `compress 2 5 7` | 位置参数是**纯数字**时视为图片 ID；ID 用 `list` 查看 |
| **按路径** | `compress ./photos` | 文件或文件夹（文件夹自动递归扫描支持的图片） |
| **当前选中** | `compress` | 不写任何图片参数；多选开启 = 压缩勾选的，否则 = 全部 |

三种方式可以混用，例如 `compress 2 ./photos`。

> 判断规则：**能解析成 `>0` 的整数就是 ID，否则是路径**。与 `select`/`remove`/`restore` 完全一致，容易记忆。

### 位置参数规则

```
caesium-cli <命令> [位置参数...] [选项...]
```

- **纯数字**（如 `2`、`5`）→ 图片 ID；
- **其他**（`./photos`、`C:\a.jpg`、`D:\图\b.png`）→ 路径；
- 全局选项（`--json`、`--no-launch` 等）可放在任意位置。

> 边界：如果某张图的文件名恰好是纯数字（如 `5.png`），直接写 `5` 会被当作 ID。这时写 `./5.png` 即可。

### 全局选项

| 选项 | 说明 |
|---|---|
| `--json` | 机器可读的 JSON 输出 |
| `--no-launch` | 软件未运行时**不**自动启动，直接报错（退出码 2） |
| `--raw <json>` | 发送原始 JSON 命令 |

### 退出码

| 退出码 | 含义 |
|---|---|
| `0` | 成功 |
| `1` | 操作失败（例如：未设置输出目录、没有图片、压缩正在进行中、无法修改 PATH、`settings set` 有无效键） |
| `2` | 软件未运行（且未自动启动，例如使用了 `--no-launch`） |
| `3` | 用法错误 / 未知命令 |

命令行可直接判断执行结果：

```bash
caesium-cli status
if %ERRORLEVEL% NEQ 0 ( echo 失败 )   # cmd
echo $LASTEXITCODE                     # PowerShell
```

---

## 命令参考

### 列表管理

#### `add` 添加文件或文件夹

```bash
caesium-cli add <路径...>
```

把文件 / 文件夹中的图片加入列表，返回导入数量。已存在的图片会自动跳过（不重复添加）。

```
已导入 42 张图片。
```

#### `remove` 移除列表项

```bash
caesium-cli remove <ID 或路径...>
```

按 **ID** 或完整路径精确匹配移除，可以混用：

```bash
caesium-cli remove 2                  # 按 ID 移除
caesium-cli remove "C:\a.jpg" 3       # 路径和 ID 混用
```

#### `clear` 清空列表

```bash
caesium-cli clear
```

#### `list` 查看列表

```bash
caesium-cli list
caesium-cli list --json
```

人类可读表格（**ID** 列是会话内的稳定编号，用于 `select`/`deselect`/`remove`/`compress`；文件名过长以 `...` 结尾；`已选` 列显示多选状态）：

```
  ID  名称                            格式    已选  状态        原始大小     节省
----------------------------------------------------------------------------
   1  a1.png                           PNG     是    等待            6.29 KB        0%
   2  spine_xinshouyindao_shou图集(256...  PNG     否    等待           20.27 KB        0%
```

`list --json` 返回每张图的 `id` / `path` / `name` / `format` / `status` / `original_size` 等完整字段（不截断）。

### 选择与定位

#### `select` / `deselect` 多选 / 定位

- **开启多选**（`settings set enable_multi_selection true`）：切换复选框状态，压缩只处理选中的图片；同时会在 GUI 中**高亮定位**该图。
- **未开启多选**：不切换复选框（此时始终处理全部），只在 GUI 中**高亮定位**该图并显示其预览。

```bash
# 全选 / 取消全选（仅开启多选时有意义）
caesium-cli select all
caesium-cli select none          # 等价于 deselect all

# 选中 / 取消选中指定图片（按 ID 或路径；会高亮定位到 GUI）
caesium-cli select 3 5 7
caesium-cli deselect 3 5
caesium-cli select "C:\a.jpg"

# 只看某张图 / 让 GUI 定位并预览它
caesium-cli select 4
```

查看选中情况用 `caesium-cli status`。

**典型流程：只压缩指定的几张图片**

```bash
caesium-cli settings set enable_multi_selection true   # 开启多选
caesium-cli clear
caesium-cli add ./photos
caesium-cli select none                                # 先全部取消
caesium-cli list                                       # 记下要保留图片的 ID
caesium-cli select 2 5                                 # 只勾选要压缩的
caesium-cli compress --wait
```

> 提示：如果只是想压缩指定图片，其实用 `caesium-cli compress 2 5 --wait` 更简单，不需要多选这套流程。

### 压缩控制

#### `compress` 压缩

`compress` 可以用三种方式指定要压缩的图片：**ID**、**路径**、或**当前选中**（见[基础概念](#三种方式指定图片)）。

```bash
# 按 ID 压缩：只压缩列表里 ID 为 2、5、7 的图片（纯数字 = ID）
caesium-cli list                      # 先查看每个图片的 ID
caesium-cli compress 2 5 7 --wait

# 一条龙：添加路径 + 设置选项 + 开始压缩（只压缩这些路径，列表里其他图片不动）
caesium-cli compress ./photos -q 80 --format jpg -o ./out --wait

# 用当前界面上的选项压缩现有列表（多选开启 = 压缩勾选的，否则 = 全部）
caesium-cli compress --wait

# 仅重试失败的项
caesium-cli compress --only-failed --wait

# 只预览将要执行的操作，不实际压缩
caesium-cli compress ./photos -q 80 --dry-run
caesium-cli compress 2 5 7 --dry-run

# 不等待，立即返回（进度用 status 查询）
caesium-cli compress
```

`--wait` 会阻塞直到压缩完成，显示进度并打印汇总（**只统计本次实际压缩的项**，按 ID/路径压缩子集时不会把列表里没处理的图算进去）：

```
压缩完成。
  图片数量：  42
  原始大小：  120.5 MB
  压缩大小：  48.2 MB
  节省：      72.3 MB (60.0%)
```

`--dry-run` 只列出"要处理哪些路径 / ID、用什么选项"，不执行，适合先确认再动手。

> 说明：传了路径或 ID 时，**只压缩指定的这些**；不传时压缩当前选中。完整选项见[压缩选项参考](#压缩选项参考)。

#### `cancel` 取消压缩

```bash
caesium-cli cancel
```

#### `status` 查询状态

```bash
caesium-cli status
caesium-cli status --wait     # 持续刷新直到压缩结束
caesium-cli status --json
```

显示是否在压缩、进度、各项状态（等待 / 压缩中 / 已压缩 / 错误）、**列表总原始大小与总压缩后大小**（同 GUI 左下角）与汇总。开启多选时会额外显示 `已选：N / M`。

### 预览

#### `preview` 预览单图压缩体积

用**当前压缩选项**对某张图生成一次真实临时预览，返回压缩后的体积与节省比例（数值和真实压缩一致，因为走同一套预览代码）：

```bash
caesium-cli preview 3                 # 按 ID 预览
caesium-cli preview "C:\a.jpg"        # 按路径预览
caesium-cli preview 3 --json          # 机器可读
```

输出示例（含格式与尺寸，与 GUI 预览一致）：

```
原始：PNG 1024x768，1.20 MB
压缩后：JPG 800x600，480.5 KB（节省 736.5 KB / 60.0%）
```

`preview --json` 返回 `original_format` / `original_resolution` / `preview_format` / `preview_resolution` / `original_size` / `preview_size` / `saved` / `saved_percent` / `preview_path` 等字段，AI 可直接用。

> 与 `compress --dry-run` 的区别：`--dry-run` 只列出"要处理哪些路径、用什么选项"，不执行；`preview` 会实际生成一次预览，告诉你"压完多大"。

### 设置

#### `settings` 读写设置

设置分为两类：**压缩选项**（决定压缩行为）和**偏好设置**（软件行为，如多选、主题、语言）。

```bash
# 读取全部设置
caesium-cli settings get

# 读取单个设置
caesium-cli settings get jpeg_quality
caesium-cli settings get enable_multi_selection

# 写入设置（可一次设置多个，键值成对出现）
caesium-cli settings set jpeg_quality 85
caesium-cli settings set format png
caesium-cli settings set output_folder "D:\out"
caesium-cli settings set enable_multi_selection true
caesium-cli settings set fit_to long_edge edge 1920

# 清空文本类设置（把值设为空字符串）
caesium-cli settings set output_suffix ""
caesium-cli settings set output_folder ""
```

> 空字符串值：在 PowerShell 里 `settings set output_suffix ""` 中的 `""` 会被 shell 吞掉，
> 软件会把「键后没有值」的情况自动当作空字符串处理，因此也能清空文本类设置。
> 清空后会回显 `选项 'output_suffix' 已设置为空。`。

`settings set` 调整的是**单个选项本身，不需要提供文件路径**。改完后压缩时再指定图片即可：

```bash
# 1. 单独调整压缩选项（不涉及任何文件路径）
caesium-cli settings set fit_to long_edge
caesium-cli settings set edge 1920
caesium-cli settings set do_not_enlarge true

# 2. 压缩时只需给图片，选项直接用上面设置好的
caesium-cli compress ./photos --wait
```

> 校验：`settings set` 会校验键和值。键不存在或值非法（如 `fit_to xyz`、`width abc`）时，会打印警告并返回**非零退出码**，不会静默忽略。设置成功后会把生效值回显出来（如 `选项 'fit_to' 已设置（long_edge）。`）。

键的完整列表见[设置键参考](#设置键参考)。

### 还原

#### `restore` 还原图片

把已压缩（或已转换格式）的图片恢复为原始文件。**需要压缩时开启了"缓存原始文件"（`settings set cache_originals true`）才能还原**，否则会提示没有可还原的图片。还原后会在 GUI 中**高亮定位**该图。

```bash
caesium-cli restore                    # 还原所有选中的图片（未开多选时 = 全部）
caesium-cli restore 2 5                # 按 ID 还原指定图片
caesium-cli restore "C:\a.jpg"         # 按路径还原
```

输出示例：

```
已还原 3 张图片。
```

`restore --json` 返回 `restored` / `failed` 计数。

### 其他

#### `path` 管理 PATH

```bash
caesium-cli path status     # 查看 caesium-cli 命令是否在用户 PATH 上
caesium-cli path add        # 添加专用 bin 文件夹到 PATH（缺失 wrapper 自动生成，并清理旧位置残留条目）
caesium-cli path remove     # 从 PATH 移除 caesium-cli 相关条目
```

此命令**不需要软件运行**，直接读写注册表（Windows）。修改后重开终端生效。

#### `show` / `quit`

```bash
caesium-cli show    # 把主窗口带到前台
caesium-cli quit    # 关闭软件（软件未运行时不自动启动）
```

#### `shell` 交互式会话

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

#### 帮助与版本

```bash
caesium-cli help          # 显示命令帮助
caesium-cli --version     # 显示版本号
```

#### `--raw` 发送原始命令

```bash
caesium-cli --raw '{"method":"import","params":{"paths":["C:/a.jpg"]}}'
caesium-cli --raw '{"method":"ping","params":{}}'
```

任意内部方法，供脚本 / AI 扩展使用。

> 引号安全：JSON 里带引号的值（如 `"output_suffix":""`）在 Windows 上经 `.cmd` 包装
> 转发时可能被 shell 拆掉引号，软件会自动从原始命令行恢复完整 JSON，因此
> PowerShell / cmd 下都能直接写带引号的 JSON，无需额外转义。

---

## 压缩选项参考

这些是 `compress` 命令的**命令行选项**（带 `--` 前缀），等价于 `settings set` 里对应的键（去掉 `--`、`-` 换成 `_`，例如 `--fit` ↔ `fit_to`）。**不带任何压缩选项时，用当前界面/设置里保存的选项。**

| 选项 | 说明 |
|---|---|
| `-q, --quality <0-100>` | 同时设置 JPEG/PNG/WebP 质量 |
| `--jpeg-quality` `--png-quality` `--webp-quality` | 单独设置某种格式质量 |
| `--format <jpg\|png\|webp\|tiff\|same>` | 输出格式（`same` = 保持原格式） |
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

> 关于 `--fit` 缩放方式：`none` 保持原始尺寸；`dimensions` 指定像素宽高；`percentage` 按百分比（`--width`/`--height` 传的是百分比数字，如 `50` = 50%）；`short_edge` / `long_edge` 按短边/长边（用 `--edge`）；`fixed_width` / `fixed_height` 只限制一边，另一边按比例自动。

---

## 设置键参考

`settings set <键> <值>` 和 `settings get [键]` 使用的键名。**压缩选项键**（对应压缩选项参考里的 `--` 选项，去掉横线前缀）；**偏好设置键**（软件行为）。

### 压缩选项键

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

### 偏好设置键

| 键 | 取值示例 |
|---|---|
| `enable_multi_selection` | `true` / `false`（多选，立即生效） |
| `import_subfolders` | `true` / `false` |
| `skip_compression_dialogs` | `true` / `false` |
| `prompt_before_exit` | `true` / `false` |
| `send_usage_reports` | `true` / `false` |
| `check_updates_at_startup` | `true` / `false` |
| `multithreading` | `true` / `false` |
| `multithreading_max_threads` | 数值 |
| `theme` | `0`（原生）/ `1`（Fusion） |
| `theme_variant` | `0` 自动 / `1` 浅色 / `2` 深色 |
| `language` | `default` `zh_CN` `en_US` 等 |
| `args_behaviour` | `0` 仅导入 / `1` 导入并压缩 |
| `post_compression_action` | `0` 无 / `1` 关闭 / `2` 休眠 / `3` 关机 / `4` 打开输出目录 |
| `transparency_fill_color` | 十六进制颜色（如 `#FFFFFF`） |

写入压缩选项会同步更新设置界面并持久化；偏好设置写入后即时生效或下次启动生效（主题/语言等）。

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
                "id": 1,
                "path": "C:/a.png",
                "name": "a.png",
                "format": "png",
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

常见错误码：`NOT_RUNNING`、`COMPRESS_IN_PROGRESS`、`NO_IMAGES`、`NO_OUTPUT_FOLDER`、`NO_IMAGES_SELECTED`、`NO_MATCH`（给定的 ID/路径不在列表中）、`NO_PATHS`、`NOT_COMPRESSING`、`UNKNOWN_KEY`、`UNKNOWN_METHOD`。

### 输出编码说明

- **CLI 的人类可读输出为中文**（状态提示、列表、帮助等），`--json` 的字段名、命令名、选项键保持英文不变（机器契约）。
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

# 先单独调整好选项，再只给路径压缩（调整选项时不需要给路径）
caesium-cli settings set fit_to long_edge edge 1920 keep_aspect_ratio true
caesium-cli compress ./photos --wait

# 只压缩列表中 ID 为 2、5、7 的图片
caesium-cli list
caesium-cli compress 2 5 7 --wait

# 只压缩已失败的图片
caesium-cli compress --only-failed --wait

# 压缩当前选中的图片（多选开启时 = 勾选的）
caesium-cli select 2 5 7
caesium-cli compress --wait

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
exe 是 GUI 子系统程序。请使用与 exe 同目录的 `caesium-cli.cmd`，或把专用 bin 文件夹加入 PATH 后使用 `caesium-cli`。

**问：`caesium-cli` 提示找不到命令？**
caesium-cli 不在 PATH（或其指向的 exe 不存在）。运行 `caesium-cli path add`（或软件内 设置 → 命令行 (CLI) → 添加到 PATH），然后重开终端。

**问：压缩时收到 `COMPRESS_IN_PROGRESS`？**
压缩进行中不能再次添加/压缩/修改列表。先 `caesium-cli cancel` 或等它结束。

**问：`compress 5` 报错，但我明明有叫 `5.png` 的文件？**
`5` 是纯数字，被当成 ID 而不是路径。写 `./5.png` 即可。

**问：`settings set fit_to xyz` 提示无效但不影响后面的命令？**
`settings set` 遇到无效键/值会回显警告并返回非零退出码（可被脚本捕获），其余有效键仍会正常生效。

**问：设置界面的语言？**
设置对话框内的命令行 (CLI) 部分跟随软件语言（支持中文）。其他语言的翻译文件尚未收录这些新字符串时，会显示英文原文。
