export interface FileApi {
  readText(options: {
    uri: string
    encoding?: string
    success: (data: { text: string }) => void
    fail: (data: unknown, code: number) => void
  }): void
  writeText(options: {
    uri: string
    text: string
    encoding?: string
    success: () => void
    fail: (data: unknown, code: number) => void
  }): void
  readArrayBuffer(options: {
    uri: string
    position?: number
    length?: number
    success: (data: { buffer: Uint8Array }) => void
    fail: (data: unknown, code: number) => void
  }): void
  writeArrayBuffer(options: {
    uri: string
    buffer: Uint8Array
    position?: number
    success: () => void
    fail: (data: unknown, code: number) => void
  }): void
  get(options: {
    uri: string
    success: (data: { length: number; type?: string }) => void
    fail: (data: unknown, code: number) => void
  }): void
  mkdir(options: {
    uri: string
    recursive?: boolean
    success: () => void
    fail: (data: unknown, code: number) => void
  }): void
  delete(options: {
    uri: string
    success: () => void
    fail: (data: unknown, code: number) => void
  }): void
}

/** Native filesystem root backing this Manager's app-scoped files URI. */
export const NATIVE_SHARED_PATH = "/data/quickapp/files/ng.lst.corona/"
export const QUICKAPP_FILES_URI = "internal://files/"

interface FileOperationError extends Error {
  code?: number
}

const MAPPINGS_URI = `${QUICKAPP_FILES_URI}mappings.tsv`
const RELOAD_URI = `${QUICKAPP_FILES_URI}reload.request`
const RESULT_URI = `${QUICKAPP_FILES_URI}reload.result`
const THEME_ROOT = `${NATIVE_SHARED_PATH}themes/`
const THEME_DIRECTORY_URI = `${QUICKAPP_FILES_URI}themes/current/app/settings/`
const THEME_FILE_URI = `${THEME_DIRECTORY_URI}launcher.bin`
const THEME_ASSET_URI = "/common/settings-launcher.bin"
const FONT_ASSET_NAME = "FusionPixel-12px-Proportional-zh-Hans-MiSans-Regular-subset.ttf"
const FONT_ASSET_URI = `/common/${FONT_ASSET_NAME}`
const FONT_SOURCES = [
  "/resource/font/MiSansF-Semibold.ttf",
  "/resource/font/MiSansF-Medium.ttf",
  "/resource/font/MiSansF-Demibold.ttf",
  "/resource/font/MiSans-Semibold.ttf",
  "/resource/font/MiSans-Regular-All.ttf",
  "/resource/font/MiSans-Medium.ttf",
  "/resource/font/MiSans-Medium-All.ttf",
  "/resource/font/MiSans-Demibold.ttf",
  "/resource/font/MiSans-Demibold-All.ttf",
  // .155 boot copies these families to /tmp and registers the copied path.
  "/tmp/MiSans-Regular.ttf",
  "/tmp/MiSans-Medium.ttf",
  "/tmp/MiSans-Demibold.ttf"
]
const FONT_GENERATION_ROOT_URI = `${QUICKAPP_FILES_URI}themes/font-generations/`
const FONT_GENERATION_COUNTER_URI = `${QUICKAPP_FILES_URI}font-generation.counter`
const FONT_ASSET_SIZE = 6285576
const FONT_CHUNK_SIZE = 32768
const TEST_SOURCE = "/resource/app/settings/"
const TEST_DESTINATION = `${THEME_ROOT}current/app/settings/`
const LEGACY_TEST_DESTINATION = "/data/canopus/themes/current/app/settings/"
const MAX_CONFIG_BYTES = 32768
const MAX_RULES = 64
const MAX_PATH_BYTES = 256
const RELOAD_PREFIX = "resource-hook-reload-v1\tng.lst.corona\t"

function fileError(operation: string, uri: string, code: number, data: unknown): FileOperationError {
  const error = new Error(`${operation} ${uri} 失败（code ${code}）：${String(data)}`) as FileOperationError
  error.code = code
  return error
}

