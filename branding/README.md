# 更换程序 Logo

程序只用一个 Logo 文件：`branding/logo.png`。下面这些地方都由它生成：

- 窗口左上角和任务栏上的图标
- “帮助 > 关于”里的 Logo
- `LiteAnno.exe` 和 `liteanno-cli.exe` 在资源管理器里的文件图标（构建时自动生成 .ico）

## 怎么换

1. 准备一张 **PNG** 图片：
   - 最好是正方形，边长至少 256 像素，建议 512 像素
   - 背景可以是透明的
   - 不是正方形也可以，程序会把它居中放在透明的正方形里，不会拉伸变形
2. 用你的图片替换 `branding/logo.png`，文件名保持不变，然后提交并推送到 GitHub。
3. 在 GitHub 的 Actions 里运行 **Package** 工作流（Run workflow，分支选 main），下载新的 exe。

自己在本机编译时，也可以不改仓库，在配置时直接指定图片：

```
cmake --preset ... -DLITEANNO_LOGO=D:/my/logo.png
```

## 注意

- 必须是 `.png` 文件，否则配置时会报错停止。
- 小尺寸（16、24 像素）是从大图缩出来的，细线和小字会看不清，Logo 最好简单、对比鲜明。
- Windows 资源管理器会缓存图标。换了 Logo 后 exe 文件图标没变的话，把 exe 改个名字，或者重启资源管理器。
- `branding/logo.svg` 是默认 Logo 的矢量原稿，`src/ui/icons/render.sh` 用它生成默认的 `logo.png`。换成你自己的 Logo 时不需要它。
