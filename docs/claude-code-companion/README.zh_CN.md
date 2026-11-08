<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# Claude Code Companion（Claude Code 状态伴侣）

一个面向 Claude Code CLI 的桌面/随身状态显示器。AI Passport 会显示 Mac 上正在运行的每个 Claude Code 会话：主代理在做什么、各个子代理的进展、哪个会话在等待权限确认或回答问题、哪个会话刚刚完成或出错。上/下键切换会话，确定键查看详情。

设备只负责显示和提醒。工具授权和问题回答仍在终端中完成，设备无法在 Mac 上执行任何操作。

## 工作原理

```text
Claude Code 会话 --hooks--> hook.py --HTTP 127.0.0.1--> bridge.py --BLE--> AI Passport
  （任意终端）        （异步，从不阻塞）            （汇总各会话）        （界面、提示音）
```

1. **状态来源：Claude Code hooks。** 每个会话都会在生命周期节点触发 hook（`SessionStart`、`UserPromptSubmit`、`PreToolUse`、`PermissionRequest`、`PostToolUse`、`SubagentStart`、`SubagentStop`、`Notification`、`Stop`、`StopFailure`、`SessionEnd` 等）。每个 hook 都会收到包含 `session_id`、`cwd` 和事件详情的 JSON。本应用为每个事件安装一个异步 command hook，Claude 不会等待它（只有 `SessionEnd` 是同步的，并有 0.5 秒超时上限）。
2. **`tools/cc_companion/hook.py`** 精简 JSON（丢弃工具输出、截断字符串、附加 hook 启动时间），然后发送给本机 bridge。即使 bridge 未运行，它也总是以 0 退出。
3. **`tools/cc_companion/bridge.py`** 为每个会话维护一条记录并排序（需要你处理的排在最前），通过 BLE 向设备发送完整快照，并对权限请求、问题、完成和错误发送一次性提醒。
4. **固件**保存最新快照、渲染界面并播放提示音。

线路协议见 [protocol.zh_CN.md](protocol.zh_CN.md)。

### 为什么选择蓝牙 LE

ESP32-C3 支持 2.4 GHz Wi-Fi 和蓝牙 LE（不支持经典蓝牙）。BLE 无需在设备上保存 Wi-Fi 凭据，在任何网络（酒店、旅行路由器、手机热点）下都能使用，功耗也更低。链路要求 LE Secure Connections 配对，并在设备上显示六位配对码，因此通信经过加密和认证；一台 Mac 绑定后，其他电脑只有在设备上打开配对窗口后才能配对。

## 安装