function utf8Length(value: string): number {
  let size = 0
  for (let i = 0; i < value.length; i++) {
    const code = value.charCodeAt(i)
    if (code <= 0x7f) size += 1
    else if (code <= 0x7ff) size += 2
    else if (code >= 0xd800 && code <= 0xdbff && i + 1 < value.length &&
             value.charCodeAt(i + 1) >= 0xdc00 && value.charCodeAt(i + 1) <= 0xdfff) {
      size += 4
      i++
    } else size += 3
  }
  return size
}

function validMappingPath(path: string): boolean {
  if (!path || utf8Length(path) >= MAX_PATH_BYTES || path[0] !== "/") return false
  let segmentStart = 1
  for (let i = 1; i <= path.length; i++) {
    if (i < path.length) {
      const code = path.charCodeAt(i)
      const character = path[i]
      if (code < 32 || code === 127 || character === "\\" || character === ":") return false
    }
    if (i === path.length || path[i] === "/") {
      const segment = path.slice(segmentStart, i)
      if ((!segment && i < path.length) || segment === "." || segment === "..") return false
      segmentStart = i + 1
    }
  }
  return true
}

function updateThemeMapping(config: string, targetSource: string, targetDestination: string,
                            install: boolean, acceptedDestinations: string[], label: string,
                            allowThemeDestination = false): string {
  if (config.indexOf("\0") >= 0 || utf8Length(config) > MAX_CONFIG_BYTES)
    throw new Error("mappings.tsv 超过大小限制或包含 NUL")

  const lines = config.split(/\r?\n/)
  if (lines[lines.length - 1] === "") lines.pop()
  const output: string[] = []
  const sources = new Set<string>()
  let activeRules = 0
  let targetSeen = false

  for (const line of lines) {
    if (!line || line[0] === "#") {
      output.push(line)
      continue
    }
    const separator = line.indexOf("\t")
    if (separator <= 0 || separator === line.length - 1 ||
        line.indexOf("\t", separator + 1) >= 0) throw new Error("mappings.tsv 格式无效")

    const source = line.slice(0, separator)
    const destination = line.slice(separator + 1)
    if (!validMappingPath(source) || !validMappingPath(destination) ||
        source.endsWith("/") !== destination.endsWith("/"))
      throw new Error("mappings.tsv 包含无效路径或源/目标类型不匹配")
    if (sources.has(source)) throw new Error(`mappings.tsv 存在重复源路径：${source}`)
    sources.add(source)

    if (source === targetSource) {
      targetSeen = true
      const accepted = destination === targetDestination ||
        acceptedDestinations.indexOf(destination) >= 0 ||
        (allowThemeDestination && destination.startsWith(THEME_ROOT))
      if (!accepted) throw new Error(`${label}源路径已映射到其他位置，已保留原配置`)
      if (install) {
        output.push(`${targetSource}\t${targetDestination}`)
        activeRules++
      }
      continue
    }

    if (!destination.startsWith(THEME_ROOT))
      throw new Error(`映射目标必须位于 ${THEME_ROOT}`)
    output.push(line)
    activeRules++
  }

  if (install && !targetSeen) {
    output.push(`${targetSource}\t${targetDestination}`)
    activeRules++
  }
  if (activeRules > MAX_RULES) throw new Error(`映射规则不能超过 ${MAX_RULES} 条`)

  if (!activeRules && !output.some(line => line.trim().length > 0))
    return "# No active theme mappings.\n"
  const result = output.join("\n") + (output.length ? "\n" : "")
  if (utf8Length(result) > MAX_CONFIG_BYTES) throw new Error("mappings.tsv 超过 32 KiB")
  return result
}

function updateTestMapping(config: string, install: boolean): string {
  return updateThemeMapping(config, TEST_SOURCE, TEST_DESTINATION, install,
                            [LEGACY_TEST_DESTINATION], "测试图标主题")
}

