# LetYourPhotoHaveTime
This program scans a folder, finds each file’s best “shot time” (from EXIF or the filename), fills missing times by interpolation, and then optionally syncs the file’s filesystem timestamps (and writes EXIF for photos) to match.

## Main logic of the program (English)

This tool reconstructs a reasonable “shot time” timeline for a folder of media files, then optionally syncs the filesystem timestamps (and photo EXIF) to that timeline.

### 1) Collect files

* You input a file or folder path.
* The program scans the directory (optionally recursive).
* It keeps files with supported extensions and records:

  * file path
  * `mtime` (last write time)
  * on Windows, also reads `ctime/wtime` via WinAPI.

### 2) Sort by modification time

* All files are sorted by `mtime` (tie-break by path for stable ordering).
* This sorted order becomes the “timeline” used for filling missing shot times.

### 3) Detect a “shot time” for each file (anchors)

For each file, it tries to obtain `shot` (anchor time) in this priority:

* Read photo metadata (EXIF: `DateTimeOriginal`, `DateTimeDigitized`, `Exif.Image.DateTime`, etc.)
* If missing and enabled: parse a timestamp from the filename (e.g., `Screenshot_20210211_203323`, `IMG_20210704_123305`, `image-2021-01-27-22_47_22-543`).
  Files that have `shot` become **anchors**.

### 4) Infer a target time for missing files (interpolation / one-sided fill)

For files with no `shot`, the program assigns a `target` time:

* If they lie between two anchors: **linear interpolation** between the two anchor times.
* If only one anchor exists on one side: **one-sided filling**, stepping by `+1s/+2s/...` (or `-1s/...`) to keep order and avoid duplicates.
* If anchor times are too close to spread across many files: fallback to **step-fill** (packing by seconds).

### 5) Deduplicate timestamps

* If multiple files end up with the same `target` second, the program automatically bumps later ones by `+1s/+2s/...` to ensure uniqueness and stable ordering.

### 6) Apply changes (optional)

* In **dry-run** mode: it prints the plan only.
* Otherwise:

  * optionally writes missing EXIF shot time for photos
  * optionally syncs filesystem timestamps (`ctime/mtime/wtime` on Windows) to the computed `target`.

### 7) Report

* For each file it prints:

  * computed `target`
  * the source/reason (metadata / filename / interpolated / one-sided fill / unique bump)
  * original filesystem times
  * whether changes were actually applied or skipped.
# Chinese Version

## 程序整体功能

给一堆照片/截图/视频文件（按“文件修改时间”排序），尽可能恢复出每个文件的“拍摄时间 target”，然后可选把**文件系统时间（创建/修改/写入时间）**同步到这个 target；对能写 EXIF 的图片还会补写 EXIF 里的拍摄时间。

---

## 主要逻辑流程（从输入到输出）

1. **收集文件**

* 你输入一个路径（文件或文件夹）
* 程序扫描目录（可选递归），筛选“支持的扩展名”
* 对每个文件记录：路径、`mtime`（last_write_time），Windows 下还读 `ctime/wtime`

2. **按修改时间排序**

* 把所有文件按 `mtime` 排序（同 `mtime` 再按路径名做稳定排序）
* 目的：让“缺拍摄时间的文件”可以在时间线里按顺序被插值补齐

3. **为每个文件尝试确定“拍摄时间 shot”（锚点 anchor）**
   依次尝试：

* **EXIF/元数据**（如 `DateTimeOriginal`、`DateTimeDigitized`、`Exif.Image.DateTime` 等）
* 如果元数据没有且你允许：**从文件名里解析时间戳**（比如 `Screenshot_20210211_203323...`、`IMG_20210704_123305`、`image-2021-01-27-22_47_22-543` 等）
  得到 `shot` 的文件就是 **锚点（anchor）**。

4. **给没有 shot 的文件推断 target（插值/填充）**
   把整个排序后的列表当成一条时间线：

* 如果某段缺失文件夹在两个锚点之间：
  → 用前后锚点时间做**线性插值**（平均分配到中间）
* 如果只有前锚点或只有后锚点：
  → 用“单侧填充”，按 `+1s/+2s...` 或 `-1s/-2s...` 推过去，避免所有文件同一秒
* 如果前后锚点太接近（间隔太小不够分配）：
  → 退化为“按秒挤进去”的 step-fill

5. **去重（避免同一秒时间戳冲突）**

* 如果插值/填充后出现多个文件 target 相同（同一秒），就自动给后面的加 `+1s/+2s...`，保证时间戳不会全挤在同一秒上

6. **执行写入（可选）**

* `Dry-run=Y`：只打印计划，不改文件
* 否则：

  * 可选：对图片**补写 EXIF 拍摄时间**（缺失才写）
  * 可选：把文件系统的 **ctime/mtime/wtime** 改成 target（Windows API）

7. **输出报告**

* 每个文件打印：目标 target、来源（metadata/filename/interpolated/only-prev/only-next/unique…）、原本的 mtime/ctime/wtime，以及是否实际写入/跳过
* 最后汇总：总文件数、锚点数、补全数、跳过数等

---

## 这个逻辑的核心设计点

* **用 mtime 排序当“时间线”**：因为没有 shot 的文件也需要有一个顺序，mtime 通常是最接近真实顺序的线索
* **锚点驱动**：只要有少量文件能读到 shot（EXIF 或文件名），就能把缺失的都“拉回到同一条真实时间轴”
* **插值 + 去重**：既能给缺失的补上合理的时间，又尽量避免大量文件出现同一秒导致排序混乱/后续软件识别异常
