# 本地投机，远程推理：面向微控制器 LLM Agent Loop 的守卫式投机执行

**研究计划书（Research Proposal）** — 草稿 v1.0，2026 年 7 月
实验平台：MimiClaw（ESP32-S3 agent 固件）

英文版：`specclaw-research-proposal.md`

---

## 摘要

基于 ReAct 范式的大语言模型（LLM）agent 通过"每步一次模型调用"来交替进行推理与行动。当 agent loop 被托管在微控制器（MCU）上——这一部署形态最近已被 MimiClaw、ESP-Claw 等开源系统证明可行——这种逐步调用的结构变得格外昂贵：每次调用都要支付一笔与 token 数无关的固定成本，包括 radio 唤醒、TLS 握手与网络往返，在电池供电设备上该成本主导整体能耗。我们提出**守卫式投机执行（guarded speculative execution）**：一次云端调用即"草拟"计划中接下来的 k 步，每步以（动作，守卫包络，超时）三元组表示；MCU 使用约一百行 C 代码构成的确定性守卫求值器在本地逐步验证并提交这些步骤，仅当预测与观测现实偏离时才回调云端。与近期涌现的服务器端投机 agent 系统不同——它们通过急切执行可丢弃的动作并用第二个模型验证来优化延迟——我们的场景反转了其全部核心假设：验证者是由确定性代码解读的物理世界而非 LLM；优化目标是调用次数本身而非延迟重叠；物理执行器上的动作不可逆，因此投机必须遵循"先验证后提交"（commit-after-verify）语义，绝不允许急切执行。这一反转暴露出一种新的失败模式——**静默偏离（silent divergence）**，即守卫通过但环境已发生语义偏离——我们对其给出定义、测量方法，并通过守卫设计与不可逆性感知的深度上界加以控制。我们将在真实 ESP32-S3 硬件上，以一个横跨任务长度、环境随机性与动作不可逆性三个维度的 18 任务测试集评估该系统，报告实测能耗、上行字节、调用次数与延迟，以及任务成功率。预期结果包括：首个关于"投机深度作为环境可预测性函数"的系统性刻画，以及首个针对设备托管 agent loop 断网韧性的量化分析。

---

## 1. 引言

ReAct 范式 [1] 将 LLM agent 组织为一个循环：模型对当前情境进行推理、选择动作、观察结果、如此往复。几乎所有关于此类 agent 的研究都假设该循环运行在服务器或工作站上。这一假设在 2026 年被悄然打破：一批开源项目——乐鑫官方的 ESP-Claw、WireClaw、zclaw 以及我们自己的 MimiClaw——展示了完整的 agent loop 在 ESP32 级微控制器上运行的形态：工具执行、记忆与通道管理都在设备上，只有推理这一步委托给云端 LLM API。这些系统能够工作，但从未被研究过：没有任何已发表的工作刻画过它们的成本结构，也没有任何已发表的机制针对它们的核心低效之处。

这一低效是结构性的。ReAct 循环每步发起一次云调用，而在 MCU 上每次调用都携带一笔与 token 无关的固定开销：唤醒 radio、完成 TLS 握手（ESP32-S3 上无连接复用时约一至三秒）、等待网络往返。一个二十步的任务要付二十次这笔"过路费"。服务器端 agent 研究以 token 计量成本，因此从未直面这一项；而在电池供电设备上，它是主导项。

我们提出直接攻击调用次数。在我们称为 **SpecClaw** 的设计中，云端模型对每次请求的回应不是单个动作，而是一段简短的**投机脚本（speculation script）**：它预期任务接下来需要的 k 个动作，每个动作附带一条机器可检查的**守卫包络（guard envelope）**——描述该步骤保持有效所需满足的传感器条件——以及一个超时。MCU 逐步执行该脚本，用确定性 C 代码求值守卫（设备上不存在任何模型），且每个动作仅在其前置条件通过后才提交。当某条守卫失败时，设备中止投机，生成一个紧凑的任务状态快照，并发起一次云调用以获取新脚本。若环境行为符合预测，一个原本需要 n 次调用的任务只需 n/L 次即可完成，其中 L 是每段脚本平均被接受的步数。