function updateFontMappings(config: string, install: boolean, destination: string): string {
  let result = config
  for (const source of FONT_SOURCES)
    result = updateThemeMapping(result, source, destination, install, [], "固件字体", true)
  return result
}

function hasBroaderDirectoryMapping(config: string, exactSources: string[]): boolean {
  const lines = config.split(/\r?\n/)
  for (const line of lines) {
    if (!line || line[0] === "#") continue
    const separator = line.indexOf("\t")
    if (separator <= 0) continue
    const source = line.slice(0, separator)
    if (source.endsWith("/") && exactSources.some(exactSource =>
        source.length < exactSource.length && exactSource.startsWith(source))) return true
  }
  return false
}

function sameBytes(left: Uint8Array, right: Uint8Array): boolean {
  if (left.length !== right.length) return false
  for (let i = 0; i < left.length; i++) if (left[i] !== right[i]) return false
  return true
}

function validateSettingsIcon(data: Uint8Array): void {
  const u16 = (offset: number): number => data[offset] | (data[offset + 1] << 8)
  if (data.length !== 13580 || data[0] !== 0x19 || data[1] !== 0x0a ||
      data[2] !== 0 || data[3] !== 0 || u16(4) !== 112 || u16(6) !== 112 ||
      u16(8) !== 112 || u16(10) !== 0) throw new Error("内置设置图标格式无效")
}

function validateMiSansSubsetHeader(data: Uint8Array): void {
  if (data.length < 4 || data[0] !== 0 || data[1] !== 1 ||
      data[2] !== 0 || data[3] !== 0) throw new Error("内置 Fusion Pixel TTF 字体格式无效")
}

export interface ReloadResult {
  version: number
  result: number
  pending: boolean
  changed: number
}

/** Reject stale revisions, unsupported schemas and partial/torn native writes. */
export function parseReloadResult(text: string, revision: string): ReloadResult | null {
  const record = text.split("\0", 1)[0]
  const lines = record.split("\n")
  if (lines.length !== 4 || lines[3] !== "" || lines[0] !== RELOAD_PREFIX + revision)
    return null
  const fields = /^RHRS1\t([56])\t(-?\d+)\t([01])\t(\d+)$/.exec(lines[1])
  if (!fields || !/^\d+$/.test(lines[2])) return null
  let hash = 2166136261
  const payload = lines[0] + "\n" + lines[1] + "\n"
  for (let i = 0; i < payload.length; i++) hash = Math.imul(hash ^ payload.charCodeAt(i), 16777619) >>> 0
  if (Number(lines[2]) !== hash) return null
  const result = Number(fields[2]), changed = Number(fields[4])
  if (!Number.isInteger(result) || result < -2147483648 || result > 1 ||
      !Number.isInteger(changed) || changed < 0 || changed > 4294967295) return null
  return { version: Number(fields[1]), result, pending: fields[3] === "1", changed }
}

function reloadFailure(code: number): string {
  const errors: { [key: string]: string } = {
    "-2": "字体加载或内存分配失败，旧字体未替换。",
    "-2014": "检测到界面框架重启，字体热重载已停用。请重启设备后重新测试。",
    "-2101": "读取配置所需内存不足。",
    "-2102": "模块无法打开映射配置。",
    "-2103": "映射配置无效或读取失败。",
    "-2201": "字体环境不匹配，或本次运行的热重载已停用。",
    "-2202": "字体注册表发生变化，已停止替换。",
    "-2204": "绘制状态不在支持范围，未强制释放缓存。",
    "-2205": "字体引用或缓存结构不在支持范围。",
    "-2206": "某个字体族尚无可验证的字体实例，整组替换已取消。",
    "-2207": "页面文字对象过多，替换已取消。",
    "-2210": "字体文件无法读取或为空。",
    "-2211": "字体文件超过 32 MiB 安全上限。",
    "-2212": "本次字体文件数量超出限制。",
    "-2213": "已使用的字体文件被改写；请用新的字体代次。"
  }
  return errors[String(code)] || "字体事务被拒绝。请保留错误码，不要强制释放缓存。"
}

