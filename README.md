<<<<<<< HEAD
# motherfucker

=======
# 航空驾驶舱模拟控制系统

## Flight Cockpit Simulation Control System

基于 **C 语言 + SDL2** 的桌面级飞行驾驶舱仿真系统。实现商用客机核心航电设备模拟，通过 UDP 协议对接 X-Plane 11 获取实时飞行数据。

---

## 功能模块

| 模块 | 说明 |
|------|------|
| **PFD** | 主飞行显示器 — 姿态指示、空速带、高度带、航向带、FMA |
| **ND** | 导航显示器 — 罗盘玫瑰、范围环、航向标、风矢量 |
| **EICAS** | 发动机指示和机组告警 — N1 表、EGT 柱、燃油流量、状态面板 |
| **FMC** | 飞行管理计算机 — 航路编辑、航点管理、5 页 CDU 界面 |
| **UDP** | X-Plane 11 数据对接 — 支持 RPOS/DSEL 协议 |
| **Map** | 高德地图 API 接入 — 天气查询、位置展示（骨架） |
| **Data Structures** | 哈希表、双向链表、AVL 平衡树 |

---

## 技术栈

- **语言**: C (C11)
- **图形**: SDL2 + SDL2_image + SDL2_ttf + SDL2_gfx
- **编译**: MSYS2 / MinGW-w64 + GCC 15.x
- **网络**: UDP（Winsock2 / BSD sockets）
- **并发**: SDL 线程 + 互斥锁
- **编辑器**: VS Code

---

## 项目结构

```
MS/
├── Makefile
├── README.md
├── config/
│   └── default.cfg              # 默认配置文件
├── resources/
│   └── fonts/                    # TTF 字体文件目录
└── src/
    ├── main.c                    # 入口 (WinMain/main)
    ├── app.h / app.c             # 应用框架（窗口、主循环、模块编排）
    ├── config.h / config.c       # INI 配置解析器
    ├── event.h / event.c         # SDL 事件分发系统
    ├── thread.h / thread.c       # 线程管理 & 互斥辅助
    ├── net/
    │   ├── udp.h / udp.c         # UDP socket 封装
    │   └── xplane.h / xplane.c   # X-Plane 11 DATA 协议解析
    ├── instruments/
    │   ├── instrument.h          # Instrument 虚表接口
    │   ├── pfd.h / pfd.c         # 主飞行显示器
    │   ├── nd.h / nd.c           # 导航显示器
    │   ├── eicas.h / eicas.c     # 发动机/告警显示
    │   └── fmc.h / fmc.c         # 飞行管理计算机
    ├── data/
    │   ├── flight_data.h / .c    # 线程安全飞行数据容器
    │   └── navdata.h / .c        # 导航数据 & 航路计划
    ├── ds/
    │   ├── hash_table.h / .c     # 哈希表（字符串键）
    │   ├── linked_list.h / .c    # 双向链表
    │   └── avl_tree.h / .c       # AVL 平衡树
    ├── map/
    │   ├── http.h / http.c       # 最小 HTTP GET 客户端
    │   └── map_display.h / .c    # 高德地图集成
    └── utils/
        ├── logger.h / logger.c   # 日志系统（线程安全）
        ├── math_util.h / .c      # 数学 & SDL 绘图辅助
        └── file_io.h / file_io.c # 文件 I/O 工具
```

---

## 构建 & 运行

### 前提条件

1. 安装 [MSYS2](https://www.msys2.org/)
2. 打开 **MSYS2 MINGW64** 终端，安装依赖：

```bash
pacman -S mingw-w64-x86_64-gcc \
         mingw-w64-x86_64-SDL2 \
         mingw-w64-x86_64-SDL2_image \
         mingw-w64-x86_64-SDL2_ttf \
         mingw-w64-x86_64-SDL2_gfx \
         make
```

3. 将 MSYS2 的 `mingw64/bin` 添加到系统 PATH（或直接在 MINGW64 终端中操作）。

### 编译

```bash
cd E:/desktop/MS
make          # 发布构建
make debug    # 调试构建 (-g -O0)
make run      # 编译并运行
make clean    # 清理构建产物
```

### 运行

```bash
./cockpit.exe                    # 使用默认配置
./cockpit.exe config/my.cfg      # 使用自定义配置
```

---

## 配置说明

编辑 `config/default.cfg`：

```ini
[window]
width  = 1920
height = 1080
fullscreen = 0

[network]
xplane_host   = 127.0.0.1    # X-Plane 11 的 IP
udp_recv_port = 49000         # 本地接收端口
data_rate     = 20            # 数据请求频率 (Hz)

[instruments]
pfd_enabled   = 1             # 启用 PFD
nd_enabled    = 1             # 启用 ND
eicas_enabled = 1             # 启用 EICAS
fmc_enabled   = 1             # 启用 FMC

[map]
amap_api_key = YOUR_API_KEY   # 高德地图 Web API Key

[logging]
log_level = 1                 # 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR
```

---

## 架构要点

### 线程模型

```
Main Thread                  UDP Thread
────────────                 ──────────
SDL Event Loop               recvfrom() loop
  → Pump events                → Parse DATA packet
  → Snapshot FlightData        → Lock mutex
  → Update instruments         → Write FlightData
  → Render instruments         → Unlock mutex
  → Frame cap
```

- 飞行数据通过 **mutex 保护的单向流动** 实现线程安全
- UDP 线程不参与渲染, 仅负责数据写入
- 主线程仅短暂持锁做 memcpy 快照

### Instrument 接口

所有仪表实现统一虚表:

```c
typedef struct Instrument {
    const char* name;
    void (*on_init)(struct Instrument*, App*);
    void (*on_update)(struct Instrument*, const FlightData*, float dt);
    void (*on_render)(struct Instrument*, SDL_Renderer*);
    int  (*on_event)(struct Instrument*, const SDL_Event*);
    void (*on_destroy)(struct Instrument*);
    SDL_Rect rect;
    void*    private_data;
} Instrument;
```

新仪表只需实现这 6 个回调并在 `app.c` 中注册即可。

---

## 键盘快捷键

| 按键 | 功能 |
|------|------|
| `ESC` | 退出程序 |
| `F1-F5` | FMC 页面切换 (IDENT/RTE/LEGS/PERF/PROG) |
| `+/-` | ND 范围缩放 |
| `M` | ND 模式切换 (ROSE/ARC) |
| 字母/数字 | FMC 草稿栏输入 |
| `Enter` | FMC 执行 |
| `Backspace` | FMC 删除 |

---

## X-Plane 11 对接

1. 启动 X-Plane 11
2. **Settings → Net Connections → UDP Ports**
3. 设置发送端口为 `49000`（与 `udp_recv_port` 一致）
4. 或者在 X-Plane 中选择 "Send network data output"
5. 启动本程序即可接收实时飞行数据

如果没有 X-Plane 11，程序仍然正常运行，仪表显示默认零值。

---

## 许可证

本项目为大学小学期实训项目，仅供学习和教育用途。

---

🤖 项目骨架由 Claude Code 生成
>>>>>>> 394c697 (1)
