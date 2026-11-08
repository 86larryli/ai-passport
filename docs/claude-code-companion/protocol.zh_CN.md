<p align="right">
  <strong>简体中文</strong> · <a href="protocol.md">English</a>
</p>

# Claude Code Companion 协议（CCP/1）

bridge（`tools/cc_companion`）与固件（`main/ccp.c`）通过一个 BLE GATT 服务交换 UTF-8 文本行。本文档即协议约定；黄金样本 `tests/fixtures/ccp_scenario.txt` 由 Python 编码器生成、由 C 主机测试解析，`tests/test_cc_companion.py` 会检查下列字节上限和 UUID 与 `main/cc_types.h`、`main/cc_ble.c` 一致。

## 传输层

| 项目 | 取值 |
| --- | --- |
| 服务 | `9aa10001-aa98-4d97-a56d-233f9c782706` |
| RX（bridge 到设备） | `9aa10002-aa98-4d97-a56d-233f9c782706`，write 与 write-without-response |
| TX（设备到 bridge） | `9aa10003-aa98-4d97-a56d-233f9c782706`，notify |
| 广播名称 | `CC-Companion-XXXX`（蓝牙 MAC 末两字节），位于扫描响应中 |
| 安全 | 仅 LE Secure Connections、MITM（设备显示配对码）、绑定；RX、TX 及 TX 的 CCCD 都要求已认证链路 |
| 首选 ATT MTU | 247 |

bridge 使用带响应写入，每块不超过 `MTU - 3` 字节。分块可能在行内任意位置切开，由设备重新拼接成行。设备 4 KB 接收缓冲区满时会以 ATT 错误拒绝写入，bridge 随后重发下一份快照。

## 行格式

- 每行一条记录，以 `\n` 结尾（行尾的 `\r` 被忽略）。
- 字段以 `\t` 分隔，第一个字段是记录类型。
- 文本字段中不含制表符、换行符或其他 C0/C1 控制字符。多行字段（仅 `Q.body`）内的换行以 `0x1F` 发送。
- 除特别说明外，整数均为无符号十进制。
- 单行最长 640 字节，超长行整行丢弃。
- 未知记录类型会被忽略，多余的尾部字段也会被忽略，便于新版 bridge 增加字段和记录。
- 发送方按下表上限截断文本（按字节、在字符边界截断并追加 `...`），设备执行相同的上限。

| 字段 | 最大字节数 |
| --- | ---: |
| 会话/子代理 id | 11 |
| 项目名 | 47 |
| 活动、备注 | 127 |
| 子代理类型 | 23 |
| 子代理活动 | 95 |
| 问题标题 | 63 |
| 问题正文 | 511 |
| 提醒文本 | 95 |
| bridge 版本 | 23 |

## bridge 到设备

| 记录 | 字段 | 含义 |
| --- | --- | --- |
| `HELLO` | 协议版本（`1`）、bridge 版本 | 仅在 TX 订阅成功后发送；设备把它视为订阅已生效的证明，并回复 `HI`。 |
| `CLOCK` | Unix 秒、UTC 偏移分钟数（-1440..1440） | 顶栏时钟。连接时及每 30 秒发送一次。 |
| `SNAP` | seq、会话总数 | 开始一份快照。新的 `SNAP` 会丢弃未完成的快照。 |
| `S` | sid、状态、已持续秒数、项目、活动、子代理总数、运行中子代理数 | 一个会话。其后的 `M`、`Q`、`A` 行属于该会话。 |
| `M` | 文本 | 当前会话最近的提示（`> ...`）或最后一条助手回复。 |
| `Q` | 类型（`perm` 或 `ask`）、标题、正文 | 会话在等待什么：工具名与命令，或问题标题、问题与编号选项。对于问题，bridge 会限制每个选项的长度并优先缩短问题本身，确保所有选项都可见。 |
| `A` | aid、状态、已持续秒数、类型、活动 | 当前会话的一个子代理或后台任务（类型为 `shell`、`monitor`、`workflow` 等），活动为其命令或描述。 |
| `END` | seq | seq 与 `SNAP` 一致时提交快照，否则丢弃。 |
| `EVT` | 类型、sid、项目、文本 | 一次性提醒：`perm`、`ask`、`done`、`err`。紧接着发送反映该事件的快照。 |
| `PING` | — | 5 秒内没有发送其他内容时的保活。 |

会话状态：`work`、`perm`、`ask`、`done`、`idle`、`err`、`bg`（主回合已结束，但后台 shell、子代理、monitor 或 workflow 仍在运行）。子代理状态：`run`、`done`、`err`。时长为发送时处于当前状态的秒数；设备从收到快照起继续计时。

设备保留快照中的前 6 个会话、每个会话的前 5 个子代理；bridge 按显示顺序发送（等待中的会话最先，等待最久的在前；其次是错误；然后是工作中、已完成和空闲会话，最近的在前）。`SNAP` 和 `S` 中的计数包含其余部分。

任何变化都会触发重发快照（150 ms 去抖），且至少每 30 秒发送一次。设备 20 秒未收到任何行时显示 *bridge quiet*。

## 设备到 bridge

| 记录 | 字段 | 含义 |
| --- | --- | --- |
| `HI` | 协议版本、固件版本、电量百分比（-1 表示未知） | 回复 `HELLO`。 |
| `ACK` | seq | 快照已提交。 |
| `ERR` | 原因 | 格式错误的行（每秒最多报告一次）。 |

## 示例

制表符显示为 `→`，`0x1F` 显示为 `⏎`：

```text
HELLO→1→0.1.0
CLOCK→1794095280→-480
EVT→perm→aaaaaaaa→ai-passport→Bash: $ rm -rf build/
SNAP→7→2
S→aaaaaaaa→perm→12→ai-passport→$ idf.py build→0→0
M→> Build the firmware
Q→perm→Bash→rm -rf build/⏎# Clean
S→bbbbbbbb→work→40→cheapbite→Agent: find usages→2→1
A→agent-ex→run→38→Explore→Grep "assay" in data
A→agent-pl→done→30→Plan→Plan ready
END→7
PING
```

## bridge 使用的 hook 事件

| Hook | 作用 |
| --- | --- |
| `SessionStart` | 创建会话（`idle`）。 |
| `UserPromptSubmit` | 进入 `work`；备注变为 `> 提示`；清除已完成的子代理。 |
| `PreToolUse` | 更新活动行（主代理或子代理）。`AskUserQuestion` 设置为 `ask` 并发出提醒。`Task`/`Agent` 调用会记住子代理描述。 |
| `PermissionRequest` | 进入 `perm`，记录工具与命令，并发出提醒。 |
| `PostToolUse`、`PostToolUseFailure` | 从 `perm`/`ask` 回到 `work`。 |
| `Notification` | `permission_prompt` 设置 `perm`；`elicitation_dialog` 与 `agent_needs_input` 设置 `ask`。 |
| `SubagentStart`、`SubagentStop` | 添加子代理行；完成时标记为 done 并记录最后回复。处于 `bg` 的会话在没有任务运行后进入 `done`。 |
| `Stop` | 用其 `background_tasks` 列表替换后台行。仍有任务运行时会话进入 `bg`（如“2 shells, 1 subagent running”）；否则进入 `done`，当整个任务（包括继续处理后台工作的回合）耗时至少 10 秒时发出提醒。 |
| `StopFailure` | 进入 `err` 并发出提醒。 |
| `SessionEnd` | 移除会话。8 小时没有事件的会话也会被移除。 |

hooks 异步运行，可能乱序到达。每个事件都携带其 hook 的启动时间，只有不早于被替换状态的变更才会生效。