/** Installs test resources and waits for revision-matched native results. */
export class ThemeBridge {
  private reloadRevision = 0
  private fontGeneration = 0
  private waitEpoch = 0
  private resultSetupFailure: { revision: string; message: string } | null = null

  cancelWait(): void { this.waitEpoch++ }

  constructor(private readonly file: FileApi) {}

  async installTestTheme(): Promise<void> {
    const oldConfig = await this.readMappings()
    const nextConfig = updateTestMapping(oldConfig, true)
    const asset = await this.readArrayBuffer(THEME_ASSET_URI)
    validateSettingsIcon(asset)
    const existing = await this.readOptionalArrayBuffer(THEME_FILE_URI)
    if (!existing || !sameBytes(asset, existing)) {
      if (existing) await this.deleteFile(THEME_FILE_URI)
      // Some Vela builds report EEXIST for an existing directory; write/readback is authoritative.
      await this.makeDirectory(THEME_DIRECTORY_URI).catch(() => undefined)
      await this.writeArrayBuffer(THEME_FILE_URI, asset)
    }
    const written = await this.readArrayBuffer(THEME_FILE_URI)
    if (!sameBytes(asset, written)) throw new Error("设置图标写入后校验失败")
    if (nextConfig !== oldConfig) await this.writeText(MAPPINGS_URI, nextConfig)
  }

  async deleteTestTheme(): Promise<void> {
    const oldConfig = await this.readMappings()
    const nextConfig = updateTestMapping(oldConfig, false)
    if (nextConfig !== oldConfig) await this.writeText(MAPPINGS_URI, nextConfig)
    if (await this.readOptionalArrayBuffer(THEME_FILE_URI)) await this.deleteFile(THEME_FILE_URI)
  }

  async installAllFirmwareFonts(): Promise<{ generation: string; revision: string }> {
    const oldConfig = await this.readMappings()
    const placeholder = `${THEME_ROOT}font-generations/validation/${FONT_ASSET_NAME}`
    updateFontMappings(oldConfig, true, placeholder)
    const generation = await this.nextFontGeneration()
    const generationDirectory = `g-${generation}`
    const destination = `${THEME_ROOT}font-generations/${generationDirectory}/${FONT_ASSET_NAME}`
    const uri = `${FONT_GENERATION_ROOT_URI}${generationDirectory}/${FONT_ASSET_NAME}`
    const nextConfig = updateFontMappings(oldConfig, true, destination)
    await this.ensureFontPathUnused(uri)
    await this.makeDirectory(`${FONT_GENERATION_ROOT_URI}${generationDirectory}/`).catch(() => undefined)
    try {
      await this.writeMiSansFont(uri)
      const fileInfo = await this.readFileInfo(uri)
      if ((fileInfo.type && fileInfo.type !== "file") || fileInfo.length !== FONT_ASSET_SIZE)
        throw new Error("字体写入后长度校验失败；映射尚未更新。")
    } catch (error) {
      // This generation is not yet referenced by mappings.tsv, so a partial copy is disposable.
      await this.deleteFile(uri).catch(() => undefined)
      throw error
    }
    if (nextConfig !== oldConfig) await this.writeText(MAPPINGS_URI, nextConfig)
    const revision = await this.requestReload()
    return { generation: generationDirectory, revision }
  }

  async restoreFirmwareFonts(): Promise<{ revision: string; broaderDirectoryRule: boolean }> {
    const oldConfig = await this.readMappings()
    const nextConfig = updateFontMappings(oldConfig, false, "")
    if (nextConfig !== oldConfig) await this.writeText(MAPPINGS_URI, nextConfig)
    // Keep all generations immutable; live or retained native objects may still reference them.
    const revision = await this.requestReload()
    return { revision, broaderDirectoryRule: hasBroaderDirectoryMapping(nextConfig, FONT_SOURCES) }
  }