这一思想将投机执行的逻辑——处理器中的分支预测、LLM 推理中的投机解码 [18]——移植到 agent loop 上；近期已有一系列工作在服务器端完成了这一移植 [2–8]。我们的贡献不在于这个类比本身，而在于它在嵌入式约束下的反转。这一反转从三个方面改变了问题的性质（第 2.2 节），使其成为一个独立且未被研究过的对象；其中最关键的是不可逆性：一次被错误投机执行的搜索查询毫无代价，而一次被错误投机执行的继电器动作无法收回。

## 2. 背景与动机

### 2.1 平台：MCU 托管的 agent loop

我们的实验平台 MimiClaw 是一个面向 ESP32-S3（双核 Xtensa LX7、PSRAM、16 MB flash）的开源 agent 固件，基于 ESP-IDF 与 FreeRTOS 构建。agent loop 作为一个独立任务运行：它从由 Telegram、飞书与 WebSocket 通道供给的队列中取出用户消息，从存储于 SPIFFS 的人格、记忆与会话文件组装上下文，经 TLS 调用 Anthropic 或 OpenAI 兼容 API，并执行返回的工具调用（经策略白名单约束的 GPIO 操作、文件访问、定时任务、网页搜索），每条消息最多十轮迭代。cron 与 heartbeat 子系统已经赋予设备无人值守的长期运行形态。这个系统实际上是一个装进消费级 IoT 固件体积里的完整 ReAct agent——它既是该部署形态真实存在的证据，也是本计划的 baseline 实现。

MimiClaw 及其同类项目都不具备的，是介于"每步都调云"与"完全不用云"之间的任何机制。本计划要提供的正是这个中间地带。

### 2.2 为什么服务器端投机无法直接迁移

面向 LLM agent 的投机执行是一个活跃领域：双模型的草拟-验证式规划 [2]、搜索 agent 中的推理/动作重叠 [3]、后台异步工具执行 [4]、模式引导的投机 [5, 6]、空闲时间投机 [7]，以及投机工具调用的隐私后果 [8]。所有这些工作共享三个假设，而每一个在我们的场景中都不成立。

第一，**验证者是模型**。服务器端系统用更强的 target 模型来检查 draft agent 的提议。MCU 无法承载任何模型；我们的验证者是经传感器读出、由守卫语言解读的物理世界本身——该语言的设计目标就是能被极简的确定性代码求值。

第二，**目标是延迟**。服务器端投机通过与工具执行重叠来隐藏推理时间；调用依然频繁，只是串行化程度改善。我们的目标是调用次数，因为在设备上每次调用是一个固定的 radio 能量与握手延迟量子。同一机制瞄准不同的成本项会导出不同的设计选择——深度比重叠更重要，并行发起投机调用也不再有收益。

第三，**动作可丢弃**。预取一个最终没用上的搜索结果只浪费 token。驱动电机、放灌溉水、锁存继电器则无法丢弃。物理动作上的投机因此必须是**先验证后提交**的：任何动作在其守卫通过前不得执行，且不可逆动作还受深度上界 D_irrev 约束——即自上次云端确认以来允许投机提交的不可逆动作数量上限。这一约束在服务器端文献中没有对应物，并催生了本计划的核心新研究对象：**静默偏离（silent divergence）**——某步的守卫通过了，但环境已偏离任务的语义意图，导致一次错误的、任何后续重规划都无法撤销的物理动作。

### 2.3 次级动机：断网韧性

由于投机脚本驻留在设备上，MCU 可以在网络中断期间继续执行其已验证的前缀。云端编排的 agent 在连接断开的瞬间即告停摆。投机因此兼具可用性机制的作用，我们将断网存活率作为一等评估维度而非附带观察。

## 3. 相关工作

**服务器端投机 agent。** 上文引述的系统 [2–8] 为 agent loop 建立了草拟、验证与接受的词汇体系，但完全运行在第 2.2 节的三个假设之内。我们将本工作定位为其嵌入式对偶：世界作为验证者、调用作为成本、动作不可逆。

**计划-执行式 agent。** ReWOO [9] 与 LLM Compiler [10] 用一次调用生成完整计划并在无逐步反馈的情况下执行，获得了调用次数上的收益，却完全没有安全性：开环计划会盲目地穿过环境漂移继续执行。ReWOO 式执行是我们的 baseline B2，我们预期它恰好在守卫发挥价值的地方失败。

