基于 @guide_agent.v2.md 的方案 结合本项目的所有markdown文件以及workflow/taskflow等代码的详细查看，对于 @guide_agent.md 提出的框架进行细致完善，如有必要请使用mermaid图细化框架以及实现过程。

下面是具体要求：
如果基于taskflow/blob/dev这个项目如何开发一个支持多线程运行且支持MCP工具的agent&workflow的LLM的智能框架呢？我想使用纯c++来实现一个高性能的库便于智能化业务通过命令行，imgui，以及web client来业务运行。
结合上面的回答，你再深度浏览taskflow的workflow子目录以及相关的头文件，实现以及例子。基于其申明式图构造方式进行合理设计适用前述任务的技术框架。特别的或许可以将许多agent以循环子图等形式分装，而LLM可以是具有系统提示词，用户提示词，以及输入输出参数的节点，而知识库可以以一个source节点提供的api来作为下一个节点的查询源，sink节点来处理所有的输出。。。

本文档第四章前面少一个整体设计以及技术框架以明确核心模块与节点，在第五章与第六章太空了，没有实现技术路线。第八章也没有具体化关键技术。第九章应该可以进行再次拓展作为第四章前面缺失的部分。在整个框架设计之初就要考虑agent与工作流都兼容，同时可以嵌套（即agent或者工作流可以作为独立节点）。


在@guide_agent.md 完善对于A2A的支持，然后再在 /home/mapoet/Data/works/taskflow/agent_framework/docs/architecture/overview.md 补充完善 A2A方面的抽象与继承设计。

参考链接
```
https://github.com/ai-boost/awesome-a2a
https://zhuanlan.zhihu.com/p/1894797987739324876
```

A2A协议：
```
What (是什么): Agent2Agent (A2A) 是一个由 Google 及合作伙伴倡导的开放协议，旨在让来自不同供应商或基于不同框架构建的异构 AI Agent（智能体）能够进行安全的通信和任务协作。它本质上是为 AI Agent 交互定义的一套标准“沟通语言”和框架。
Why (为什么需要): 为了解决当前 AI Agent 生态普遍存在的“智能孤岛”问题。缺乏互操作性阻碍了构建能够跨越不同 Agent 系统边界、协同完成复杂任务的应用。A2A 的目标是促进形成一个开放、协作的 Agent 生态系统，打破技术壁垒和供应商锁定，并为企业级集成提供基础。
How (如何实现): A2A 基于成熟的标准网络技术（如 HTTP, JSON-RPC 2.0, SSE）。它定义了一套核心概念和机制，包括：用于服务发现和能力描述的 Agent Card；用于管理协作工作单元及其状态的 Task；用于承载多模态信息交换的 Message 和 Part；以及用于表示最终成果的 Artifact。协议强调异步优先、模态无关、安全可靠以及不透明执行的交互原则，并通过标准化的方法（如 tasks/send, tasks/get）来驱动协作流程。
(资源入口): 欲了解协议细节、查找实现库或代码示例，请访问社区维护的权威资源库 Awesome A2A:https://github.com/ai-boost/awesome-a2a。

A2A 协议定义了几个核心的数据结构，作为 Agent 之间信息交换和任务管理的基础。


Agent Card (智能体名片) 是 Agent 进行自我描述和被发现的基础。这份标准化的 JSON 文档包含了 Agent 的基本信息（名称、描述、提供商等）、API 端点 URL、支持的能力（如流式传输、推送通知）、认证方案要求以及最重要的技能列表 (Skills)。每个 Skill 描述了一项具体能力及其细节。Agent Card 是 A2A 互操作性的起点，客户端通过它来了解远程 Agent 并确定如何安全地与之交互。


Task (任务) 是跟踪和管理一次协作交互的核心实体。它是一个有状态的工作单元，包含唯一的 Task ID、可选的 Session ID（用于关联相关任务）、当前的状态（如 working, completed, input-required 等）、交互历史（一系列 Message 对象）、生成的工件列表 (Artifacts) 以及扩展元数据。Task 的生命周期由客户端创建，状态由服务器管理。

Message (消息) 是 Agent 之间传递非最终成果信息的载体，用于承载指令、上下文、状态更新、错误信息等。一个 Message 包含来源角色（user 或 agent）和一系列 Part (部件)。


Part (部件) 是构成 Message 或 Artifact 内容的基本单元。每个 Part 包含类型（如 text, file, data）和对应的数据内容（文本字符串、包含 MIME 类型和 URI/字节的文件描述、或 JSON 对象），允许在单条消息或单个工件中混合传输不同类型的信息。


Artifact (工件) 代表任务执行完成后产生的最终输出或成果物，如报告、代码、确认信息等。它通常是不可变的，并由一个或多个 Part 组成，承载实际的成果内容。


2.4 关键交互模式与协议方法


A2A 定义了一组基于 JSON-RPC 2.0 的标准方法来实现 Agent 间的交互流程。


整个交互始于 Agent 发现与连接建立。客户端首先需要获取目标 Agent 的 Agent Card，这可以通过访问预定义的 .well-known 路径、查询 Agent 注册中心或带外机制完成。解析 Agent Card 后，客户端了解其能力、端点和认证要求，并根据要求通过标准认证流程获取访问凭证。


任务生命周期管理 是核心交互环节。客户端使用 tasks/send 方法向远程 Agent 发送消息，以创建新任务或更新现有任务（如提供额外输入或迭代指令）。远程 Agent 处理后返回更新后的 Task 状态。客户端可以使用 tasks/get 方法轮询任务的当前状态和已生成的 Artifacts，或获取交互历史。如果需要，客户端可以通过 tasks/cancel 请求取消一个正在进行中的任务。


针对异步通信与更新，协议提供了更高效的机制。客户端可以使用 tasks/sendSubscribe 发起任务并同时订阅服务器推送的更新（需服务器支持 SSE）。服务器通过 SSE 连接实时推送 TaskStatusUpdateEvent (状态变更) 和 TaskArtifactUpdateEvent (流式传输结果)。如果 SSE 连接中断，客户端可以通过 tasks/resubscribe 重新订阅事件流。对于完全断开连接的场景，客户端可以通过 tasks/pushNotification/set 为任务配置一个 Webhook URL，服务器将在任务状态发生重要变化时向该 URL 发送包含 Task 更新信息的 HTTP POST 请求。客户端也可以用 tasks/pushNotification/get 查询当前的推送配置。

认证与安全交互实践 贯穿始终。客户端在每次请求时都必须在 HTTP Header 中携带有效的认证凭证。服务器必须验证这些凭证，并在失败时返回标准 HTTP 错误码。对于推送通知，服务器应验证 Webhook URL 的所有权，接收端也需要验证推送来源的真实性，例如通过签名校验。全程强制使用 HTTPS 保证传输安全。
```

A2ATaskStatus -> AgentTaskStatus
A2ASkill -> AgentSkill
A2AAgentCard -> AgentCard
A2AFileInfo -> AgentFileInfo
A2APart -> AgentPart
A2AMessage -> AgentMessage
A2AArtifact -> AgentArtifact
A2ATask -> AgentTask