要求：带蓝牙的 macOS、Python 3.9 或更高版本，以及用于运行 bridge 唯一依赖（`bleak`）的 [uv](https://docs.astral.sh/uv/)。

1. **烧录固件**（见[构建与烧录](#构建与烧录)）并开机。设备显示 *Waiting for your Mac* 和设备名 `CC-Companion-XXXX`。
2. **在终端中启动 bridge** 并保持运行：

   ```bash
   uv run --no-project --with bleak==3.1.0 python tools/cc_companion/bridge.py
   ```

   首次运行时，macOS 会请求终端应用的蓝牙权限，随后弹出配对对话框。输入设备上显示的六位数字。`--scan` 列出附近设备；`--device CC-Companion-XXXX` 指定设备。
3. **安装 hooks**（用户级设置，作用于所有项目）。先预览：

   ```bash
   python3 tools/cc_companion/install_hooks.py --dry-run
   python3 tools/cc_companion/install_hooks.py
   ```

   安装脚本会备份 `~/.claude/settings.json`，为每个事件添加一个异步 hook（`SessionEnd` 同步运行，最多 0.5 秒，确保关闭的会话从设备上消失），不改动你的其他 hook。之后可用 `--uninstall` 移除。修改后新启动的会话才会生效。
4. 在任意目录启动 `claude`，该会话就会出现在设备上。

不运行 Claude 也可以体验各个界面：启动 bridge 后回放一组脚本化会话（权限请求、问题、中文文本、子代理、错误）：

```bash
python3 tools/cc_companion/simulate.py --speed 2
```

`GET http://127.0.0.1:47820/status` 可查看 bridge 正在跟踪的内容；请求时需带上令牌：`Authorization: Bearer $(cat ~/.config/cc-companion/token)`。`--dry-run` 会打印协议行而不使用 BLE。

## 安全

| 路径 | 保护措施 | 剩余风险 |
| --- | --- | --- |
| Mac 与设备之间的无线链路 | 仅允许 LE Secure Connections，设备显示配对码（防中间人），绑定，AES-CCM 链路加密。RX、TX 及 TX 订阅都要求已认证链路。第一台 Mac 绑定后，除非打开 *Settings > Pair new Mac*，否则拒绝新的配对。 | 对看不到配对码的附近攻击者，暂无已知风险。 |
| Mac 上的其他应用 | 绑定属于 macOS 而非 bridge，因此任何获得蓝牙权限的应用都能使用这条已绑定链路（与 Android 文档对 BLE 的提醒相同）。CoreBluetooth 不允许一个应用观察另一个应用的写入，设备也不提供可读取的会话数据；设备的通知只包含固件版本、电量、ACK 和错误。 | 这类应用可以写入伪造的协议行，显示误导性的状态或提醒。设备无法批准或执行任何操作。 |
| hooks 到 bridge（本机 HTTP） | `POST /event` 与 `GET /status` 都要求 `Authorization: Bearer <令牌>`。bridge 首次启动时在 `~/.config/cc-companion/token` 创建令牌（目录 0700、文件 0600），并以常量时间比较；`hook.py` 和 `simulate.py` 读取该令牌。 | 以你的用户身份运行的进程可以读取令牌，正如它们也能读取 `~/.claude` 中的会话记录。 |
| 设备存储 | 绑定与设置保存在 NVS 中。会话文本只存在于 RAM，重启后清除。 | 未启用 Flash 加密；拿到设备的人可以从 Flash 读出绑定密钥。 |

BLE 之上没有应用层加密或认证。如果需要防范本机其他应用伪造数据，下一步可以由设备生成一个密钥、在 bridge 中输入一次，并为每条消息附加认证标签。

## 界面与按键

| 界面 | 内容 | 上 / 下 | 确定短按 | 确定长按 |
| --- | --- | --- | --- | --- |
| 主页 | 当前会话卡片（状态、项目、活动、子代理圆点）及最多三个其他会话 | 上一个 / 下一个会话（循环） | 打开详情 | 设置 |
| 详情 | 待处理的权限或问题、主代理活动、最近的提示或回复、子代理列表 | 滚动 | 返回 | 返回 |
| 提醒 | 会话需要授权或回答时的全屏卡片，并播放提示音 | 上：打开详情；下：下一个等待中的会话 | 关闭 | 关闭 |
| 设置 | 声音、亮度、息屏时间、配对新 Mac、忘记已配对 Mac、关于 | 选择 | 修改 | 返回 |
| 等待 / 配对 | 连接状态、设备名、六位配对码 | — | — | 设置 |

使用按键时，主页卡片保持你用上/下键选择的会话；最后一次按键 30 秒后，卡片会回到最重要的会话（等待你处理的优先，其次是错误、工作中、已完成）。每种会话状态对应一种颜色：珊瑚色 **WORKING**（带动画星芒）、青色 **BACKGROUND**（Claude 已结束本轮，但它启动的 shell、子代理或 monitor 仍在运行；同样带动画，并显示如“2 shells, 1 subagent running”的说明）、琥珀色 **NEEDS APPROVAL** / **QUESTION**、绿色 **DONE**、红色 **ERROR**、灰色 **IDLE**。耗时至少十秒的任务真正完成（后台不再有任何运行中的任务）或一轮对话失败时，会出现持续 15 秒的横幅（任意键可关闭）。电量显示在右上角；电量计不可用时只绘制图标。

屏幕变暗或熄灭时，第一次按键只会唤醒屏幕。屏幕在所选超时（默认一分钟）后变暗，再过一分钟熄灭；权限和问题提醒会让屏幕保持点亮一分钟。任何提醒都会唤醒屏幕。

## 中文文本与字体

界面标签使用英文。提示、问题、文件名、回复等动态内容可能包含中文。正文字体是 16 px Noto Sans SC Medium 子集，覆盖 ASCII、全部 6,763 个 GB2312 汉字、GB2312 符号及常用标点（7,782 个字形，占用 0.9 MB Flash）。子集之外的字符（如 emoji 或生僻字）会被有意绘制为白色方框（U+25A1），而不是 LVGL 的占位框。字体来源与重新生成命令见 [assets 说明](../../assets/README.zh_CN.md#字库fonts)。

## 构建与烧录

```bash
. <path-to-esp-idf-v5.5.3>/export.sh
./tools/validate.sh            # 静态检查、主机测试、固件构建
```

经过校验的合并镜像为 `build/FoloToy-AI-Passport-full.bin`，烧录地址为 `0x0`。合并镜像烧录会重写 NVS，从而清除本应用的设置和配对信息，之后需要重新配对。参见[烧录策略](../development/engineering/firmware-layout.zh_CN.md#烧录与已存数据)。

## 上机验收清单

- 启动日志显示 `glyph self-test: 0 missing`、LVGL 内存池信息和剩余堆。
- 显示 *Waiting for your Mac* 界面、设备名，右上角显示电量。
- bridge 首次连接时显示配对码，配对成功；设备或 bridge 重启后重连无需再次配对。
- 已绑定一台 Mac 时，新的配对请求会被拒绝，直到使用 *Settings > Pair new Mac*。
- `simulate.py` 覆盖所有状态：卡片颜色、星芒动画、子代理圆点、带提示音的权限与问题提醒、完成与错误横幅、中文项目名和提示文本、详情页滚动。
- 真实 Claude Code 会话：工具活动、子代理、权限提示、`AskUserQuestion` 提问、完成，以及 `/exit` 后会话消失。
- 屏幕会变暗并熄灭；第一次按键只唤醒屏幕。
- 设置在重启后保留；*Forget Macs* 会删除绑定。

## 限制

- hooks 异步运行；bridge 按 hook 启动时间排序事件，因此状态可能比终端晚几百毫秒。
- 最多向设备发送六个会话、每个会话五个子代理；计数仍包含其余部分。
- 子代理内部的每个工具事件是否携带 `agent_id` 取决于 Claude Code 版本；若不携带，子代理行只显示启动、完成和最后已知的活动。
- bridge 必须运行在运行 Claude Code 的那台 Mac 上。BLE 距离约为数米，尚未在本板上实测。
- 常驻 BLE 连接时的功耗尚未实测；除音频 codec 挂起外，背光策略是唯一的省电措施。
