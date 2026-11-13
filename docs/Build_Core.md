## 运行核心和更新器

Nekoray 使用更新中心从 [SagerNet/sing-box](https://github.com/SagerNet/sing-box/releases) 安装官方 sing-box。普通节点、代理链、Nekoray TUN 和完整的 sing-box JSON 配置均由官方二进制运行。Xray 和 Mihomo 配置还需要安装各自的官方核心。

Go 构建只生成更新器。它在 Linux 便携包中同时用作启动器。

```sh
GOOS=linux GOARCH=amd64 ./libs/build_go.sh
GOOS=windows GOARCH=amd64 ./libs/build_go.sh
```
