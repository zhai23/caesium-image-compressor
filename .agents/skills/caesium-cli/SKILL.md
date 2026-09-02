---
name: caesium-cli
description: Use when the user asks to compress images, especially with the prompt "压缩图片", or requests Caesium Image Compressor CLI operations such as image import, compression, preview, restore, settings, status, or scripting.
---

# Caesium CLI

## Purpose

Use this skill to control the running Caesium Image Compressor instead of
editing its source code for normal image operations. The complete user-facing
reference is [docs/CLI.md](../../../docs/CLI.md). This file distills the
behavior that matters when an AI executes commands and records implementation
details verified in `src/ipc/CliRunner.cpp`, `src/ipc/CliClient.cpp`,
`src/ipc/CliServer.cpp`, `src/MainWindow.cpp`, and `caesium-cli.cmd`.

## Operating rules

1. Establish the executable before doing anything destructive. Prefer
   `caesium-cli` when it is on PATH. On Windows, prefer the adjacent
   `caesium-cli.cmd` wrapper over invoking the GUI executable directly.
2. Treat the GUI's list and settings as shared mutable state. Commands are
   sent to the running GUI through a per-user local named pipe, and changes are
   reflected in the GUI immediately.
3. Before compression, identify targets and output behavior explicitly. Use
   `list --json`, `settings get`, and `compress ... --dry-run` when there is any
   ambiguity.
4. Ask for confirmation before `clear`, `remove`, `restore`, `quit`, enabling
   deletion of original-format files, or writing into an input directory. Do
   not assume a compression command is reversible unless original caching was
   enabled before the compression.
5. For scripts or AI parsing, always use `--json` and set
   `CAESIUM_CLI_UTF8=1` on Windows when strict UTF-8 is required. Check both
   the process exit code and the JSON `ok` field.
6. Do not reuse numeric image IDs from an earlier application session. IDs are
   assigned by the GUI's in-memory session counter; call `list --json` again
   after restarting Caesium or changing the list.

## Finding and invoking the CLI

The CLI command is made available through a dedicated single-file bin folder
(`%LOCALAPPDATA%\CaesiumImageCompressor\bin`), which is what gets added to the
user PATH — not the whole application folder. The wrapper there reads the real
executable location from `app-path.txt` (refreshed on every app start), so the
entry keeps working and is de-duplicated even if the app is moved:

```powershell
caesium-cli path status
caesium-cli path add
# Open a new terminal after changing PATH.
```

If PATH is not configured, invoke the wrapper by its full path or from the
application directory:

```powershell
& "C:\Path\To\Caesium Image Compressor\caesium-cli.cmd" --version
```

The wrapper is important on Windows because the application is built as a GUI
subsystem executable. It captures and replays stdout/stderr and propagates the
exit code. Do not assume the repository's wrapper is runnable unless a built
`Caesium Image Compressor.exe` is beside it.

By default, a command starts Caesium if it is not running and waits up to about
15 seconds for the local IPC endpoint. Add `--no-launch` when starting the GUI
is not acceptable; a missing application then maps to exit code 2.

The executable's direct equivalent is:

```text
Caesium Image Compressor.exe --cli <command> [options]
```

Global options are `--json`, `--no-launch`, and `--raw <json>`. The CLI removes
these global flags before parsing the command, so they may be placed anywhere
that the shell passes through unchanged.

## Safe compression workflow

Use this sequence for an ordinary batch job:

```powershell
# 1. Check the current shared list and obtain current IDs.
caesium-cli status --json
caesium-cli list --json

# 2. Preview the exact targets and one-off options without writing files.
caesium-cli compress "C:\images" --format webp -q 80 -o "C:\output" --dry-run

# 3. Execute and wait for completion. The explicit path limits this job to it.
$env:CAESIUM_CLI_UTF8 = "1"
caesium-cli compress "C:\images" --format webp -q 80 -o "C:\output" --wait --json

# 4. Inspect final per-item states and output paths.
caesium-cli status --json
```

Use `--wait` for foreground automation. Without it, `compress` returns after
the job starts; poll `status --json` or use `status --wait`. `cancel` stops the
current job. A second import, compression, list mutation, or setting change
while compression is running is rejected with `COMPRESS_IN_PROGRESS`.

### Target selection

- A positive, pure integer such as `2` is an image ID.
- Any other positional value is a file or folder path.
- `compress` with IDs or paths processes only those matching current-list
  items. A path is scanned using the same supported-image and subfolder rules
  as import; it is imported first if needed.
- `compress` without targets uses selected items when multi-selection is
  enabled, otherwise it uses the whole current list.
- IDs and paths can be mixed: `compress 2 5 "C:\images\a.png" --wait`.
- A file whose name is only numeric must be written as a path such as
  `./5.png`; otherwise `5` is interpreted as ID 5.

For repeatable ID-based work:

```powershell
caesium-cli list
caesium-cli compress 2 5 7 --wait
```

For GUI checkbox workflows, enable multi-selection, clear existing checks,
select the desired IDs, and then compress without explicit targets. Direct
IDs are simpler when the job is only a one-off subset:

```powershell
caesium-cli settings set enable_multi_selection true
caesium-cli select none
caesium-cli select 2 5 7
caesium-cli compress --wait
```

### Output and options

Options supplied to `compress` are applied before starting the job. To change
persisted settings independently of a file operation, use:

```powershell
caesium-cli settings get
caesium-cli settings set jpeg_quality 85 format jpg
caesium-cli settings set fit_to long_edge edge 1920 do_not_enlarge true
```

Important option behavior:

- `-q` / `--quality` sets JPEG, PNG, and WebP quality together. Use the
  format-specific flags to set only one.
