# 启动前 SSH 隧道建立

> 每次启动 cockpit.exe 前，必须先建立 SSH 隧道连接到工作站推理服务器。

---

## 一、前提

| 项目 | 值 |
|------|-----|
| 工作站 SSH 地址 | `100.127.220.110` |
| SSH 用户名 | `nakamura-kumoutau` |
| 工作站推理端口 | `8090` |
| 本地隧道端口 | `58090` |

---

## 二、建立隧道

打开 **Git Bash** 或 **PowerShell**，运行：

```bash
ssh -L 58090:localhost:8090 -N -f nakamura-kumoutau@100.127.220.110
```

> `-L 58090:localhost:8090` — 本地 58090 → 工作站 localhost:8090  
> `-N` — 不执行远程命令  
> `-f` — 认证后转入后台  

输入密码时**屏幕无回显**是正常的，盲打后按回车。

---

## 三、验证隧道

```bash
# 检查本地端口是否在监听
netstat -ano | findstr 58090
```

应看到一行 `TCP 127.0.0.1:58090 ... LISTENING`。

```bash
# 确认工作站推理服务在运行（SSH 进去看）
ssh nakamura-kumoutau@100.127.220.110 "ss -tlnp | grep 8090"
```

---

## 四、启动驾驶舱

确认隧道存活后：

```bash
cd E:\desktop\MS
cockpit.exe
```

启动日志应显示：

```
[STARTUP] AI Advisor: connecting to 127.0.0.1:58090
```

---

## 五、关闭隧道

```bash
# 查找 SSH 进程
tasklist | findstr ssh

# 终止（替换 PID）
taskkill //F //PID <PID>
```
