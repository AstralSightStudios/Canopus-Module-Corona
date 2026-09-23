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
}

export interface ProbeResult {
  uri: string
  bytes: number
}

/** Exact path the native module is intended to consume. */
export const NATIVE_SHARED_PATH = "/data/files/ng.lst.corona/"

/** App-scoped Vela files root; the runtime resolves the current app identity. */
export const QUICKAPP_FILES_URI = "internal://files/"

interface FileOperationError extends Error {
  code?: number
}

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

/**
 * Diagnostic only: current firmware routes system.file paths through the
 * Quick App sandbox. A successful API round-trip alone does not prove that the
 * native module can see the same file.
 */
export class ThemeBridge {
  constructor(private readonly file: FileApi) {}

  probeNativeSharedPath(): Promise<ProbeResult> {
    return this.roundTrip(`${NATIVE_SHARED_PATH}manager-probe.txt`)
  }

  probeQuickAppFilesUri(): Promise<ProbeResult> {
    return this.roundTrip(`${QUICKAPP_FILES_URI}manager-probe.txt`)
  }

  private async roundTrip(uri: string): Promise<ProbeResult> {
    const marker = `manager-probe\t${Date.now()}\n`
    await this.writeText(uri, marker)
    const actual = await this.readText(uri)
    if (actual !== marker) throw new Error(`回读内容不一致：${uri}`)
    return { uri, bytes: utf8Length(actual) }
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
}