- `--format` accepts `jpg`, `png`, `webp`, `tiff`, or `same`.
- `--fit` accepts `none`, `dimensions`, `percentage`, `short_edge`,
  `long_edge`, `fixed_width`, or `fixed_height`; use `--width`, `--height`,
  or `--edge` as appropriate.
- `--same-folder-as-input` removes the need for an output folder. Otherwise
  compression fails with `NO_OUTPUT_FOLDER` if no output folder is configured.
- `--cache-originals` is required for later `restore`; it is not a substitute
  for preserving an already-overwritten original.
- `--skip-if-bigger` avoids replacing an output with a larger result.
- `--keep-structure` preserves the source directory structure under the
  output folder.
- `--only-failed` retries only failed items from the current list.

For the complete compression flags and setting keys, read `docs/CLI.md`
instead of inventing a key. `settings set` validates keys and values; invalid
entries are reported and produce a non-zero result rather than being silently
accepted. Multiple settings are accepted as key/value pairs. In PowerShell,
an empty trailing value can be swallowed by the `.cmd` wrapper; the CLI
intentionally interprets a trailing key without a value as an empty string, so
`settings set output_suffix ""` clears a text setting.

## Read-only inspection and recovery

Use these commands for diagnosis:

```powershell
caesium-cli list --json
caesium-cli status --json
caesium-cli settings get --json
caesium-cli preview 3 --json
```

`preview` performs a real temporary compression using the current options and
reports original/preview format, resolution, sizes, saved bytes, percentage,
and a temporary preview path. It is different from `compress --dry-run`, which
only reports intended targets and options and does not compress anything.

`status --json` returns `compressing`, progress, list-wide totals, selected
counts, and per-item objects. Per-item objects include `id`, `path`, `name`,
`format`, `status`, `original_size`, `compressed_size`, `ratio`, `saved`, and
`compressed_path`. Human-readable output may translate or truncate values;
JSON does not.

Use `restore [ID/path...]` only after verifying that cached originals exist.
Without explicit targets it follows the same selected/all rule as compression.
`show` brings the window forward. `quit` closes it and should not be used merely
to finish an automation job.

## Automation contract

Successful JSON has this shape:

```json
{"ok": true, "result": {}}
```

Failed JSON has this shape:

```json
{"ok": false, "code": "NO_OUTPUT_FOLDER", "message": "..."}
```

Common error codes include `NOT_RUNNING`, `COMPRESS_IN_PROGRESS`, `NO_IMAGES`,
`NO_OUTPUT_FOLDER`, `NO_IMAGES_SELECTED`, `NO_MATCH`, `NO_PATHS`, `NO_FILES`,
`NOT_COMPRESSING`, `UNKNOWN_KEY`, and `UNKNOWN_METHOD`.

Exit codes are:

- `0`: success
- `1`: operation failure, including invalid settings, missing output, or a
  failed PATH update
- `2`: application not running when auto-launch is disabled
- `3`: invalid usage or unknown command

On Chinese Windows, all output defaults to the console's local encoding,
including JSON. Set `CAESIUM_CLI_UTF8=1` before parsing output. Keep stdout and
stderr separate when possible: progress, auto-launch notices, and errors can
be written to stderr while structured results are written to stdout.

## Commands at a glance

Use the following command families rather than sending ad-hoc GUI keystrokes:

- `add <path...>` imports supported files or folders and skips duplicates.
- `remove <ID/path...>` removes exact current-list matches; `clear` removes all.
- `list` shows the current list and IDs.
- `select` / `deselect` changes selection and focuses the corresponding GUI item.
- `compress`, `cancel`, and `status` control and monitor compression.
- `preview <ID/path>` estimates a single image using real compression logic.
- `settings get/set` reads or persists compression and preference settings.
- `restore` restores cached originals.
- `path status/add/remove` manages the Windows user PATH without requiring the
  application to be running.
- `show`, `quit`, and Windows `shell` control the application session.

## Raw protocol use

Use `--raw` only when the normal command does not expose the needed operation,
and prefer a documented method. The server receives a JSON object containing
`method` and `params`; supported handlers currently include `ping`, `import`,
`remove`, `clear`, `list`, `status`, `compress`, `cancel`, `select`,
`restore`, `preview`, `options.get`, `options.set`, `settings.get`,
`settings.set`, `show`, and `quit`.

Examples:

```powershell
caesium-cli --raw '{"method":"ping","params":{}}' --json
caesium-cli --raw '{"method":"import","params":{"paths":["C:/images/a.png"]}}' --json
```

The implementation uses a per-user named pipe, a four-byte big-endian frame
length, and compact JSON request/response payloads. This is an implementation
detail for protocol debugging, not a reason to bypass the CLI wrapper. On
Windows the runner also recovers quoted raw JSON from the original command
line because `%*` expansion in the `.cmd` wrapper can remove embedded quotes.

## Troubleshooting checklist

1. If the command is not found, run `caesium-cli path status`; run
   `caesium-cli path add` (adds the dedicated bin folder and cleans up stale
   entries) and open a new terminal.
2. If direct `.exe` invocation has no output or does not wait, use the adjacent
   `caesium-cli.cmd` wrapper.
3. If `NOT_RUNNING` occurs, remove `--no-launch` or start Caesium manually.
4. If `COMPRESS_IN_PROGRESS` occurs, poll `status --json` or ask whether to
   run `cancel`; do not issue more mutations concurrently.
5. If `NO_MATCH` occurs, run `list --json` and use current IDs or exact paths.
6. If a numeric filename is mistaken for an ID, prefix it with `./` or use an
   absolute path.
7. If JSON contains garbled Chinese text, set `CAESIUM_CLI_UTF8=1` and rerun;
   do not try to parse the human-readable table.
