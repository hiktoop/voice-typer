# voice-typer

**Linux 桌面全局语音输入**：按一次快捷键说话，文字直接出现在任意应用的输入框里。
全程本地推理，音频不出本机。

基于 [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx)（Apache-2.0），
默认模型为中文识别 [Paraformer-zh](https://huggingface.co/csukuangfj/sherpa-onnx-paraformer-zh-2023-09-14)。

```
Win+Space → 木击声 → 🎤 正在听 → 说话 → 低音 → 文字出现在光标处
```

---

## 特性

- **全局可用**——终端、编辑器、浏览器、聊天软件，凡是有光标的地方都行
- **完全离线**——Paraformer 本地 CPU 推理，无任何网络请求
- **按两次结束**——不靠静音判断，第二次按下立刻出字，无等待
- **固定响度提示音**——不论系统音量调多少，响度恒定；系统静音时自动不响
- **中文标点**——挂 CT-Transformer 后处理，自动补逗号句号
- **零重型依赖**——不需要 xdotool、不需要 X11 开发包、不需要 Python

---

## 系统要求

| 项目 | 要求 |
|---|---|
| 显示服务器 | **X11**（Wayland 原生会话下 XTEST 不可用） |
| 音频 | PulseAudio 或 PipeWire（提供 `parecord` / `paplay` / `pactl`） |
| 桌面 | GNOME（用 `gsettings` 绑定全局快捷键） |
| 运行时 | Node.js ≥ 20（开发时使用 v24） |
| 编译 | `gcc`（仅用于编译注入用的 C 程序） |
| 系统库 | `libX11.so.6`、`libXtst.so.6`（发行版默认自带，无需 `-dev` 包） |

---

## 快速开始

### 1. 编译注入程序

```bash
cd ~/voice-typer
gcc -O2 -Wall -o bin/xtype    src/xtype.c    -ldl
gcc -O2 -Wall -o bin/xpaste   src/xpaste.c   -ldl
gcc -O2 -Wall -o bin/xfocus   src/xfocus.c   -ldl
gcc -O2 -Wall -o bin/xgetclip src/xgetclip.c -ldl
```

### 2. 安装 Node 依赖

```bash
npm install          # 安装 sherpa-onnx-node
```

> 若本机已装有 DSH（DeepSeek Harness），可直接软链接复用它的依赖：
> `ln -sfn "$(npm root -g)/@deepseek-ai/dsh/node_modules" node_modules`

### 3. 下载模型

```bash
mkdir -p models/paraformer models/punct models/silero

# 中文识别模型（232MB，精度优先）
curl -L -o models/paraformer/model.int8.onnx \
  https://huggingface.co/csukuangfj/sherpa-onnx-paraformer-zh-2023-09-14/resolve/main/model.int8.onnx
curl -L -o models/paraformer/tokens.txt \
  https://huggingface.co/csukuangfj/sherpa-onnx-paraformer-zh-2023-09-14/resolve/main/tokens.txt

# 标点模型（72MB int8）
curl -L -o models/punct/model.int8.onnx \
  https://huggingface.co/ranger810/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/resolve/main/model.int8.onnx

# VAD 模型（1.8MB，语音端点检测）
curl -L -o models/silero/silero_vad.onnx \
  https://github.com/snakers4/silero-vad/raw/master/src/silero_vad/data/silero_vad.onnx
```

嫌慢可以用镜像：把 `huggingface.co` 换成 `hf-mirror.com`。

更快的替代模型（78MB，精度略低）：把 URL 里的
`sherpa-onnx-paraformer-zh-2023-09-14` 换成 `sherpa-onnx-paraformer-zh-small-2024-03-09`，
目录换成 `models/paraformer-small`，并设 `VOICE_TYPER_MODEL=paraformer-small`。

### 4. 启动

```bash
./bin/voice daemon                                        # 前台（调试看日志）
setsid nohup ./bin/voice daemon > /tmp/vt-daemon.log 2>&1 < /dev/null &   # 后台
```

首次启动会加载模型，约 2–5 秒。

### 5. 绑定快捷键

```bash
./bin/voice install                     # 默认 Win+Space
./bin/voice install 'F9'                # 改成单键 F9
./bin/voice install '<Primary><Alt>v'   # 改成 Ctrl+Alt+V
```

---

## 命令参考

```bash
./bin/voice                # 切换：未录音则开始，录音中则停止并出字
./bin/voice start          # 开始录音
./bin/voice stop           # 停止、识别、注入
./bin/voice status         # 查看状态（JSON）
./bin/voice quit           # 关闭守护进程
./bin/voice daemon         # 前台运行守护进程

./bin/voice inject "文字"   # 直接把文字注入光标处（排错用）
./bin/voice install [键位]  # 安装 GNOME 全局快捷键
./bin/voice uninstall      # 移除快捷键

./bin/try-sounds           # 试听所有开始音候选
./bin/try-stop-sounds      # 试听所有结束音候选
```

单独测试各组件：

```bash
./bin/xtype "测试中文 abc123"     # 逐字符注入（Unicode 重映射）
./bin/xpaste "测试中文 abc123"    # 剪贴板粘贴注入
./bin/xgetclip                    # 读当前剪贴板内容
./bin/xfocus                      # 打印当前焦点窗口 ID
./bin/xfocus 0x1234567            # 把焦点切到指定窗口
```

---

## 配置项（环境变量）

所有配置都通过环境变量，没有配置文件。

| 变量 | 默认 | 说明 |
|---|---|---|
| `VOICE_TYPER_MODEL` | `paraformer` | 识别模型：`paraformer` / `paraformer-small` / `sensevoice` |
| `VOICE_TYPER_PUNCT` | `1` | 设为 `0` 关闭标点后处理 |
| `VOICE_TYPER_LANG` | `auto` | 语言提示（仅 sensevoice 有效）：`zh` / `en` / `ja` / `ko` / `yue` |
| `VOICE_TYPER_INJECT` | `paste` | 注入方式：`paste`（剪贴板，推荐）/ `type`（逐字符） |
| `VOICE_TYPER_TERMINAL` | `0` | 设为 `1` 改用 `Ctrl+Shift+V`（终端里粘贴） |
| `VOICE_TYPER_BEEP` | `1` | 设为 `0` 关闭提示音 |
| `VOICE_TYPER_BEEP_LEVEL` | `11` | 提示音**绝对响度**（百分比），详见下文 |
| `VOICE_TYPER_NOTIFY` | `1` | 设为 `0` 关闭所有通知弹窗 |
| `VOICE_TYPER_SILENCE_MS` | `0` | 设为正数则改为"说完静音 N 毫秒自动停止" |
| `VOICE_TYPER_MAX_SECONDS` | `120` | 单次录音上限（秒） |
| `VOICE_TYPER_KILL_GRACE_MS` | `250` | 停止时等待录音进程退出的宽限 |
| `VOICE_TYPER_ONSET_PADDING` | `0.3` | 首段前置补齐秒数，防止首字被截断 |
| `VOICE_TYPER_SOCK` | `~/.voice-typer.sock` | 控制套接字路径 |
| `VOICE_TYPER_MODELS` | `~/.dsh/speech-to-text/sensevoice/models` | sensevoice 模型目录 |

### 常用组合示例

```bash
# 省资源模式（small 模型 + 无提示音 + 无通知）
VOICE_TYPER_MODEL=paraformer-small VOICE_TYPER_BEEP=0 VOICE_TYPER_NOTIFY=0 \
  setsid nohup ./bin/voice daemon > /tmp/vt-daemon.log 2>&1 &

# 终端专用（Ctrl+Shift+V 粘贴）
VOICE_TYPER_TERMINAL=1 setsid nohup ./bin/voice daemon > /tmp/vt-daemon.log 2>&1 &

# 说完自动停（免按第二次）
VOICE_TYPER_SILENCE_MS=900 setsid nohup ./bin/voice daemon > /tmp/vt-daemon.log 2>&1 &

# 更轻的提示音
VOICE_TYPER_BEEP_LEVEL=6 setsid nohup ./bin/voice daemon > /tmp/vt-daemon.log 2>&1 &
```

---

## 提示音采用固定绝对响度

系统音量滑块调到多少都不影响听到的响度——只有系统**静音**时提示音才不响。

原理：`paplay` 的流音量上限被截在 100%（实测传 200%、400% 都被压回 100%），
所以反过来按 sink 音量做补偿：

```
流音量 = 目标响度 / 系统音量
实际响度 ≈ 目标响度
```

验证过的对照表（目标 11%）：

| 系统音量 | 流音量 | 实际响度 |
|---|---|---|
| 5% | 100%（封顶） | 5% |
| 11% | 100% | 11% |
| 22% | 50% | 11% |
| 50% | 22% | 11% |
| 100% | 11% | 11% |

系统音量 ≥ 目标值时响度**精确恒定**；低于目标值时受物理上限限制，
只能做到系统音量那么大（无法凭空放大）。

---

## 模型

| `VOICE_TYPER_MODEL` | 体积 | 加载 | 解码 2.7s 音频 | 特点 |
|---|---|---|---|---|
| `paraformer`（默认） | 232 MB | ~3s | ~214 ms | 中文精度最高 |
| `paraformer-small` | 78 MB | ~3s | ~78 ms | 更快，精度略低 |
| `sensevoice` | 239 MB | ~1s | ~90 ms | 多语种，内置标点 |

内存占用（含 Node 基线约 57 MB）：

| 组合 | 实测 RSS |
|---|---|
| paraformer 单独 | ~381 MB |
| paraformer + 标点模型 | ~462 MB |
| paraformer-small 单独 | ~200 MB |

标点模型是 **CT-Transformer (zh-en)**，纯文本后处理，推理 5–12 ms，
与语音识别质量无关，可随时用 `VOICE_TYPER_PUNCT=0` 关闭。

---

## 快捷键

```bash
./bin/voice install                     # Win+Space（默认）
./bin/voice install 'F9'                # F9
./bin/voice install '<Primary><Alt>v'   # Ctrl+Alt+V
./bin/voice install '<Super><Shift>q'   # Win+Shift+Q
./bin/voice uninstall                   # 移除
```

**注意**：`Win+Space` 是 GNOME 默认的输入源切换键。`voice install` 会先清空
`switch-input-source` 再绑定；若你确实需要切换键盘布局，
请改用 `F9` 之类的键，并自行还原：

```bash
gsettings set org.gnome.desktop.wm.keybindings switch-input-source "['<Super>space', 'XF86Keyboard']"
```

GNOME 无法绑定**纯修饰键**组合（只按 Ctrl+Win 不触发），必须带一个普通键。

---

## 开机自启

```bash
mkdir -p ~/.config/autostart
cat > ~/.config/autostart/voice-typer.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=Voice Typer
Comment=本地语音输入守护进程
Exec=/home/hisper/voice-typer/bin/voice daemon
Terminal=false
NoDisplay=true
X-GNOME-Autostart-enabled=true
EOF
```

把 `Exec` 改成你的实际路径。

---

## 工作原理

```
快捷键 → 守护进程 → parecord 录音 → VAD 分段 → Paraformer 识别
       → CT-Transformer 加标点 → xpaste 注入 → 光标处
```

三个关键实现：

**1. 文本注入用剪贴板，不用逐字符模拟**
`xpaste` 先取得 X11 `CLIPBOARD` 所有权，再模拟 `Ctrl+V`，应用来取数据时
一次性交出整段 UTF-8 文本。逐字符重映射（`xtype`）会被输入法或应用的
键盘映射缓存搞乱字符顺序，所以只作为备用。

**2. 没有 xdotool / 没有 X11 开发包**
所有 C 程序都通过 `dlopen` 在运行时解析 `libX11.so.6` / `libXtst.so.6`，
手写 `XSelectionRequest`/`XSelectionNotify` 事件结构，不依赖任何 `-dev` 包。

**3. 标点在识别之后单独加**
Paraformer 本身不产出标点。各段识别结果先拼接成完整文本，
再统一送入 CT-Transformer，跨段衔接更自然。

---

## 排错

```bash
./bin/voice status                        # 守护进程是否活着
tail -f /tmp/vt-daemon.log                # 守护进程日志
echo $DISPLAY                             # 必须是 X11 显示（如 :0）
./bin/xfocus                              # 当前焦点窗口 ID

./bin/voice inject "测试"                  # 文字能进输入框吗？
./bin/xpaste "测试"                        # 换一种注入方式试试
./bin/xgetclip                            # 剪贴板里有内容吗？
```

**文字没进输入框**——先看日志里的 `[focus] 注入目标` 那行，
确认当时的焦点窗口是不是你要输入的那个。`Ctrl+V` 只发给当前焦点窗口。

**终端里粘贴无效**——终端用 `Ctrl+Shift+V`，设 `VOICE_TYPER_TERMINAL=1`。

**识别结果错乱**——说明走的是 `type` 模式，改回默认的 `paste`。

**首字被吃掉**——调大 `VOICE_TYPER_ONSET_PADDING`（如 `0.5`）。

---

## 卸载

```bash
./bin/voice uninstall          # 移除快捷键（要在删目录前执行）
./bin/voice quit               # 关闭守护进程
rm -f ~/.config/autostart/voice-typer.desktop
rm -rf ~/voice-typer
```

---

## 目录结构

```
voice-typer/
├── bin/                    # 可执行文件
│   ├── voice               # CLI 入口
│   ├── xtype               # 逐字符 Unicode 注入（备用）
│   ├── xpaste              # 剪贴板粘贴注入（默认）
│   ├── xfocus              # 焦点读写
│   ├── xgetclip            # 读取剪贴板
│   ├── try-sounds          # 试听开始音
│   └── try-stop-sounds     # 试听结束音
├── src/                    # 源码
│   ├── voice-typer.mjs     # 守护进程 + 客户端
│   ├── xpaste.c            # 剪贴板注入
│   ├── xtype.c             # XTEST 逐字符注入
│   ├── xfocus.c
│   └── xgetclip.c
├── assets/                 # 提示音
│   ├── start.wav
│   ├── stop.wav
│   ├── candidates/         # 开始音候选
│   └── candidates-stop/    # 结束音候选
└── models/                 # 模型（不入库）
```

运行时的临时文件：控制套接字 `~/.voice-typer.sock`，日志 `/tmp/vt-daemon.log`。

---

## 致谢与许可

本项目的代码为原创，依赖以下开源组件：

| 组件 | 提供方 | 许可证 |
|---|---|---|
| [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) | k2-fsa / Next-gen Kaldi 团队 | Apache-2.0 |
| ONNX Runtime | Microsoft | MIT |
| Paraformer-zh | 阿里巴巴达摩院 FunASR | Apache-2.0 |
| CT-Transformer 标点 | 阿里巴巴达摩院 FunASR | Apache-2.0 |
| Silero VAD | Silero 团队 | MIT |

模型文件体积较大，不随仓库分发，请按上文「下载模型」自行获取。

## License

MIT
