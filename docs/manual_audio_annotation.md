# 本地音频标注器：操作、技术实现与研究边界

这是独立回听和人工标注工具，不是新的检测算法。它不读预测 CSV，不自动找峰，不生成“真实标签”，
不修改 WAV，不启动麦克风，不影响 C++/FFTW/CoreAudio 主链，也不参与 CPU/GPU 性能计时。

## 1. 打开与实际操作

macOS 在仓库中运行 `bash scripts/open_audio_annotator.sh`，或用浏览器打开
`tools/audio-annotator/index.html`。脚本优先打开已安装的 Chrome，否则用默认浏览器。
使用工具不需要 Node、Python、服务器、账户或联网。保留同目录的 HTML、CSS 和两个 JS 文件，
不要只把 HTML 单独复制出去。第一版支持 8–96 kHz、单声道、PCM16 little-endian RIFF/WAVE，
最大 128 MiB；请选择 `capture.wav`，而不是供正式回放的 `capture-analysis-f32.wav`。

1. 选择供回听的 WAV。核对文件名、原始采样率、实际时长和 SHA-256。
2. 核对计划基线秒数和 development/held-out 划分；首次载入默认 30 秒（短录音不超过其时长）、
   development。它们是人工协议，不是程序对录音内容的自动判断。
3. 在上方总览点击大致位置。下方可前移、后移、放大、缩小或输入定位秒数。
4. 在下方波形拖选一次敲击。也可以直接输入开始、结束秒数。纵向显示 ×4/×16/×64 能看清轻声，
   但会截掉超出纵轴范围的波峰，仅影响显示。
5. 回听选区；默认附带前后各 0.15 秒，可设 0–2 秒。附带的回听上下文不会加入标签。
   可循环或以 0.75×、0.5× 播放；慢放会改变音高，边界最终仍应结合原速确认。
6. 选择类别、边界置信度，填写标注人。单次敲击选择 `anomaly_impact`，工况固定为 `steady`。
   默认置信度为 medium，不自动声称高置信度。
7. 点“添加标签”。列表可编辑、删除、撤销、重做；最多保留 100 次撤销快照。
8. 定期“下载草稿 JSON”，确认浏览器实际保存了文件。恢复时先选择同一份 WAV，再导入草稿。
9. 全段复核后勾选“我已回听，确认基线仅有正常背景声”，下载标签 CSV，并同时保留草稿。

下载只包含已经点击“添加标签 / 保存修改”的记录。尚未提交的编辑框不是正式标签，下载前需先保存该条修改。

界面不限制或推荐事件数量。预先记下的敲击次数只能用于核对，不允许为达到某个数量补造标签。
空标签表用于人工确认没有目标事件的负样本；导出空表前有二次提示。

## 2. 标注规则要先固定

- start：第一次可辨识的目标声音变化，不是振幅最高处，也不是听到后按键的时刻。
- end：按实验预先约定的事件结束规则，例如该次声音衰减回背景附近。它是事件持续时间，
  **不是**人为追加的 ±50 ms 容差。评价容差由现有 C++ 评价器处理。
- 相邻敲击必须听清边界；不要为避开重叠检查而移动事实。难分开的事件先复听、记录不确定性，
  必要时按预先定义的 burst 类别处理，而不是看到预测后改变分类。
- 不清楚的边界标 medium/low；保存很多小数位不意味着人工具有同等精度。
- 先保存独立标签，再看检测器输出。不把预测导入标注界面；知道预测数量本身也可能产生偏差，
  自己标注时应在工程日志记录这一限制，正式研究优先采用不了解预测的独立标注人。

## 3. 时间与绘图实现

`core.js` 解析 RIFF 的 fmt/data chunk，并正确跳过未知 chunk 和奇数字节 padding；
拒绝截断、重复关键 chunk、未完成文件头、非单声道 PCM16 和不一致的格式字段。
它不假设所有 WAV 都只有 44 字节头部。

PCM16 解码为 `Float32Array`，样本值是 int16 / 32768。原始采样率、样本总数和持续时间不变。
内部选择区间和草稿保存整数 `startSample` / `endSample`（结束边界可等于样本总数），
时间统一是 `sampleIndex / sourceSampleRate`。不会减掉前 30 秒，缩放和慢放也不改变时间坐标。

Canvas 总览和局部图使用每个水平像素覆盖区间的 min/max 包络，包含区间内所有样本，避免点抽样
丢失短促脉冲；放大到足够细时每个桶只含一个样本。绘图包络按视窗缓存，播放游标更新时不重新
扫描整段样本。最小横向视窗是 10 ms；纵向放大只影响图像，音量单独控制。

