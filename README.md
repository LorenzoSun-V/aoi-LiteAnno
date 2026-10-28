# aoi-LiteAnno

轻量的图片标注工具，标注文件与 X-AnyLabeling 的 JSON 格式兼容。面向工业检测和安防场景的标注人员：启动快、安装包小，软件内不加载模型。

使用说明（标注人员、现场部署人员、算法工程师）见 [docs/user-manual.md](docs/user-manual.md)。每次更新的变化见 [CHANGELOG.md](CHANGELOG.md)。

## 当前状态

主要标注流程、目录索引、固定批次、导入导出和姿态功能均已实现，当前处于发布前验收阶段。

交付目标为 Windows 10 1809 及以上、Windows 11，64 位。Linux 提供源码构建和 CI 验证。

### 已实现功能

- **形状标注**：矩形、多边形、旋转框、点、线段、折线、圆；支持移动、改类别、旋转、复制粘贴、隐藏，以及撤销重做。矩形可直接输入位置和尺寸。
- **姿态与关键点**：框内画点自动同组，组号可编辑；选中关键点可标记“遮挡”。YOLO 姿态和 COCO 关键点使用 YAML 配置类别与关键点顺序。
- **图片级分类**：支持项目配置的单选、多选标签。
- **复核流程**：逐图确认；形状任务支持确认无目标；跳过需要填写原因，内容修改后需要重新确认。
- **普通文件夹与项目**：没有项目文件也能直接标注；项目可配置类别和工具，按固定批次标注、续做和查看未完成项。
- **保存与恢复**：自动保存、原子替换、本机恢复记录、外部修改检测，以及本机同一用户的窗口和 CLI 共用的文件夹锁。确认和跳过状态随撤销重做恢复。
- **目录索引**：后台扫描、自然排序、图片与标注配对、名称冲突检查；再次打开时先读取本机索引，后台核对变化。
- **格式兼容**：保留已有内嵌图片数据、未知字段和无法识别的对象；未编辑的文件不写盘。无法识别的对象会阻止确认和导出，避免静默丢弃。

### 导入与导出

| 格式 | 导入 | 导出 |
| --- | --- | --- |
| YOLO 检测、分割、旋转框、姿态 | ✓ | ✓ |
| VOC 矩形、多边形 | ✓ | ✓ |
| COCO 检测、实例分割、关键点 | ✓ | ✓ |
| DOTA | ✓ | ✓ |
| 掩膜 PNG | ✓ | ✓ |
| LabelMe | — | ✓ |
| 图片级分类 | — | ✓ |

