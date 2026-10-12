# 托盘运行

在“设置 → 通用”中开启“关闭窗口时缩小到托盘”，点击窗口关闭按钮会隐藏到托盘，保留当前列表、预览缓存和正在执行的任务。该设置默认关闭，修改后立即生效。

开启“退出前询问”时，退出确认框包含“是”“缩小到托盘”“取消”。选择托盘按钮只影响这一次操作，不修改默认设置。启用关闭到托盘后，窗口关闭按钮直接缩小，不再弹出确认框；通过菜单中的“退出”仍可真正退出。

点击或双击托盘图标、选择托盘菜单中的“显示”，或再次启动程序，都可以恢复窗口。托盘菜单中的“退出”和 `caesium-cli quit` 会真正退出，CLI 退出保持不弹确认框的行为。系统托盘不可用时，关闭窗口按原退出流程处理，确认框中的托盘按钮不可用。

CLI 设置示例：

```text
caesium-cli settings set close_to_tray true
caesium-cli settings set close_to_tray false
```

## 验证

可选 GUI 集成测试覆盖设置持久化、关闭到托盘、确认框按钮、恢复最大化窗口、取消退出后再次关闭、CLI 退出，以及无托盘环境的回退行为：

```text
cmake -S . -B build_dir -DCAESIUM_GUI_TESTS=ON
cmake --build build_dir --target TrayTests
ctest --test-dir build_dir -R TrayTests --output-on-failure
```

在 Windows 桌面会话下运行以验证真实托盘。另设 `QT_QPA_PLATFORM=offscreen` 运行同一测试，可验证系统托盘不可用时不会隐藏窗口。