  private async writeMiSansFont(uri: string): Promise<void> {
    let position = 0
    while (position < FONT_ASSET_SIZE) {
      const length = Math.min(FONT_CHUNK_SIZE, FONT_ASSET_SIZE - position)
      const chunk = await this.readArrayBuffer(FONT_ASSET_URI, position, length)
      if (chunk.length !== length) throw new Error(`字体资源分块读取不完整（${position}）。`)
      if (position === 0) validateMiSansSubsetHeader(chunk)
      await this.writeArrayBuffer(uri, chunk, position)
      position += chunk.length
    }
  }

  private async ensureFontPathUnused(uri: string): Promise<void> {
    try {
      await this.readFileInfo(uri)
      throw new Error("字体代次路径已存在；为保持文件不可变，拒绝覆盖。")
    } catch (error) {
      const code = (error as FileOperationError).code
      if (code === 300 || code === 301) return
      throw error
    }
  }

  private async nextFontGeneration(): Promise<string> {
    const savedText = await this.readOptionalText(FONT_GENERATION_COUNTER_URI)
    let saved = 0
    if (savedText !== null) {
      const value = savedText.trim()
      if (!/^\d+$/.test(value)) throw new Error("字体代次计数器格式无效；为避免覆盖旧字体已停止操作。")
      saved = Number(value)
      if (!Number.isSafeInteger(saved) || saved < 0)
        throw new Error("字体代次计数器超出安全范围；为避免复用旧路径已停止操作。")
    }
    const next = Math.max(Date.now(), saved + 1, this.fontGeneration + 1)
    if (!Number.isSafeInteger(next)) throw new Error("无法分配唯一字体代次。")
    await this.writeText(FONT_GENERATION_COUNTER_URI, String(next))
    this.fontGeneration = next
    const random = Math.floor(Math.random() * 0x100000000).toString(36)
    return `${next.toString(36)}-${random}`
  }

  async requestReload(): Promise<string> {
    this.reloadRevision = Math.max(Date.now(), this.reloadRevision + 1)
    const revision = String(this.reloadRevision)
    // Vela rejects empty writeText with code 202. This nonempty placeholder
    // is deliberately not a valid acknowledgement. Prepare before the signal,
    // but never let optional diagnostics block the original reload operation.
    this.resultSetupFailure = null
    try {
      await this.writeText(RESULT_URI, `pending\t${revision}\n`)
    } catch (error) {
      this.resultSetupFailure = { revision, message: String(error) }
    }
    await this.writeText(RELOAD_URI, `${RELOAD_PREFIX}${revision}\n`)
    return revision
  }

  async waitForReload(revision: string, onProgress?: (message: string) => void): Promise<string> {
    const epoch = ++this.waitEpoch
    if (this.resultSetupFailure && this.resultSetupFailure.revision === revision)
      return `重载请求已发送；回执功能不可用，无法确认结果。${this.resultSetupFailure.message}`
    let last: ReloadResult | null = null
    for (let attempt = 0; attempt < 30; attempt++) {
      if (epoch !== this.waitEpoch) throw new Error("已停止等待模块回执。")
      let text: string | null
      try {
        text = await this.readOptionalText(RESULT_URI)
      } catch (error) {
        throw new Error(`重载请求已发送，但读取回执失败，无法确认结果。${String(error)}`)
      }
      if (epoch !== this.waitEpoch) throw new Error("已停止等待模块回执。")
      const result = text === null ? null : parseReloadResult(text, revision)
      if (result) {
        last = result
        if (result.result < 0) {
          const partial = result.changed > 0
            ? ` 已切换 ${result.changed} 个字体族，但刷新未完成；请勿继续切换。` : ""
          throw new Error(`${reloadFailure(result.result)}（${result.result}）${partial}`)
        }
        if (!result.pending && result.result === 0) {
          if (result.version !== 6) return "资源重载完成；当前模块不支持字体热重载，请安装新版 .155 实验模块。"
          return result.changed > 0
            ? `重载完成，已更新 ${result.changed} 个字体族。`
            : "重载完成，本次无字体改动：可能已应用，或映射未命中已注册字体。"
        }
        if (onProgress) onProgress("模块已收到请求，正在等待资源就绪…")
      }
      await new Promise<void>(resolve => setTimeout(resolve, 1000))
    }
    throw new Error(last
      ? "模块仍在等待资源就绪，未确认替换成功。稍后再次点重载；不要删除旧代文件。"
      : "未收到此请求的模块回执。请确认已安装新版实验模块；信号发送不代表替换成功。")
  }

