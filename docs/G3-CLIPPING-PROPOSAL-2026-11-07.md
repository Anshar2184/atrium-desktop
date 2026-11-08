# A08 · 可见裁剪方案与原型结果

日期：2026-11-07。对应 [质量审计 A08](QUALITY-AUDIT-2026-11-07.md#a08--p1设计契约--设置中心部分可见裁剪仍是折线轮廓)。

## 结论与范围

A08 **尚未修复**。当前产品继续保留原生 ImageBrush、AcrylicBrush、XAML 内容和焦点行为。有限折线几何裁剪的能力边界已在设计规范与源码注释中明确；未用增加折线密度、实色替换或更换材质宣称完成。

已有解析 CornerProfile 的 G3 端点论证成立；原生填充、独立玻璃、Widget 内容 mask、C# 实色/渐变及焦点绘制的解析覆盖率继续保留。待解决的是图片、原生 Acrylic 与 ClipChildren 实际使用的可见裁剪路径。

## 已实现并运行的独立对比

独立程序位于 Git 忽略的目录：

- [原型源码](../devshots/quality-audit-fixes/clip-prototype/App.xaml.cs)，直接链接产品 CornerProfile.cs 与基线路径 ContinuousCorners.cs。
- [原型工程](../devshots/quality-audit-fixes/clip-prototype/ClipProbe.csproj)，使用与产品一致的 WinUI 2.6.4、Win2D 1.4.0 和 DPI manifest。
- 可运行文件：devshots/quality-audit-fixes/clip-prototype-run/ClipProbe.exe。未加入开发分发包或发布包。

左侧使用产品当前的原生路径裁剪；右侧保留同配置的原生 Acrylic、活动 Image 和真实 Button/TextBox，将源子树映射为 CompositionVisualSurface，再用同一 CornerProfile 逐像素生成的 alpha 蒙版绘制。为避免原始方形内容外露，右侧源子树的父承载层裁为空。没有复制控件业务逻辑或改用图片按钮。

实际窗口观察结果：

| 项目 | 结果 |
| --- | --- |
| 解析蒙版接入 | 原型可绘制完整子树；数学边界由 CornerProfile.Coverage 生成 |
| Acrylic 材质 | **不通过**：左侧保留照片背景的模糊色彩，右侧下半部明显变为近白色；把子树放入离屏表面后，原生 Acrylic 的背景采样上下文发生变化 |
| 鼠标输入 | **不通过**：基线按钮实际点击后计数变为 1；候选按钮的同位置点击后仍为 0，且候选控件没有可用的 UI Automation 点击边界 |
| 焦点与可访问性 | 控件仍出现在可访问性树中，但上述命中失败已阻止它满足“保留 XAML 内容/焦点行为”的验收；没有宣称 Tab、下拉、IME 等全部通过 |
| DPI | 加入产品 DPI manifest 后复核，仍存在材质与命中差异 |
| 覆盖范围 | 此实验只排除该子树重定向路线；不证明 WinUI 的所有可能实现均不可行 |

对比由 computer-use 技能在真实 WinUI 窗口中观察，原型窗口已关闭。截图只用于确认材质和交互差异，不用于证明 G3 连续性。

## 后续实现必须满足的约束

1. 保持原生 Acrylic 的背景采样位置、系统材质策略及现有参数；不把离屏表面的白色结果当作正常材质。
2. 最终可见边界直接使用解析覆盖率；整数命中区域、视觉 mask 与 XAML 业务树分别处理。
3. 源 XAML 控件保留正常的命中、Tab 焦点、键盘、文本输入、模板状态与浮层锚定。不能靠隐藏源父层再复制画面交付。
4. ImageBrush 路径需覆盖 BitmapImage 与实时 WriteableBitmap，保留对齐、Stretch、换图及 420 ms 淡出。仅支持静态 URI 的路径不足以替换当前预览。
5. 在图片、Acrylic、下拉选项、颜色浮层、焦点框上分别比较；同时验证分数缩放、初载、主题和 mask 替换，记录实际离屏资源尺寸及字节。
6. 先给出符合这些条件的实际对比，再接入产品；否则保留现有行为并继续标记 A08 未闭合。

下一步的技术调查应围绕“在原背景采样上下文内接入 alpha 覆盖率”展开。需要更改原生材质实现或 XAML 承载结构时，先展示完整材质/输入对比供用户决定；本次没有授权改变其效果。

## 平台依据

Microsoft 的 [XAML 与 Visual Layer 互操作说明](https://learn.microsoft.com/en-us/windows/apps/develop/composition/using-the-visual-layer-with-xaml) 说明了 handout Visual 与 handin Visual 的边界，以及自绘 Visual 在可访问性方面的约束。[CompositionVisualSurface](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.ui.composition.compositionvisualsurface) 将一棵视觉树作为表面使用；这不构成对原背景采样与 XAML 命中行为等价的保证。[Composition brushes](https://learn.microsoft.com/en-us/windows/uwp/composition/composition-brushes) 区分了 XAML Brush、CompositionSurfaceBrush 与 CompositionMaskBrush。上述失败结论来自本次原型实测。