**设备-云闭环。** EcoAgent [11] 是最接近的先例：云端规划器输出带逐步预期的完整步骤列表，设备端 agent 执行并验证每一步，失败时升级到云端。关键区别在于 EcoAgent 的设备端验证由智能手机上的一个 2B 参数视觉语言模型完成。MCU 上跑不了任何规模的模型。我们的守卫语言正是为了用确定性代码替代那个模型——这同时使验证变得可审计，而 VLM 裁判无法提供这一性质。此外，EcoAgent 的投机深度固定为"整个计划"，不区分可逆与不可逆动作，其评估报告调用与 token 但不含能耗与字节。

**LLM 到 MCU 的协议。** Device Context Protocol [12] 采取相反的架构立场——loop 留在主机上，MCU 只执行预验证的命令——其能力作用域与范围检查思想被我们吸收进守卫语言。loop 在设备与 loop 在主机两种架构在断网条件下的对比是我们评估的一部分。

**上下文与状态压缩。** MEM1 [13]、StateAct [14] 以及近期的常量上下文表述 [15] 将 agent 历史压缩为可重写或结构化的状态。我们在重预测载荷上借鉴了这条线（定长纯文本状态块；StateAct 关于 JSON 格式状态损害准确率的发现直接决定了其序列化方式），但需要指出：这些工作都不涉及执行语义，而执行语义正是我们的核心课题。观测掩蔽 [16] 是该文献中强而廉价的 baseline，出现在我们的消融实验中。

**前 LLM 时代的嵌入式 agent。** 编译到裸机固件的 BDI 架构 [17] 表明 MCU 上的 agent loop 有前 LLM 时代的血统；LLM 时代的新变量在于策略成了一个远程的、昂贵的、可能出错的 oracle——而这恰恰是投机得以获利的条件。

## 4. 方法设计

### 4.1 投机脚本

每次云调用经结构化输出返回如下形式的脚本：

```json
{
  "state_update": "relay=on; last_temp=27.3; goal_phase=cooling",
  "script": [
    { "step": 1,
      "action": {"tool": "gpio", "args": {"pin": 5, "level": 1}},
      "irreversible": false,
      "guard": {
        "pre":  [{"var": "temp_c", "op": "gt", "val": 26.0}],
        "post": [{"var": "temp_c", "op": "lt", "val": 26.5, "within_s": 120}]
      },
      "on_fail": "replan" }
  ],
  "done_when": [{"var": "temp_c", "op": "lt", "val": 25.0}]
}
```

守卫语言刻意保持最小：数值比较、离散相等、有界变化率、时间窗口，且仅支持合取（AND）。受限的表达力正是设计要点——守卫必须可判定、可用约一百行 C 代码求值、可静态审计。至于 LLM 能否在如此小的语言中**可靠地生成**充分的守卫，这本身就是我们的研究问题之一（RQ3）。

### 4.2 守卫式提交语义

执行逐步推进：求值前置守卫（失败即判定为预测失误 mispredict）；强制执行不可逆深度上界 D_irrev（超出则强制回云确认）；经固件既有的策略白名单执行动作；在时间窗口内等待后置守卫（违反即 mispredict）；将该步追加到持久化提交日志。发生 mispredict 时，设备生成一个紧凑状态快照——目标、当前传感器状态、已提交前缀、失败的守卫及实际观测值、以及一个有界的要点事实列表，序列化为 300–500 字节的纯结构化文本——然后发起一次云调用获取新脚本。因此每次重规划的上行载荷是 O(1) 的，与标准 ReAct 中随步数增长为 O(n) 的 transcript 形成对照。

两条不变量定义了该模型：任何动作在其前置守卫通过前不得执行；投机层永不绕过其下的安全层。

### 4.3 术语定义

我们定义**接受长度 L（acceptance length）**为每次云调用后被连续验证并提交的步数（投机解码中 acceptance length 的系统级类比）；**静默偏离（silent divergence）**为守卫通过但环境已发生语义偏离时被提交的步骤（守卫 false-accept，以模拟器 ground truth 判定）；**守卫误拒（guard false-reject）**为环境未偏离但过紧的守卫触发的重规划。这三个量与 D_irrev 一起参数化了整个设计空间。

## 5. 研究问题

**RQ1（收益）。** 在任务成功率持平的条件下，守卫式投机相对逐步 ReAct 能降低多少云调用、能耗与中位步延迟？*假设：调用次数下降 L 倍（L ∈ [3, 8]）；由于固定的每调用开销占主导，每任务能耗下降至少 60%；步延迟分布呈双峰（毫秒级本地步与秒级云端步）。*

