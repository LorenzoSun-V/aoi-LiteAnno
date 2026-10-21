# aoi-LiteAnno

轻量的图片标注工具，标注文件与 X-AnyLabeling 的 JSON 格式兼容。面向工业检测和安防场景的标注人员：启动快、安装包小，软件内不加载模型。

## 当前状态

第二个里程碑进行中：单图编辑与可靠保存。可靠保存和单图编辑界面已完成，与真实 X-AnyLabeling 文件的往返验证还没做。

- `core`：标注文档模型（形状、图片级标签、确认与跳过状态）、项目配置、确认条件和派生状态（格式规范第 5 节），只依赖 Qt Core。
- `formats`：读写 X-AnyLabeling 的逐图 JSON。未知字段和无法识别的对象原样保留，未编辑的文件不写盘；写出的字节与 X-AnyLabeling 保存时（Python `json.dump(..., ensure_ascii=False, indent=2)`）一致。保存前比对文件内容，别的程序改过就不覆盖；替换失败时短暂重试。还负责项目文件和恢复记录的读写。
- `platform`：数据根目录所在卷的标识，用于计算 `bindingId`。
- `application`：保存流程（先写本机恢复记录，再替换标注文件，再删记录）、崩溃后的恢复、项目锁、打开项目、单图编辑会话（撤销和重做含确认与跳过状态）、后台保存（全局待保存上限，到上限时切图等待）。
- `dataset`：扫描数据根目录下的图片，自然排序，标出主文件名冲突和不支持的格式。
- `imaging`：读图（按 EXIF 方向摆正，高位深图片先简单转成 8 位显示）。
- `ui`：编辑窗口。打开项目（选数据根目录，或把目录作为命令行参数），画和改矩形、多边形，改类别，图片级标签，确认、确认无目标、跳过（必须填原因）、取消跳过，切图和停顿时自动保存，外部修改和恢复记录的提示。

快捷键与 X-AnyLabeling 4.2.0 的默认设置一致：A / D 上一张、下一张；Ctrl+J 选择，R 矩形，P 多边形；画多边形时点回第一个点、双击或回车结束，Esc 取消，退格或 Ctrl+Z 删最后一个点；双击形状或 Ctrl+E 改类别；Delete 删除；方向键把选中的形状移动 1 像素；Ctrl+Alt+K 确认当前图片；Ctrl+Z 撤销，Ctrl+Shift+Z 重做（X-AnyLabeling 没有重做，Ctrl+Y 留给它的同名功能）；Ctrl+S 立即保存；Ctrl+U 打开项目；Ctrl+Q 退出；Ctrl++ / Ctrl+- 放大缩小，Ctrl+= 原始大小，Ctrl+F 适合窗口；Ctrl + 滚轮缩放，滚轮滚动；空格加左键、右键或中键拖动平移。

## 目录

| 目录 | 内容 |
| --- | --- |
| `src/core` | 文档模型，不依赖 Qt Gui 和 Widgets |
| `src/formats` | 标注文件读写与保存、项目文件、恢复记录、工作批次文件 |
| `src/platform` | 与操作系统有关的部分：卷标识 |
| `src/application` | 跨模块的流程：打开项目、编辑会话、保存、后台保存、恢复、项目锁 |
| `src/dataset` | 图片目录扫描 |
| `src/imaging` | 读图（只在构建界面时编译） |
| `src/ui` | 桌面程序 |
| `tests` | Qt Test 单元测试、测试数据、与 Python json 模块的字节对照 |
| `third_party` | 随仓库附带的第三方代码：nlohmann/json 3.12.0（MIT） |
| `cmake` | 工具链版本检查、模块依赖规则检查 |

## 构建

发布和 CI 只用一套固定的工具链（见 `cmake/LiteAnnoToolchainPin.cmake`）：

- Qt 6.8.3
- Windows：Visual Studio 2022 17.14（MSVC 工具集 v143，编译器 cl.exe 19.44.35229），x64。Visual Studio 小版本更新换了编译器也算升级，需要改 `cmake/LiteAnnoToolchainPin.cmake` 并重新在最低系统上验收
- CMake 3.22 或更高；运行测试还需要 Python 3

Windows，在装有 Visual Studio 2022 的机器上：

```
cmake --preset windows-msvc -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

Linux，用 GCC 和 Ninja：

```
cmake --preset linux-gcc -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/gcc_64
cmake --build --preset linux-gcc
ctest --preset linux-gcc
```

找到的 Qt 版本或 MSVC 编译器版本与固定版本不一致时，配置会直接报错。本地开发如果只有别的 Qt 6（6.4 或更高，例如 Linux 发行版自带的），可以用 `dev` 预设，它关掉这项检查，构建产物不能用于发布：

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

`ctest` 会运行全部单元测试、与 Python json 模块的逐字节对照，以及程序启动检查。没有 Python 3 时可以加 `-DLITEANNO_SKIP_PYTHON_TESTS=ON` 跳过字节对照。

与 X-AnyLabeling 的往返验证（格式规范第 11 节）需要装有固定版本 X-AnyLabeling 的 Python，Linux CI 里会自动运行：

```
python3 -m venv ~/xal && ~/xal/bin/pip install "x-anylabeling-cvhub==4.2.0"
cmake --preset linux-gcc -DLITEANNO_UPSTREAM_PYTHON=$HOME/xal/bin/python
```

样本包括：X-AnyLabeling 4.2.0 自己保存的各类形状文件、上游仓库里 2.4.0 写的示例（按固定版本下载并校验摘要）、仓库里的上游格式样本。每份样本先由 X-AnyLabeling 读取，再由本程序读入（不编辑时不写盘）、改类别、加矩形、加图片级标签、确认后写出，然后用 X-AnyLabeling 读回逐项比对，最后让 X-AnyLabeling 再存一次，本程序读回。现场标注文件不放进仓库，可以用 `tests/python/upstream_compat.py ... --field-dir <目录>` 在本地跑。

## 设计文档

架构说明和标注文件格式规范 1.0 目前放在项目的共享文档中。代码注释里的 “format spec section N” 指格式规范的第 N 节，“architecture section N” 指架构说明的第 N 节。