类别编号、姿态可见性、导入追加/替换规则及与 X-AnyLabeling 的有意差异，见使用手册的[导出](docs/user-manual.md#16-导出)、[导入](docs/user-manual.md#17-导入)和[姿态配置](docs/user-manual.md#18-姿态关键点配置文件)。

### 验证范围

Linux、Windows CI 覆盖自动测试；Linux CI 还运行 X-AnyLabeling 4.2.0 的标注往返及导入导出对照。目录索引包含 5000 张、20 万张合成数据的性能检查，具体结果见各次 CI 运行摘要。

发布前仍需在目标 Windows 电脑验收真实图片、保存恢复、导入导出和图标显示；机械硬盘、U 盘及断电场景的现场验收结果尚未记录。高位深图片目前使用基础的 8 位显示，尚无窗宽窗位调节。

## 开始使用

1. 从 [Package 工作流](https://github.com/LorenzoSun-V/aoi-LiteAnno/actions/workflows/package.yml)获取测试包；tag 发布的版本可在 [Releases](https://github.com/LorenzoSun-V/aoi-LiteAnno/releases) 下载。测试包只提供 `LiteAnno.exe`，不用安装 Qt 或 VC 运行库。
2. 双击运行，用 Ctrl+U 打开图片文件夹。普通文件夹可以自由填写类别；有 `liteanno/project.json` 时按项目配置标注。
3. 选择工具画形状，检查后按 **F 确认并下一张**；无法处理时按 **E 跳过**并填写原因。A / D 只翻页，F1 查看全部快捷键。
4. 标注会自动保存，也可按 Ctrl+S。导出入口在“文件 > 导出…”；项目导出已确认的图片。普通文件夹的形状标注无需先确认，仍需通过几何和格式检查。

每图标注 JSON 保存在图片旁边。本机索引可删除重建；**恢复记录可能是未写入编辑的唯一副本，不能当作缓存清理**，也不会随 U 盘或数据文件夹迁移。

详细操作和错误处理见[使用手册](docs/user-manual.md)。工程师可使用 CLI 的 `init`、`status`、`import`、`export`；CLI 不随测试包发出，也不提供批量确认。

## 目录

| 目录 | 内容 |
| --- | --- |
| `src/core` | 文档模型，不依赖 Qt Gui 和 Widgets |
| `src/formats` | 标注文件读写与保存、项目与批次文件、恢复记录、导入导出和姿态配置 |
| `src/platform` | 与操作系统有关的部分：卷标识 |
| `src/application` | 跨模块的流程：打开项目、编辑、保存恢复、目录索引、批次、导入导出 |
| `src/dataset` | 图片目录扫描、本机目录索引 |
| `src/imaging` | 读图、由 Logo 生成 Windows 图标（只在构建界面时编译） |
| `src/ui` | 桌面程序 |
| `src/cli` | 工程师命令行工具 |
| `docs` | 使用手册 |
| `branding` | 程序 Logo：换成自己的 Logo 见 [branding/README.md](branding/README.md) |
| `tests` | Qt Test 单元测试、测试数据、与 Python json 模块的字节对照 |
| `third_party` | 随仓库附带的第三方代码：nlohmann/json 3.12.0（MIT）；构建时下载并校验摘要的 SQLite 3.54.1（公有领域） |
| `cmake` | 工具链版本检查、模块依赖规则检查、Logo 设置 |

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

构建时会从 sqlite.org 下载 SQLite 源码并校验 SHA-512。连不上时，`dev` 预设可以加 `-DLITEANNO_SYSTEM_SQLITE=ON` 改用系统自带的 SQLite（同样不能用于发布）。

`ctest` 会运行全部单元测试、与 Python json 模块的逐字节对照，以及程序启动检查。CI 还会用 5000 张和 20 万张生成的图片测目录索引的速度，结果在每次运行的摘要里；本地可以运行 `tests/liteanno_catalog_benchmark <放测试数据的目录> <张数>`。没有 Python 3 时可以加 `-DLITEANNO_SKIP_PYTHON_TESTS=ON` 跳过字节对照。

与 X-AnyLabeling 的往返验证（格式规范第 11 节）需要装有固定版本 X-AnyLabeling 的 Python，Linux CI 里会自动运行：

```
python3 -m venv ~/xal && ~/xal/bin/pip install "x-anylabeling-cvhub==4.2.0"
cmake --preset linux-gcc -DLITEANNO_UPSTREAM_PYTHON=$HOME/xal/bin/python
```

样本包括：X-AnyLabeling 4.2.0 自己保存的各类形状文件、上游仓库里 2.4.0 写的示例（按固定版本下载并校验摘要）、仓库里的上游格式样本。每份样本先由 X-AnyLabeling 读取，再由本程序读入（不编辑时不写盘）、改类别、加矩形、加图片级标签、确认后写出，然后用 X-AnyLabeling 读回逐项比对，最后让 X-AnyLabeling 再存一次，本程序读回。现场标注文件不放进仓库，可以用 `tests/python/upstream_compat.py ... --field-dir <目录>` 在本地跑。

配置上述 Python 后，CTest 还会运行导入、导出对照（包括姿态）；有意保留的行为差异见使用手册对应章节。

## 单文件版与 Qt 许可

测试版只发 `LiteAnno.exe`（标注窗口）一个文件，Qt 6.8.3 和 MSVC 运行库都静态链接在内，旁边不需要任何文件，可以放在任意文件夹或 U 盘里直接运行。命令行工具 `liteanno-cli.exe` 也这样编译和检查，但不随测试版发出，需要时按下面的步骤自己编译。打包流程见 `.github/workflows/package.yml`，静态 Qt 由 `.github/scripts/build-static-qt.ps1` 从 Qt 官方源码编译（只含 qtbase 和 qtimageformats，源码包校验 SHA-256）。

Qt 按 GNU LGPL 3.0 授权使用。窗口程序的“帮助 > 关于”、命令行的 `liteanno-cli --licenses` 和 `--license <名称>` 列出所用的第三方库和它们的许可证全文。LGPL 允许你换用自己修改过的 Qt 重新链接：本仓库就是程序的全部源码，在装有 Visual Studio 2022 的 Windows 上，用 PowerShell 按下面的步骤做，得到的就是同样的两个单文件程序（静态 Qt 不含 Qt Test，所以要关掉测试）：

```
# 1. VS 开发者环境（cl.exe、Ninja）
& "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64
# 2. 静态 Qt：下载并校验 qtbase、qtimageformats 6.8.3 源码，编译安装到 C:\qt-static，
#    许可证全文收集到 C:\qt-static\licenses。用改过的 Qt：把源码放在 C:\qt-src\qtbase-everywhere-src-6.8.3
#    和 C:\qt-src\qtimageformats-everywhere-src-6.8.3，再加 -UseUnpackedSources
.github/scripts/build-static-qt.ps1 -Version 6.8.3 -Prefix C:\qt-static -WorkDir C:\qt-src
# 3. 程序附带的许可证：Qt 的，加上本仓库第三方代码的
New-Item -ItemType Directory -Force C:\liteanno-licenses
Copy-Item C:\qt-static\licenses\* C:\liteanno-licenses
Copy-Item third_party/nlohmann/LICENSE.MIT C:\liteanno-licenses\nlohmann-json-LICENSE.MIT
Copy-Item third_party/opencv/LICENSE C:\liteanno-licenses\opencv-LICENSE
# 4. Qt 对话框的中文翻译 qtbase_zh_CN.qm 取自官方同版本 Qt（安装器装的 Qt 6.8.3 的 translations 文件夹里也有）
python -m pip install "aqtinstall==3.3.0"
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --archives qttranslations --outputdir C:\qt-translations
# 5. 配置、编译
cmake --preset windows-msvc -DLITEANNO_BUILD_TESTS=OFF -DCMAKE_PREFIX_PATH=C:/qt-static `
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
    -DLITEANNO_QT_TRANSLATION=C:/qt-translations/6.8.3/msvc2022_64/translations/qtbase_zh_CN.qm `
    -DLITEANNO_BUNDLED_LICENSES_DIR=C:/liteanno-licenses
cmake --build --preset windows-msvc
# 6. 检查：两个程序都应返回 0
build/windows-msvc/Release/liteanno-cli.exe --check-single-file
Start-Process -Wait -PassThru build/windows-msvc/Release/LiteAnno.exe --check-single-file
```

这些步骤和 CI 打包（`.github/workflows/package.yml`）做的一样。

## 设计文档

架构说明和标注文件格式规范 1.0 目前放在项目的共享文档中。代码注释里的 “format spec section N” 指格式规范的第 N 节，“architecture section N” 指架构说明的第 N 节。
