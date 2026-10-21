# aoi-LiteAnno

轻量的图片标注工具，标注文件与 X-AnyLabeling 的 JSON 格式兼容。面向工业检测和安防场景的标注人员：启动快、安装包小，软件内不加载模型。

## 当前状态

第二个里程碑进行中：单图编辑与可靠保存。已完成可靠保存部分，标注界面还没有开始做。

- `core`：标注文档模型（形状、图片级标签、确认与跳过状态）、项目配置、确认条件和派生状态（格式规范第 5 节），只依赖 Qt Core。
- `formats`：读写 X-AnyLabeling 的逐图 JSON。未知字段和无法识别的对象原样保留，未编辑的文件不写盘；写出的字节与 X-AnyLabeling 保存时（Python `json.dump(..., ensure_ascii=False, indent=2)`）一致。保存前比对文件内容，别的程序改过就不覆盖；替换失败时短暂重试。还负责项目文件和恢复记录的读写。
- `platform`：数据根目录所在卷的标识，用于计算 `bindingId`。
- `application`：保存流程（先写本机恢复记录，再替换标注文件，再删记录）、崩溃后的恢复、项目锁。
- `ui`：只有一个空窗口。

## 目录

| 目录 | 内容 |
| --- | --- |
| `src/core` | 文档模型，不依赖 Qt Gui 和 Widgets |
| `src/formats` | 标注文件读写与保存、项目文件、恢复记录、工作批次文件 |
| `src/platform` | 与操作系统有关的部分：卷标识 |
| `src/application` | 跨模块的流程：保存、恢复、项目锁 |
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

## 设计文档

架构说明和标注文件格式规范 1.0 目前放在项目的共享文档中。代码注释里的 “format spec section N” 指格式规范的第 N 节，“architecture section N” 指架构说明的第 N 节。
