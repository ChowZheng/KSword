# 内存工作台夹具：并行变异验证与构建的四个坑（wave1b 实测）

- 起因：给 HexCanvas 补接口（contentChanged / stageBytes / editableChanged 等）后，用 26 条人为缺陷做变异验证，
  每条要完整构建夹具（单条约 170 s、产物 40 MB）。第一次直接并行开 6 个，连踩四个坑。
- **环境变量是进程级的**：`ForEach-Object -Parallel` 里 `$env:MEMWB_OUT = ...` 会被各任务互相覆盖，
  moc 报 "Cannot create ...\other_mutation\out\moc\...: Permission denied"，**所有变异都假"被抓到"**（控制组也"失败"）。
  判据：对照组（未变异）必须先跑出 SURVIVED；把变量放进各自的 `cmd /c "set \"MEMWB_OUT=...\"&& call script"` 命令行里。
- **并行构建会把系统盘写满**（C: 剩 0 字节）：产物目录里的 Qt DLL/插件每份 ~30 MB，26 份加副本就是 1 GB 以上；
  症状是 `LNK1108 无法在 0x0 处写入文件`、`out-line: 磁盘空间不足`、结果文件缺汇总行却显示 CAUGHT。
  处理：每条变异跑完立刻删副本与产物、副本里的构建脚本不再复制 DLL（PATH 指向已部署的目录）、并行数压到 3。
  **verdict 里必须区分 NO_SUMMARY(INVALID)**：没有 "memwb_ui_tests: N checks" 汇总行的"失败"不是被测试抓到。
- **/W4 /WX 下"常量条件"与"未使用变量"都会让变异体编不过**：`if (true)` 触发 C4127，删掉某个检查后留下的局部变量触发 C4189。
  变异要写成非常量形式（`if (editable || changed)`、`|| (last < address)`），否则得到的是 COMPILE_ERROR 而不是"被抓到"。
- **msys 的 `grep -c $'\r'` 会数出总行数**（对 LF 文件也一样），别用它判断换行符；用 PowerShell 逐字节数 13 与 10。
  仓库里这批新文件全是 LF；`.cmd` 例外：LF-only 的批处理文件在内容长度变化后可能被 cmd 按 512 字节块错解
  （报一堆 "'xxx' is not recognized"，其实是 rem 里的中文被切碎），改成 CRLF 才稳。
- **QSettings 默认格式是注册表（HKCU）——同名夹具的所有进程共享**（wpJ6 第二轮复核修复时踩的）：并行跑变异重放时，
  各进程对 `sidebarVisible` 等键的读写互相覆盖，出现与被测代码无关的"随机"失败（甚至把本该 SURVIVED 的变异报成 CAUGHT）。
  处理：夹具 main() 里 `QSettings::setDefaultFormat(IniFormat)` + `setPath(..., applicationDirPath()+"/settings")`，启动先清空；
  这样每份副本各用各的 ini，也不再污染用户注册表。**凡是夹具里读写 QSettings 的，并行前先查这一条。**
- 另一个顺手记下的判据坑：用像素比较"不可读占位符 ≠ 真实问号"时，两者文字颜色本来就不同（禁用色 vs 正文色），
  直接比像素即使画的是同一个字形也"不同"，抓不到把占位符改回 `?` 的退化；要比"是不是字迹"的掩码（`InkDifference`）。
