# 第三方代码与许可证

本项目源码不包含第三方代码，也不捆绑第三方二进制或运行时。

程序使用 Microsoft Windows SDK 头文件和导入库，并在运行时调用 Windows 系统组件，包括 `winsqlite3.dll`、Win32、Common Controls、Shell 和 PSAPI。这些组件由 Windows 提供，不随便携包再分发。
