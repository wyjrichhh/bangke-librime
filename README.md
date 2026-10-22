# bangke-librime

蚌壳拼音使用的 Rime 引擎分支，基于 [rime/librime](https://github.com/rime/librime)。负责三平台共享的引擎构建、插件装配和工具链兼容。

Rime 的输入方案、拼写运算和通用 API 请参阅[上游文档](https://github.com/rime/librime#readme)。本页介绍本分支维护的差异。

## 本分支的扩展

- **独立插件接入**：AI 预测实现已拆到 [bangke-ai-predict](https://github.com/wyjrichhh/bangke-ai-predict)，通过 Rime 插件构建机制接入。`plugins/ai-predict` 是插件集成位置；模型推理、上下文构造和候选重排由插件负责。
- **Ubuntu 22.04 工具链兼容**：按 `CMAKE_SYSTEM_NAME` 识别 Linux，显式查找 Boost 1.74 的 regex、filesystem、system 组件；插件加载时将路径转为字符串，兼容旧版 Boost。Boost 的 Module / Config 查找模式由调用方选择，兼顾旧发行版和新 CMake。
- **glog 兼容调整**：移除已删除的 `SetLogSymlink` 调用，调整初始化逻辑，适配发行版提供的日志库接口。
- **Windows 源码编码**：MSVC 统一使用 `/utf-8`，避免中文注释和字符串受系统代码页影响。
- **发行构建与测试分开**：`make release` 启用外部插件、关闭合并插件和测试；测试构建需显式开启 `BUILD_TEST`。

## 与前端、AI 插件的分工

```text
平台前端 → librime 会话、方案与候选流水线 → ai_predict 插件 → CTranslate2 模型
平台前端 ← 候选列表与 Context 属性通知 ← 插件异步推理结果
```

`ai_predict_translator`、`ai_predict_filter` 以及 `_refresh_ui` 等属性约定的生产和消费分别在插件与前端实现。这里提供共享引擎和插件运行基础，集成方式见[插件 README](https://github.com/wyjrichhh/bangke-ai-predict#readme)。

## 构建与验证

递归获取依赖后构建引擎：

```bash
git submodule update --init --recursive
make deps
make release
```

需要 AI 时，先按插件文档将源码接入 `plugins/ai-predict` 并构建 CTranslate2 依赖；仅编译引擎不会自动下载模型或完成输入法配置。

测试需使用独立构建目录，并先准备所需依赖：

```bash
cmake -S . -B build-test -DBUILD_TEST=ON
cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

平台说明：[macOS](README-mac.md)、[Windows](README-windows.md)。平台前端的打包脚本还负责各自的插件、模型与运行库装配。

## 上游与许可

基础引擎来自 [rime/librime](https://github.com/rime/librime)，感谢上游及其依赖项目的贡献者。引擎采用 BSD-3-Clause，见 [LICENSE](LICENSE)；插件及依赖遵循各自许可。