**RQ2（深度与随机性）。** 有利可图的投机深度如何随环境可预测性变化？*假设：确定性环境中 L 随给定深度 k 增长；噪声与注入故障增强时 L 塌缩趋向 1，且存在一个可测量的交叉点，越过后投机不再有净收益。这条曲线——"什么环境值得投机多深"——是本计划的核心实证贡献。*

**RQ3（守卫质量）。** LLM 生成的守卫呈现怎样的 false-accept/false-reject 权衡？包络松紧与守卫密度如何移动这一权衡？*假设：默认生成的守卫偏松（静默偏离非零）；提示词要求"每个动作至少一条可观测后置条件"能以适度的误拒代价抑制静默偏离，勾勒出 ROC 形的前沿。*

**RQ4（断网）。** 在注入 10、60、600 秒网络中断时，投机执行的任务存活率比逐步 ReAct 与云端编排架构高多少？*假设：中断落在已验证前缀内的任务不受影响地完成，且存活率差距随深度扩大。*

## 6. 评估方案

**实验基底。** 所有方法（含 baselines）均实现为同一 MimiClaw 固件的编译配置，消除实现差异的干扰。主基底为"硬件在环 + 模拟环境"：ESP32-S3 运行真实固件、发起真实 API 调用，传感器与执行器则由 host 侧 Python 模拟器经固件既有的 WebSocket gateway 提供。模拟是必需的：静默偏离只能对照 ground truth 测量，且随机性必须可复现地注入。一个物理台架（温度传感器、继电器风扇、INA226 电流计）运行任务子集，使能耗数字建立在实测而非模拟之上。

**任务集。** 十八个任务排布在三维网格上：任务长度（3–5、8–12、20+ 步）、环境可预测性（确定性；高斯传感器噪声加偶发野值；对抗性注入 I2C 错误、执行器失效与状态突变）、不可逆性（只读；可逆执行；不可逆执行，如计量放水）。每个任务在模拟器侧配备 ground-truth 判定函数。代表性任务：恒温调节、定时灌溉、多传感器故障诊断、有序执行器上电序列，以及测试跨重规划事实保留能力的带约束调度任务。

**Baselines。**

| 编号 | 描述 |
|---|---|
| B1 | 逐步 ReAct（现有 MimiClaw loop） |
| B1+ | B1 开启 Anthropic prompt caching，预先回应"缓存已解决成本"的质疑 |
| B2 | ReWOO 式计划-执行：一次完整计划、开环、无守卫 |
| B3 | EcoAgent 设计的 MCU 可行改编：全长计划配朴素相等守卫，任何失败触发整体重规划 |
| Ours | 可调深度 k、包络守卫语言、紧凑状态重预测、D_irrev |

**指标。** 对照 ground truth 的任务成功率；平均接受长度 L；每任务云调用数、输入/输出 token、上行字节；每任务焦耳（台架子集）；步延迟分布；静默偏离率与守卫误拒率；PSRAM 高水位；断网存活率。每配置至少 10 个随机种子，报告 95% 置信区间。

**消融。** 守卫密度（无/每步一条/每步多条后置条件）；包络松紧（模型默认与提示词收紧）；重预测载荷（紧凑状态与完整 transcript 对比，衔接上下文压缩文献）；D_irrev ∈ {0, 1, 3, ∞}；模型档位（Haiku 与 Sonnet，考察协议对模型能力的要求）。另设一个自适应变体——模型自行决定投机视界并在不确定处提前截断脚本——与固定深度对比；学习型调度器不在范围内。

## 7. 预期贡献

1. 首个面向不可逆物理动作的 agent loop 投机执行语义——带不可逆深度上界的守卫式提交——以及静默偏离这一服务器端投机中不存在的失败模式的定义与测量方法。
2. 一种云端 LLM 可生成、MCU 可确定性求值的最小守卫语言，替代既有设备-云闭环中的设备端模型验证。
3. 首个在真实硬件上、以物理单位（焦耳、字节、调用次数）进行的 MCU 托管 agent loop 系统评估，包括深度-随机性刻画与量化的断网韧性。
4. 开源产物：固件（MimiClaw 主线的一个编译开关）、环境模拟器与任务集，作为未来 MCU-agent benchmark 的奠基工作。

## 8. 工作计划