Web Audio 的 AudioBuffer 按源采样率建立，直接复制原始解码样本，不用 `decodeAudioData()`
生成标注坐标。播放起止 offset/duration 始终使用源录音秒数；变速只影响回听速度。
浏览器/设备仍可能在实际播放输出阶段重采样。播放游标只用于听音导航，不能作为硬件时间戳。
相关 API 依据：[AudioBufferSourceNode.start](https://developer.mozilla.org/en-US/docs/Web/API/AudioBufferSourceNode/start)、
[createBuffer](https://developer.mozilla.org/en-US/docs/Web/API/BaseAudioContext/createBuffer)。

## 4. 保存、隐私与故障保护

- 页面由本地 HTML/CSS/JS 组成，无 CDN、遥测、服务器、fetch、上传端点或预测文件输入。
  CSP 显式使用 `connect-src 'none'`。所有文件都是用户主动选择读取。
- SHA-256 在浏览器内计算；草稿校验完整文件指纹、文件大小、采样率、样本数和 schema 版本。
  同名异内容会拒绝。指纹证明草稿对应这份文件，不证明事件标签或“正常基线”本身正确。
- 草稿包含原始文件名、音频指纹和人工标签，**仍是私有研究数据**。仓库忽略默认导出文件名；
  请将它们保存在 `data/local/`，不要手动强制添加到 Git。
- 不依赖 `file://` 下不一致的浏览器持久存储，不自动保存到云端。内存中的编辑关闭后会丢失；
  有未保存改动时尽可能提示，但崩溃/强制退出无法保证提示。定期下载草稿是正式保存步骤。
- 下载按钮只能发起下载，不能确认浏览器确实写入成功；页面会提醒人工核对下载文件。
- 时间区间检查包括负数、非整数采样点、空区间、越界、异常重叠和计划基线内异常。
  工况固定 steady；允许的四类标签与现有 C++ 读取器一致。
- 标注人限制为 1–40 字，以字母、汉字或数字开头，其余可用字母、汉字、数字、空格、点、下划线、
  连字符；禁止逗号、引号和换行，以兼容当前不支持 quoted CSV 的读取器，并避免公式前缀。
- 界面插入文件名和标签使用 `textContent`，不会把导入文字作为 HTML 执行。
- 最大文件 128 MiB 并不是所有设备的内存保证：解码、原文件、回听缓冲可同时驻留，长录音应另行规划。

## 5. 与现有评价器衔接

CSV 固定表头：

```text
start_seconds,end_seconds,class,operating_state,confidence,annotator
```

导出按开始时间排序，保留原始时间，不输出 sample ID 列。秒数用可往返的 double 文本表示，
避免把文件末尾边界四舍五入到 duration 之后而被 C++ 拒绝。

下载的 `hero-labels-<指纹前12位>.human-labels.csv` 需要人工核对后复制为对应 session 的
`human-labels.csv`。已有标签先备份；工具不覆盖项目文件。草稿同样保留，作为来源和协议记录。
后续按 [异常回放与评价协议](anomaly_replay_and_evaluation.md) 运行现有两个 CLI。

特别注意：

1. 计划基线“30秒”与评价器从 replay frame CSV 读取的实际 monitoring 起点可能有数毫秒差异。
   本工具不读取该 CSV。正式评价器仍执行最终检查；边界附近被拒绝的真实事件必须复核协议，
   不能移动标签以通过检查。
2. 界面的 split 保存于草稿，不会修改 evaluation manifest。运行评价前需确保 manifest 的 split
   与预注册划分一致；界面不是阻止 held-out 调参的数据管理系统。
3. 基线确认复选框不是采集完整性证明。原有 `anomaly-summary.json` 的 live 完整性检查仍然必须通过。
4. CSV 里的 confidence 当前只被校验和保留，不会自动排除低置信度标签或对 F1 加权。
5. 评价器按“预测点到标签区间”的距离匹配；过宽区间可能使匹配更宽松，因此持续时间规则必须一致。
6. replay 输出的 delay 是 `algorithm_only_replay`。浏览器播放、标注精度或这份工具不能证明真实
   麦克风到报警的端到端 P95；不能将这两种口径混用。

## 6. 开发测试（使用者无需执行）

无依赖核心测试：

```bash
node --test tools/audio-annotator/tests/core.test.cjs
node tools/audio-annotator/tests/cpp.test.cjs
```

第二条需要先构建 C++ evaluator，默认使用 `build/macos-arm64-release/hero-audio-anomaly-eval`，
也可通过 `HERO_ANOMALY_EVAL` 指定路径。只用生成的合成音频/标签验证读取兼容性，测试数据不是研究结果。

浏览器测试依赖 Node 24、pnpm 11.19.0 和锁定的 Playwright：

```bash
cd tools/audio-annotator
pnpm install --frozen-lockfile --ignore-scripts
pnpm exec playwright install chromium
pnpm run test:browser
```

`HERO_BROWSER_EXECUTABLE` 可指定已有 Chromium 系浏览器；`HERO_PLAYWRIGHT_MODULE` 可指定已安装的
Playwright 模块绝对路径。浏览器测试在独立测试进程中使用合成音频，不启动麦克风，不触及实际录音。
截图和导出检查文件位于忽略的 `build/annotator-browser/`。

自动验证包含：44.1/48/96 kHz 时间保持、异常 WAV 拒绝、min/max 包络完整性、缩放坐标一致、
标签边界/重叠/CSV 字符校验、草稿指纹、撤销重做、播放/循环/慢放、导出恢复、指针选区、窄屏布局、
没有网络请求，以及现有 C++ CLI 对导出标签的实际读取。

浏览器听感、真实敲击的人工边界、macOS 的设备输出延迟仍需要用户实测；自动测试不替代这些验证。
