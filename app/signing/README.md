# 固定签名密钥

`yieyu.jks.b64` 是这个 App 的**唯一**签名密钥（base64 后的 JKS）。
所有 CI 构建都用它，因此每次出的 APK 签名一致 → **可以直接覆盖安装**。

## 为什么必须固定

`flutter build apk --release` 的默认模板用 `signingConfigs.debug`，
而 debug keystore 是每个 CI runner 现场生成的 → 每次签名都不同 →
Android 会拒绝"应用未安装/签名不一致"。

## 首次是怎么来的

`.github/workflows/build.yml` 里有一道 `0.3` 步骤：仓库里没有这个文件时，
用 `keytool` 生成一把 RSA-2048 / 100 年有效期的密钥，然后**提交回仓库**。
之后所有构建都复用它。

## ⚠️ 安全提示（重要）

这个仓库是 **public**，密钥和口令都在仓库里。这意味着：
- 任何拿到仓库的人都能签出"和官方一模一样"的包；
- 对一个自己折腾的项目来说可以接受，**但绝不能用于任何商业分发**；
- 如果哪天要正经发版，第一步就是换密钥 + 把密钥移到 GitHub Secrets。

口令：见 `app/build-app.sh` 里的 `STORE_PASS`（同一处改）。
