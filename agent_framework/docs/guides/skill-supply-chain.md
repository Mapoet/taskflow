# Skill Supply Chain（Stage 8）

Stage 8 为 Skill 增加可复现归档、CycloneDX SBOM、来源证明、Ed25519 签名、发布者信任策略、
签名 Registry 索引、镜像回退以及固定摘要的在线/离线导入。所有真实性和完整性检查都发生在
store、lock、history 与 Registry generation 变更之前；任一检查失败均 fail closed。

## 归档与审计材料

`.tfskill` 是规范化 ZIP32：条目按 UTF-8 路径排序、固定时间戳、Store 模式、无 extra/comment，
文件权限归一化为 `0644` 或 `0755`。构建器拒绝绝对路径、`..`、链接、特殊文件、大小超限、
重复路径和大小写折叠冲突。相同输入和构建参数产生逐字节相同的归档。

```bash
skillctl --root ROOT package build SKILL_DIR skill.tfskill \
  --source https://registry.example/skills/gnss-ro-qc \
  --revision 0123456789abcdef --builder ci.example/build-v1
skillctl --root ROOT package inspect skill.tfskill
skillctl --root ROOT package sbom SKILL_DIR
```

经过审计的归档包含 `META-INF/sbom.cdx.json` 和 `META-INF/provenance.json`。SBOM 使用
CycloneDX 1.6；provenance 记录 source URI、revision、builder 和输入资源摘要。验证时会重新生成
并比较这两份材料，不能仅依赖归档内声明值。

## 签名与信任库

包和 Registry 索引使用不同 `subjectKind` 和 trust role。签名算法固定为 Ed25519，签名输入是带
magic 与长度前缀的二进制字段序列，覆盖 subject digest、发布者、source URI、SBOM 与 provenance
摘要。`keyId` 是公钥 DER 的 SHA-256；私钥只从本地文件读入，命令输出不会回显私钥内容。

```bash
skillctl --root ROOT package sign skill.tfskill skill.tfskill.sig \
  --key publisher-ed25519.pem --publisher org.example \
  --source https://registry.example/skills/gnss-ro-qc
skillctl --root ROOT package verify skill.tfskill skill.tfskill.sig \
  --trust trust.json --now 1784160000
```

最小信任库如下。生产环境应设置有效期、最小化 role/source scope，并通过
`revokedPublishers`、`revokedKeys`、`revokedPackages` 分发撤销状态。

```json
{
  "apiVersion": "agent.taskflow/skill-trust-store/v1",
  "keys": [{
    "keyId": "<sha256-of-public-key-der>",
    "publisher": "org.example",
    "publicKeyPem": "-----BEGIN PUBLIC KEY-----\n...\n-----END PUBLIC KEY-----\n",
    "roles": ["package", "registry"],
    "sourcePrefixes": ["https://registry.example/skills/"],
    "notBefore": 1780000000,
    "notAfter": 1811536000
  }],
  "revokedPublishers": [],
  "revokedKeys": [],
  "revokedPackages": []
}
```

未知 key、发布者不匹配、role/source 越权、超出有效期、撤销、签名损坏或摘要不匹配都会拒绝。
没有 OpenSSL 时签名能力不可用并明确失败，不会降级为“已验证”。

## Registry、镜像与固定解析

Registry v1 索引独立签名，artifact 固定 `packageId + version + digest + size`，并声明有序 mirrors
和 detached signature URI。`sync` 先验证索引签名；`resolve` 只接受精确版本，不执行浮动解析。

```bash
skillctl --root ROOT registry sync index.json index.json.sig trust.json \
  https://registry.example/index.json
skillctl --root ROOT registry resolve index.json index.json.sig trust.json \
  https://registry.example/index.json gnss-ro-qc 1.2.0
```

`SkillRemoteRegistryClient::fetch_pinned` 按顺序尝试 mirrors。每个候选都必须同时匹配声明 size、
SHA-256 和包签名；失败候选不会留下部分文件，所有镜像失败也不改变安装状态。transport 接口与
provider 无关；内存 transport 用于无网络 CI，curl transport 仅接受 HTTPS，并设置协议、跳转、
响应大小和超时边界。部署侧仍应以网络 namespace/egress allowlist 约束 DNS 和目标 IP。

## 已验证安装与离线导入

远程来源必须提供签名和信任库。摘要固定值来自已验证 Registry 索引：

```bash
skillctl --root ROOT --store STORE install skill.tfskill \
  --signature skill.tfskill.sig --trust trust.json --remote \
  --digest <archive-sha256> --registry-digest <index-sha256> --now 1784160000
```

离线导入使用同一 `.tfskill`、detached signature、trust store 和预先固定的 archive digest；
去掉 `--remote` 不会放宽签名或摘要验证。只有明确的本地开发流程可使用
`--allow-unsigned-local`，该标志对 `--remote` 无效。目录安装继续作为 legacy 兼容入口，并在
store/lock 中记录 `legacyUnsigned: true`，不能被解释为已验证包。

安装后 store 与 lock 持久化 archive、publisher、key、signature、SBOM、provenance 和 Registry
摘要。加载时重新检查已保存归档，防止安装后替换；lock replay 使用精确包身份，不随后续 Registry
generation 漂移。

## 安全边界与非目标

- Stage 8 验证来源和内容身份，但不代表发布者代码安全；运行时仍必须执行 Stage 2–7 的权限、
  sandbox、resource budget 和 schema gate。
- 当前不提供透明日志、阈值/多方签名、在线证书状态、TUF/Sigstore 兼容或自动密钥轮换。
- 当前 Registry resolver 只接受精确版本；依赖 SemVer 求解仍由 lifecycle lock 流程负责。
- curl transport 不替代部署环境的 DNS/egress 防火墙。高保证环境应只开放受控 Registry/mirror。
- 格式契约见安装的 `skill-{package,signature,provenance,trust-store,registry}-v1.schema.json`。