  private async readMappings(): Promise<string> {
    return (await this.readOptionalText(MAPPINGS_URI)) || ""
  }

  private async readOptionalText(uri: string): Promise<string | null> {
    try {
      return await this.readText(uri)
    } catch (error) {
      if ((error as FileOperationError).code === 301) return null
      throw error
    }
  }

  private async readOptionalArrayBuffer(uri: string): Promise<Uint8Array | null> {
    try {
      return await this.readArrayBuffer(uri)
    } catch (error) {
      if ((error as FileOperationError).code === 301) return null
      throw error
    }
  }

  private readText(uri: string): Promise<string> {
    return new Promise((resolve, reject) => {
      try {
        this.file.readText({
          uri,
          encoding: "UTF-8",
          success: result => resolve(result.text),
          fail: (data, code) => reject(fileError("读取", uri, code, data))
        })
      } catch (error) {
        reject(error)
      }
    })
  }

  private writeText(uri: string, text: string): Promise<void> {
    return new Promise((resolve, reject) => {
      try {
        this.file.writeText({
          uri,
          text,
          encoding: "UTF-8",
          success: resolve,
          fail: (data, code) => reject(fileError("写入", uri, code, data))
        })
      } catch (error) {
        reject(error)
      }
    })
  }

  private readArrayBuffer(uri: string, position?: number, length?: number): Promise<Uint8Array> {
    return new Promise((resolve, reject) => {
      const options: {
        uri: string
        position?: number
        length?: number
        success: (data: { buffer: Uint8Array }) => void
        fail: (data: unknown, code: number) => void
      } = {
        uri,
        success: result => resolve(result.buffer),
        fail: (data, code) => reject(fileError("读取", uri, code, data))
      }
      if (position !== undefined) options.position = position
      if (length !== undefined) options.length = length
      try {
        this.file.readArrayBuffer(options)
      } catch (error) {
        reject(error)
      }
    })
  }

  private writeArrayBuffer(uri: string, buffer: Uint8Array, position?: number): Promise<void> {
    return new Promise((resolve, reject) => {
      const options: {
        uri: string
        buffer: Uint8Array
        position?: number
        success: () => void
        fail: (data: unknown, code: number) => void
      } = {
        uri,
        buffer,
        success: resolve,
        fail: (data, code) => reject(fileError("写入", uri, code, data))
      }
      if (position !== undefined) options.position = position
      try {
        this.file.writeArrayBuffer(options)
      } catch (error) {
        reject(error)
      }
    })
  }

  private readFileInfo(uri: string): Promise<{ length: number; type?: string }> {
    return new Promise((resolve, reject) => {
      try {
        this.file.get({
          uri,
          success: result => resolve({ length: result.length, type: result.type }),
          fail: (data, code) => reject(fileError("检查文件", uri, code, data))
        })
      } catch (error) {
        reject(error)
      }
    })
  }

  private makeDirectory(uri: string): Promise<void> {
    return new Promise((resolve, reject) => {
      try {
        this.file.mkdir({
          uri,
          recursive: true,
          success: resolve,
          fail: (data, code) => reject(fileError("创建目录", uri, code, data))
        })
      } catch (error) {
        reject(error)
      }
    })
  }

  private deleteFile(uri: string): Promise<void> {
    return new Promise((resolve, reject) => {
      try {
        this.file.delete({
          uri,
          success: resolve,
          fail: (data, code) => reject(fileError("删除", uri, code, data))
        })
      } catch (error) {
        reject(error)
      }
    })
  }
}
