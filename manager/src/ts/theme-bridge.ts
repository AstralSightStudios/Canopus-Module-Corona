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
    success: (data: { buffer: Uint8Array }) => void
    fail: (data: unknown, code: number) => void
  }): void
  writeArrayBuffer(options: {
    uri: string
    buffer: Uint8Array
    success: () => void
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
const THEME_ROOT = `${NATIVE_SHARED_PATH}themes/`
const THEME_DIRECTORY_URI = `${QUICKAPP_FILES_URI}themes/current/app/settings/`
const THEME_FILE_URI = `${THEME_DIRECTORY_URI}launcher.bin`
const THEME_ASSET_URI = "/common/settings-launcher.bin"
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

function validDirectoryPath(path: string): boolean {
  if (!path || utf8Length(path) >= MAX_PATH_BYTES || path[0] !== "/" ||
      path[path.length - 1] !== "/") return false
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

function updateTestMapping(config: string, install: boolean): string {
  if (config.indexOf("\0") >= 0 || utf8Length(config) > MAX_CONFIG_BYTES)
    throw new Error("mappings.tsv 超过大小限制或包含 NUL")

  const lines = config.split(/\r?\n/)
  if (lines[lines.length - 1] === "") lines.pop()
  const output: string[] = []
  const sources = new Set<string>()
  let activeRules = 0
  let testRuleSeen = false

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
    if (!validDirectoryPath(source) || !validDirectoryPath(destination))
      throw new Error("mappings.tsv 包含无效目录路径")
    if (sources.has(source)) throw new Error(`mappings.tsv 存在重复源目录：${source}`)
    sources.add(source)

    if (source === TEST_SOURCE) {
      if (testRuleSeen) throw new Error("测试主题映射重复")
      testRuleSeen = true
      if (destination !== TEST_DESTINATION && destination !== LEGACY_TEST_DESTINATION)
        throw new Error("测试源目录已映射到其他目标，已保留原配置")
      if (install) {
        output.push(`${TEST_SOURCE}\t${TEST_DESTINATION}`)
        activeRules++
      }
      continue
    }

    if (!destination.startsWith(THEME_ROOT))
      throw new Error(`映射目标必须位于 ${THEME_ROOT}`)
    output.push(line)
    activeRules++
  }

  if (install && !testRuleSeen) {
    output.push(`${TEST_SOURCE}\t${TEST_DESTINATION}`)
    activeRules++
  }
  if (activeRules > MAX_RULES) throw new Error(`映射规则不能超过 ${MAX_RULES} 条`)

  if (!activeRules && !output.some(line => line.trim().length > 0))
    return "# No active theme mappings.\n"
  const result = output.join("\n") + (output.length ? "\n" : "")
  if (utf8Length(result) > MAX_CONFIG_BYTES) throw new Error("mappings.tsv 超过 32 KiB")
  return result
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

/** Installs the known settings-icon test theme into this app's private files. */
export class ThemeBridge {
  private reloadRevision = 0

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

  async requestReload(): Promise<string> {
    this.reloadRevision = Math.max(Date.now(), this.reloadRevision + 1)
    const revision = String(this.reloadRevision)
    await this.writeText(RELOAD_URI, `${RELOAD_PREFIX}${revision}\n`)
    return revision
  }

  private async readMappings(): Promise<string> {
    try {
      return await this.readText(MAPPINGS_URI)
    } catch (error) {
      if ((error as FileOperationError).code === 301) return ""
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

  private readArrayBuffer(uri: string): Promise<Uint8Array> {
    return new Promise((resolve, reject) => {
      try {
        this.file.readArrayBuffer({
          uri,
          success: result => resolve(result.buffer),
          fail: (data, code) => reject(fileError("读取", uri, code, data))
        })
      } catch (error) {
        reject(error)
      }
    })
  }

  private writeArrayBuffer(uri: string, buffer: Uint8Array): Promise<void> {
    return new Promise((resolve, reject) => {
      try {
        this.file.writeArrayBuffer({
          uri,
          buffer,
          success: resolve,
          fail: (data, code) => reject(fileError("写入", uri, code, data))
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