六个里程碑，约十三周。**M0**（1.5 周）：逐调用计量插桩（握手、TTFB、字节）、模拟器骨架、驱动 baseline B1 的自动化 runner。**M1**（2 周）：脚本协议、解析器、守卫求值器与执行器快路径，以恒温任务上的 smoke 实验收尾；在此节点发布 arXiv 技术报告以确立优先权。**M2**（1.5 周）：状态快照、持久化提交日志、D_irrev 强制执行与畸形脚本拒收。**M3**（2.5 周）：完整任务集、随机性与断网注入、其余 baselines，以完整配置矩阵的无人值守运行收尾。**M4**（2 周）：台架搭建、能耗实测与消融，产出全部目标图表。**M5**（3 周）：论文写作与补充实验。

M1 的 smoke 实验被有意安排为最早的风险探针：若恒温任务无法达到 L ≥ 3，或生成守卫中畸形比例过高，则在实验面铺开之前重新设计协议。

## 9. 风险

主要的实证风险是实测 L 偏短，会削弱收益叙事；对策在于固定开销的账在 L = 2 时已然成立，且 RQ2 将"何时不值得投机"本身框定为可发表的发现而非失败。主要的评审风险有二：与 EcoAgent 的对比（以五处具体差异正面回应：确定性守卫替代设备端 VLM、不可逆性语义、可调深度、物理单位指标、断网分析），以及对模拟环境的怀疑（以物理台架和取自真实传感器 datasheet 的模拟噪声参数回应）。领域层面的风险是撞车——该领域以半年为周期推进——M1 节点的 arXiv 发布即为应对；在撰写本计划时，投机 × MCU × 不可逆执行这一交叉点尚无人占据。

## 10. 发表计划

主要目标：SenSys、MobiSys 或 IPSN（完整论文）；先以四页版本投 HotMobile/HotEdge 类 workshop 作为早期占位。产物以公开 MimiClaw 仓库的一个编译配置形式发布。

---

## 参考文献

[1] Yao et al. *ReAct: Synergizing Reasoning and Acting in Language Models.* arXiv:2210.03629.
[2] Guan et al. *Dynamic Speculative Agent Planning.* arXiv:2509.01920.
[3] *SPAgent: Reducing Latency of LLM Search Agents via Speculation-Based Algorithm–System Co-Design.* arXiv:2511.20048.
[4] *Sherlock: Reliable and Efficient Agentic Workflow Execution.* arXiv:2511.00330.
[5] *Act While Thinking: Accelerating LLM Agents via Pattern-Aware Speculative Tool Execution.* arXiv:2603.18897.
[6] *B-PASTE: Beam-Aware Pattern-Guided Speculative Execution for Resource-Constrained LLM Agents.* arXiv:2604.16469.
[7] *IdleSpec: Exploiting Idle Time via Speculative Planning for LLM Agents.* arXiv:2605.22154.
[8] *Ghost Tool Calls: Issue-Time Privacy for Speculative Agent Tools.* arXiv:2606.02483.
[9] Xu et al. *ReWOO: Decoupling Reasoning from Observations for Efficient Augmented Language Models.* arXiv:2305.18323.
[10] Kim et al. *An LLM Compiler for Parallel Function Calling.* arXiv:2312.04511.
[11] Yi et al. *EcoAgent: An Efficient Device–Cloud Collaborative Multi-Agent Framework for Mobile Automation.* AAAI 2026. arXiv:2505.05440.
[12] Yang. *Device Context Protocol: Safety-First LLM Control of Constrained Devices.* arXiv:2605.26159.
[13] *MEM1: Learning to Synergize Memory and Reasoning for Efficient Long-Horizon Agents.* arXiv:2506.15841.
[14] Rozanov and Rei. *StateAct: Enhancing LLM Base Agents via Self-Prompting and State-Tracking.* arXiv:2410.02810.
[15] *Remember, Don't Re-read: Stateful ReAct Agents for Token-Efficient Autonomous Experimentation.* arXiv:2606.14945.
[16] Lindenbauer et al. *The Complexity Trap: Simple Observation Masking Is as Efficient as LLM Summarization for Agent Context Management.* arXiv:2508.21433.
[17] *Embedding Autonomous Agents in Resource-Constrained Robotic Platforms.* arXiv:2601.04191.
[18] Leviathan et al. *Fast Inference from Transformers via Speculative Decoding.* arXiv:2211.17192.
