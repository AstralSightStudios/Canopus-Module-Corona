import { interconnect } from "./import";
import * as file from "./file";
import type { FileOperationError } from "./file";

const VERSION = 1;
const MAX_TEXT_CHARS = 18000;
const MAX_WINDOW = 4;
const MAX_FILES = 128;
const MAX_THEME_BYTES = 64 * 1024 * 1024;
const MAX_CONFIG_BYTES = 32 * 1024;
const MAX_CHUNKS_PER_FILE = 2048;
const QUICKAPP_FILES_URI = "internal://files/";
const MAPPINGS_URI = `${QUICKAPP_FILES_URI}mappings.tsv`;
const TRANSFER_STATE_URI = `${QUICKAPP_FILES_URI}interconnect-transfer.json`;
const INSTALLED_THEMES_URI = `${QUICKAPP_FILES_URI}interconnect-themes.json`;
const NATIVE_SHARED_PATH = "/data/quickapp/files/ng.lst.corona/";
const NATIVE_THEME_ROOT = `${NATIVE_SHARED_PATH}themes/`;
const THEME_ROOT_URI = `${QUICKAPP_FILES_URI}themes/`;

// basE91 alphabet (Bas Wijnen's 91-character variant). Keep identical on sender.
const BASE91_ALPHABET =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&()*+,./:;<=>?@[]^_`{|}~\"";
const BASE91_TABLE = new Int8Array(128);
BASE91_TABLE.fill(-1);
for (let i = 0; i < BASE91_ALPHABET.length; i++)
  BASE91_TABLE[BASE91_ALPHABET.charCodeAt(i)] = i;

export interface ThemeManifestFile {
  relativePath: string;
  sizeBytes: number;
}

interface StoredFile extends ThemeManifestFile {
  chunkSizeBytes: number | null;
  chunkCount: number | null;
  receivedBitmap: string;
  complete: boolean;
}

interface TransferState {
  version: 1;
  themeId: string;
  mode: "replace" | "resume";
  fileCount: number;
  totalBytes: number;
  files: StoredFile[];
  manifestComplete: boolean;
  manifestReceiving: boolean;
  manifestSeen: number;
  finished: boolean;
}

interface ProtocolError extends Error {
  errorCode: string;
}

interface InterconnectEvent {
  data?: unknown;
  code?: number;
  isReconnected?: boolean;
}

export type ReceiverPhase = "waiting" | "ready" | "receiving" | "error" | "success";

export interface ReceiverSnapshot {
  phase: ReceiverPhase;
  message: string;
  themeId: string;
  fileName: string;
  fileIndex: number;
  fileCount: number;
  bytesReceived: number;
  totalBytes: number;
  percent: number;
}

interface InterconnectLink {
  getApkStatus?: () => unknown;
  send(options: {
    data: string;
    success?: () => void;
    fail?: (data: unknown, code: number) => void;
  }): void;
  onmessage?: ((event: InterconnectEvent) => void) | null;
  onopen?: ((event: InterconnectEvent) => void) | null;
  onclose?: ((event: InterconnectEvent) => void) | null;
  onerror?: ((event: InterconnectEvent) => void) | null;
}

interface InterconnectModule {
  instance(): object;
}

function utf8Length(value: string): number {
  let size = 0;
  for (let i = 0; i < value.length; i++) {
    const code = value.charCodeAt(i);
    if (code <= 0x7f) size += 1;
    else if (code <= 0x7ff) size += 2;
    else if (code >= 0xd800 && code <= 0xdbff && i + 1 < value.length &&
             value.charCodeAt(i + 1) >= 0xdc00 && value.charCodeAt(i + 1) <= 0xdfff) {
      size += 4;
      i++;
    } else size += 3;
  }
  return size;
}

function protocolError(errorCode: string, message: string): ProtocolError {
  const error = new Error(message) as ProtocolError;
  error.errorCode = errorCode;
  return error;
}

function errorCode(error: unknown, fallback = "invalid-manifest"): string {
  if (typeof error === "object" && error !== null && "errorCode" in error &&
      typeof error.errorCode === "string") return error.errorCode;
  return fallback;
}

function isRecord(value: unknown): value is { [key: string]: unknown } {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function parseJsonPacket(text: string, type: string): { [key: string]: unknown } {
  if (text.length < 2 || text[0] !== type)
    throw protocolError("invalid-manifest", `无效的 ${type} 控制包`);
  let value: unknown;
  try {
    value = JSON.parse(text.slice(1));
  } catch (_error) {
    throw protocolError("invalid-manifest", "控制包 JSON 格式无效");
  }
  if (!isRecord(value)) throw protocolError("invalid-manifest", "控制包必须是 JSON 对象");
  return value;
}

function integer(value: unknown, min: number, max: number): value is number {
  return typeof value === "number" && Number.isSafeInteger(value) && value >= min && value <= max;
}

function validThemeId(value: unknown): value is string {
  return typeof value === "string" && /^[a-z0-9_-]{1,12}$/.test(value);
}

/** Validate without rewriting: the returned path is the exact transmitted spelling. */
export function validateThemeRelativePath(relativePath: unknown, themeId: string): relativePath is string {
  if (typeof relativePath !== "string" || !relativePath || relativePath[0] === "/" ||
      relativePath.indexOf("\\") >= 0) return false;
  const segments = relativePath.split("/");
  if (segments.some(segment => !segment || segment === "." || segment === "..")) return false;
  const absolutePath = `${NATIVE_THEME_ROOT}${themeId}/${relativePath}`;
  return utf8Length(absolutePath) < 256;
}

/** Decode directly into one bounded buffer; avoid a temporary JS number array. */
export function decodeBase91(encoded: string, expectedLength?: number): Uint8Array {
  const capacity = expectedLength === undefined ? Math.ceil(encoded.length * 7 / 8) + 1 : expectedLength;
  const output = new Uint8Array(capacity);
  let outputLength = 0;
  let accumulator = 0;
  let bitCount = 0;
  let pending = -1;
  const writeByte = (value: number) => {
    if (outputLength >= output.length)
      throw protocolError("decode-failed", "Base91 数据解码长度超出分片预期");
    output[outputLength++] = value & 0xff;
  };

  for (let i = 0; i < encoded.length; i++) {
    const code = encoded.charCodeAt(i);
    const value = code < BASE91_TABLE.length ? BASE91_TABLE[code] : -1;
    if (value < 0) throw protocolError("decode-failed", "Base91 数据含非法字符");
    if (pending < 0) {
      pending = value;
      continue;
    }
    const pair = pending + value * 91;
    accumulator |= pair << bitCount;
    bitCount += (pair & 8191) > 88 ? 13 : 14;
    while (bitCount >= 8) {
      writeByte(accumulator);
      accumulator >>>= 8;
      bitCount -= 8;
    }
    pending = -1;
  }

  if (pending >= 0) {
    accumulator |= pending << bitCount;
    bitCount += 7;
    while (bitCount >= 8) {
      writeByte(accumulator);
      accumulator >>>= 8;
      bitCount -= 8;
    }
  }
  if (expectedLength !== undefined && outputLength !== expectedLength)
    throw protocolError("decode-failed", "Base91 数据解码长度与分片预期不符");
  return outputLength === output.length ? output : output.slice(0, outputLength);
}

function hex4(value: number): string {
  return (`0000${value.toString(16)}`).slice(-4);
}

function hasChunk(bitmap: string, index: number): boolean {
  const nibble = parseInt(bitmap.charAt(Math.floor(index / 4)) || "0", 16);
  return (nibble & (1 << (index % 4))) !== 0;
}

function markChunk(bitmap: string, index: number): string {
  const digit = Math.floor(index / 4);
  const nibble = parseInt(bitmap.charAt(digit) || "0", 16) | (1 << (index % 4));
  const value = nibble.toString(16);
  if (digit >= bitmap.length) return bitmap + value;
  return bitmap.slice(0, digit) + value + bitmap.slice(digit + 1);
}

function receivedCount(bitmap: string, chunkCount: number): number {
  let count = 0;
  for (let index = 0; index < chunkCount; index++) if (hasChunk(bitmap, index)) count++;
  return count;
}

function rangesJson(bitmap: string, chunkCount: number, complete: boolean): string {
  if (complete) return chunkCount ? `[[0,${chunkCount - 1}]]` : "[]";
  let output = "[";
  let start = -1;
  let previous = -1;
  for (let index = 0; index <= chunkCount; index++) {
    if (index < chunkCount && hasChunk(bitmap, index)) {
      if (start < 0) start = index;
      previous = index;
    } else if (start >= 0) {
      if (output.length > 1) output += ",";
      output += `[${start},${previous}]`;
      start = -1;
    }
  }
  return output + "]";
}

function sameHeader(state: TransferState, themeId: string, fileCount: number,
                    totalBytes: number): boolean {
  return state.themeId === themeId && state.fileCount === fileCount &&
    state.totalBytes === totalBytes;
}

function sameManifestFile(left: ThemeManifestFile, right: ThemeManifestFile): boolean {
  return left.relativePath === right.relativePath && left.sizeBytes === right.sizeBytes;
}

function toStoredFile(entry: ThemeManifestFile): StoredFile {
  return {
    relativePath: entry.relativePath,
    sizeBytes: entry.sizeBytes,
    chunkSizeBytes: null,
    chunkCount: null,
    receivedBitmap: "",
    complete: false
  };
}

/** Manager-side receiver for one resumable theme upload at a time. */
export class InterconnectThemeReceiver {
  private link: InterconnectLink | null = null;
  private state: TransferState | null = null;
  private stateLoaded = false;
  private queue: Promise<void> = Promise.resolve();
  private stopped = false;
  private handshakeSent = false;
  private peerMaxTextChars = MAX_TEXT_CHARS;
  private transferPageOpened = false;
  private readonly listeners = new Set<(snapshot: ReceiverSnapshot) => void>();

  constructor(private readonly onTransferPage?: () => void) {}
  private snapshot: ReceiverSnapshot = {
    phase: "waiting",
    message: "等待手机端互联连接…",
    themeId: "",
    fileName: "",
    fileIndex: 0,
    fileCount: 0,
    bytesReceived: 0,
    totalBytes: 0,
    percent: 0
  };

  subscribe(listener: (snapshot: ReceiverSnapshot) => void): () => void {
    this.listeners.add(listener);
    listener({ ...this.snapshot });
    return () => this.listeners.delete(listener);
  }

  start(): void {
    if (this.link) return;
    this.stopped = false;
    this.handshakeSent = false;
    const api = interconnect as unknown as InterconnectModule;
    const link = api.instance() as InterconnectLink;
    this.link = link;
    this.updateSnapshot({ phase: "waiting", message: "等待手机端互联连接…" });
    link.onmessage = event => {
      const packet = typeof event === "string" ? event : event && event.data;
      this.enqueue(typeof packet === "string" ? packet : "");
    };
    const opened = (event: InterconnectEvent = {}) => {
      if (this.stopped) return;
      this.handshakeSent = false;
      this.setPhase("ready", event.isReconnected ? "互联已重连，正在恢复接收状态…" : "互联已连接，正在握手…");
      void this.sendHandshake().catch(error => {
        this.setPhase("error", `互联握手发送失败：${String((error as Error).message || error)}`);
      });
    };
    link.onopen = opened;
    link.onclose = event => {
      if (!this.stopped) {
        const interrupted = this.snapshot.phase === "receiving";
        this.setPhase(interrupted ? "error" : "waiting",
          `互联已断开（${String(event.code ?? "未知原因")}），接收进度已保留，可续传。`);
      }
    };
    link.onerror = event => {
      if (this.stopped || this.snapshot.phase === "success" || this.snapshot.phase === "error") return;
      if (this.snapshot.phase === "receiving") {
        const reason = String(event.data || event.code || "未知错误");
        this.setPhase("error", `互联中断（${reason}），接收进度已保留，可重连续传。`);
      } else {
        // Initial link errors are normal while waiting for the phone to connect.
        this.setPhase("waiting", "等待手机端互联连接…");
      }
    };
    try {
      const status = link.getApkStatus?.();
      const alreadyOpen = status === true || status === 1 ||
        (typeof status === "string" && /open|connect|ready/i.test(status)) ||
        (isRecord(status) && (status.status === 1 || status.status === "open" || status.status === "connected"));
      if (alreadyOpen) opened({});
    } catch (_error) {
      // onopen remains the authoritative connection lifecycle event.
    }
  }

  stop(): void {
    this.stopped = true;
    if (this.link) {
      this.link.onmessage = null;
      this.link.onopen = null;
      this.link.onclose = null;
      this.link.onerror = null;
    }
    this.link = null;
  }

  private updateSnapshot(patch: Partial<ReceiverSnapshot>): void {
    this.snapshot = { ...this.snapshot, ...patch };
    if (!this.stopped) this.listeners.forEach(listener => listener({ ...this.snapshot }));
  }

  private setPhase(phase: ReceiverPhase, message: string): void {
    this.updateSnapshot({ phase, message });
  }

  private updateTransferProgress(state: TransferState, fileIndex: number, message: string): void {
    const entry = state.files[fileIndex];
    let bytesReceived = 0;
    for (const item of state.files) {
      if (item.complete) {
        bytesReceived += item.sizeBytes;
      } else if (item.chunkSizeBytes !== null) {
        for (let chunkIndex = 0; chunkIndex < (item.chunkCount || 0); chunkIndex++) {
          if (hasChunk(item.receivedBitmap, chunkIndex))
            bytesReceived += Math.min(item.chunkSizeBytes,
              item.sizeBytes - chunkIndex * item.chunkSizeBytes);
        }
      }
    }
    const percent = state.totalBytes > 0
      ? Math.min(100, Math.floor(bytesReceived * 100 / state.totalBytes))
      : 0;
    this.updateSnapshot({
      phase: "receiving",
      message,
      themeId: state.themeId,
      fileName: entry ? entry.relativePath : "",
      fileIndex: Math.min(fileIndex + 1, state.fileCount),
      fileCount: state.fileCount,
      bytesReceived,
      totalBytes: state.totalBytes,
      percent
    });
  }

  private enqueue(packet: string): void {
    this.queue = this.queue.then(() => this.handleMessage(packet)).catch(async error => {
      if (this.stopped) return;
      const code = errorCode(error, "write-failed");
      this.setPhase("error", `接收错误（${code}）：${String((error as Error).message || error)}`);
      try {
        await this.sendError(code);
      } catch (_sendError) {
        // Keep the saved transfer state; the sender can retry after reconnecting.
      }
    });
  }

  private async ensureStateLoaded(): Promise<void> {
    if (this.stateLoaded) return;
    const text = await file.readOptionalText(TRANSFER_STATE_URI);
    if (text !== null) {
      let parsed: unknown;
      try { parsed = JSON.parse(text); }
      catch (_error) { throw protocolError("invalid-manifest", "本地续传状态损坏"); }
      if (!isRecord(parsed) || parsed.version !== VERSION || !validThemeId(parsed.themeId) ||
          (parsed.mode !== "replace" && parsed.mode !== "resume") ||
          !integer(parsed.fileCount, 1, MAX_FILES) || !integer(parsed.totalBytes, 0, MAX_THEME_BYTES) ||
          !Array.isArray(parsed.files) || !integer(parsed.manifestSeen, 0, parsed.fileCount) ||
          typeof parsed.manifestComplete !== "boolean" || typeof parsed.manifestReceiving !== "boolean" ||
          typeof parsed.finished !== "boolean")
        throw protocolError("invalid-manifest", "本地续传状态格式无效");
      const loaded = parsed as unknown as TransferState;
      const seenPaths = new Set<string>();
      let loadedBytes = 0;
      for (const entry of parsed.files) {
        if (!isRecord(entry) || !validateThemeRelativePath(entry.relativePath, parsed.themeId) ||
            seenPaths.has(entry.relativePath) || !integer(entry.sizeBytes, 0, MAX_THEME_BYTES) ||
            (entry.chunkSizeBytes !== null && !integer(entry.chunkSizeBytes, 1, MAX_TEXT_CHARS)) ||
            (entry.chunkCount !== null && !integer(entry.chunkCount, 0, 65536)) ||
            typeof entry.receivedBitmap !== "string" || !/^[0-9a-f]*$/.test(entry.receivedBitmap) ||
            typeof entry.complete !== "boolean")
          throw protocolError("invalid-manifest", "本地续传文件状态无效");
        const stored = entry as unknown as StoredFile;
        seenPaths.add(stored.relativePath);
        loadedBytes += stored.sizeBytes;
        if (!Number.isSafeInteger(loadedBytes) || loadedBytes > loaded.totalBytes)
          throw protocolError("invalid-manifest", "本地续传文件总大小无效");
        if (stored.chunkSizeBytes === null &&
            (stored.chunkCount !== null || stored.receivedBitmap.length || stored.complete))
          throw protocolError("invalid-manifest", "本地续传分片状态无效");
        if (stored.chunkSizeBytes !== null) {
          const expectedCount = Math.ceil(stored.sizeBytes / stored.chunkSizeBytes);
          const expectedBitmapLength = Math.ceil(expectedCount / 4);
          if (stored.chunkCount !== expectedCount || expectedCount > MAX_CHUNKS_PER_FILE ||
              (!stored.complete && stored.receivedBitmap.length !== expectedBitmapLength) ||
              (stored.complete && stored.receivedBitmap.length !== 0))
            throw protocolError("invalid-manifest", "本地续传分片状态无效");
        }
      }
      if (loaded.files.length > loaded.fileCount ||
          (loaded.manifestComplete && loaded.files.length !== loaded.fileCount) ||
          (loaded.mode === "replace" && loaded.files.length !== loaded.manifestSeen) ||
          (loaded.finished && (!loaded.manifestComplete || loaded.files.some(entry => !entry.complete))))
        throw protocolError("invalid-manifest", "本地续传清单状态无效");
      this.state = loaded;
    }
    this.stateLoaded = true;
  }

  private async persist(next: TransferState): Promise<void> {
    await file.writeText(TRANSFER_STATE_URI, JSON.stringify(next));
    this.state = next;
  }

  private async handleMessage(packet: string): Promise<void> {
    if (!packet) throw protocolError("invalid-manifest", "收到空互联消息");
    if (packet.length > MAX_TEXT_CHARS)
      throw protocolError("invalid-manifest", "互联消息超过本地 18,000 字符上限");

    try {
      switch (packet[0]) {
        case "H": await this.handleHandshake(packet); return;
        case "T": await this.handleTransfer(packet); return;
        case "P": await this.handlePrepare(packet); return;
        case "F": await this.handleFileChunk(packet); return;
        case "C": await this.handleFileComplete(packet); return;
        case "E": {
          const message = parseJsonPacket(packet, "E");
          this.setPhase("error", `手机端报告错误：${String(message.errorCode || "unknown")}`);
          return;
        }
        case "A": return; // Acknowledgements are sent by this receiver, not consumed here.
        default: throw protocolError("invalid-manifest", "未知互联包类型");
      }
    } catch (error) {
      const themeId = this.state?.themeId;
      const operation = packet[0] === "T" ? this.safeOperation(packet) : "";
      const code = errorCode(error, "write-failed");
      if (packet[0] === "T" && operation === "end") {
        try {
          await this.sendJson("T", {
            operation: "status", themeId: themeId || "unknown", status: "reject", errorCode: code
          });
        } catch (_sendError) { /* The sender will retry the end packet. */ }
      } else {
        try { await this.sendError(code); }
        catch (_sendError) { /* The sender will retry; persisted chunks remain safe. */ }
      }
      this.setPhase("error", `接收错误（${code}）：${String((error as Error).message || error)}`);
    }
  }

  private safeOperation(packet: string): string {
    try { return String(parseJsonPacket(packet, "T").operation || ""); }
    catch (_error) { return ""; }
  }

  private async handleHandshake(packet: string): Promise<void> {
    const message = parseJsonPacket(packet, "H");
    if (message.version !== VERSION || !integer(message.maxTextChars, 256, 1000000))
      throw protocolError("invalid-manifest", "互联协议版本或最大消息长度不受支持");
    this.peerMaxTextChars = Math.min(MAX_TEXT_CHARS, message.maxTextChars);
    this.setPhase("ready", `握手完成，可接收主题包（单条消息上限 ${this.peerMaxTextChars} 字符）。`);
    if (!this.handshakeSent) await this.sendHandshake();
  }

  private async sendHandshake(): Promise<void> {
    if (this.handshakeSent) return;
    this.handshakeSent = true;
    try {
      await this.sendJson("H", { version: VERSION, maxTextChars: MAX_TEXT_CHARS, maxWindow: MAX_WINDOW });
    } catch (error) {
      this.handshakeSent = false;
      throw error;
    }
  }

  private async handleTransfer(packet: string): Promise<void> {
    const message = parseJsonPacket(packet, "T");
    const operation = message.operation;
    if (operation === "begin") {
      await this.beginTransfer(message);
      return;
    }
    if (operation === "file") {
      await this.receiveManifestFile(message);
      return;
    }
    if (operation === "end") {
      await this.endManifest(message);
      return;
    }
    if (operation === "finish") {
      await this.finishTransfer(message);
      return;
    }
    throw protocolError("invalid-manifest", "不支持的主题清单操作");
  }

  private async beginTransfer(message: { [key: string]: unknown }): Promise<void> {
    const themeId = message.themeId;
    const mode = message.mode;
    const fileCount = message.fileCount;
    const totalBytes = message.totalBytes;
    if (!validThemeId(themeId) || (mode !== "replace" && mode !== "resume") ||
        !integer(fileCount, 1, MAX_FILES) || !integer(totalBytes, 0, MAX_THEME_BYTES))
      throw protocolError("invalid-manifest", "主题清单头无效或超过本地上限");

    await this.ensureStateLoaded();
    const current = this.state;
    if (mode === "resume") {
      if (!current || !sameHeader(current, themeId, fileCount, totalBytes) || !current.manifestComplete)
        throw protocolError("invalid-manifest", "没有可匹配的续传状态；请以 replace 重新传输");
      if (current.mode === "resume" && current.manifestReceiving) {
        await this.sendTransferAck(themeId, "begin");
        return;
      }
      const next = { ...current, mode: "resume" as const, manifestReceiving: true, manifestSeen: 0 };
      await this.persist(next);
      if (current.finished) this.transferPageOpened = false;
      await this.sendTransferAck(themeId, "begin");
      this.updateSnapshot({ phase: "ready", message: `正在校验主题 ${themeId} 的续传清单…`, themeId,
        fileCount, totalBytes, bytesReceived: 0, percent: 0 });
      return;
    }

    const duplicateBegin = current && !current.finished && current.mode === "replace" &&
      current.manifestReceiving && sameHeader(current, themeId, fileCount, totalBytes);
    if (duplicateBegin) {
      await this.sendTransferAck(themeId, "begin");
      return;
    }
    if (current && !current.finished && current.themeId !== themeId)
      throw protocolError("invalid-manifest", `主题 ${current.themeId} 仍有未完成传输`);
    if (await this.isThemeActive(themeId))
      throw protocolError("active-theme", `主题 ${themeId} 正在使用；请先切换到其他主题`);

    await this.removeThemeDirectory(themeId);
    await this.unregisterTheme(themeId);
    const next: TransferState = {
      version: VERSION,
      themeId,
      mode: "replace",
      fileCount,
      totalBytes,
      files: [],
      manifestComplete: false,
      manifestReceiving: true,
      manifestSeen: 0,
      finished: false
    };
    await this.persist(next);
    this.transferPageOpened = false;
    await this.sendTransferAck(themeId, "begin");
    this.updateSnapshot({ phase: "ready", message: `正在接收主题 ${themeId} 的文件清单…`, themeId,
      fileName: "", fileIndex: 0, fileCount, bytesReceived: 0, totalBytes, percent: 0 });
  }

  private async receiveManifestFile(message: { [key: string]: unknown }): Promise<void> {
    const state = this.state;
    if (!state || !state.manifestReceiving || message.themeId !== state.themeId ||
        !integer(message.fileIndex, 0, state.fileCount - 1) ||
        !integer(message.sizeBytes, 0, MAX_THEME_BYTES) ||
        !validateThemeRelativePath(message.relativePath, state.themeId))
      throw protocolError("invalid-manifest", "文件清单条目无效或未开始");

    const index = message.fileIndex;
    const entry: ThemeManifestFile = {
      relativePath: message.relativePath as string,
      sizeBytes: message.sizeBytes
    };
    if (index < state.manifestSeen) {
      const existing = state.files[index];
      if (!existing || !sameManifestFile(existing, entry))
        throw protocolError("invalid-manifest", "重复文件序号内容不一致");
      await this.sendTransferFileAck(state.themeId, index);
      return;
    }
    if (index !== state.manifestSeen)
      throw protocolError("invalid-manifest", "文件序号必须从 0 连续递增");

    const next = { ...state, files: state.files.slice(), manifestSeen: state.manifestSeen + 1 };
    if (state.mode === "resume") {
      const existing = state.files[index];
      if (!existing || !sameManifestFile(existing, entry))
        throw protocolError("invalid-manifest", "续传清单与原清单不同；请使用 replace");
    } else {
      next.files.push(toStoredFile(entry));
    }
    await this.persist(next);
    await this.sendTransferFileAck(state.themeId, index);
  }

  private async endManifest(message: { [key: string]: unknown }): Promise<void> {
    const state = this.state;
    if (!state || message.themeId !== state.themeId)
      throw protocolError("invalid-manifest", "主题清单结束包与当前主题不匹配");
    if (state.manifestComplete && !state.manifestReceiving) {
      await this.sendStatus(state.themeId, "ready");
      return;
    }
    if (!state.manifestReceiving || state.manifestSeen !== state.fileCount ||
        state.files.length !== state.fileCount)
      throw protocolError("invalid-manifest", "文件清单数量不完整");

    let totalBytes = 0;
    let mappingCount = 0;
    const paths = new Set<string>();
    for (const entry of state.files) {
      if (!validateThemeRelativePath(entry.relativePath, state.themeId) || paths.has(entry.relativePath))
        throw protocolError("invalid-path", "主题清单包含不安全或重复路径");
      paths.add(entry.relativePath);
      totalBytes += entry.sizeBytes;
      if (!Number.isSafeInteger(totalBytes) || totalBytes > MAX_THEME_BYTES)
        throw protocolError("no-space", "主题总大小超过 Manager 本地限制");
      if (entry.relativePath === "mappings.tsv") {
        mappingCount++;
        if (entry.sizeBytes > MAX_CONFIG_BYTES)
          throw protocolError("invalid-manifest", "mappings.tsv 超过 32 KiB 模块限制");
      }
    }
    if (totalBytes !== state.totalBytes || mappingCount !== 1)
      throw protocolError("invalid-manifest", "清单总字节数不匹配或缺少唯一的 mappings.tsv");

    const next = {
      ...state,
      manifestComplete: true,
      manifestReceiving: false
    };
    await this.persist(next);
    await this.sendStatus(state.themeId, "ready");
    this.updateSnapshot({ phase: "ready", message: `主题 ${state.themeId} 清单已校验，等待文件分片。`,
      themeId: state.themeId, fileName: "", fileIndex: 0, fileCount: state.fileCount,
      bytesReceived: 0, totalBytes: state.totalBytes, percent: 0 });
  }

  private async handlePrepare(packet: string): Promise<void> {
    const message = parseJsonPacket(packet, "P");
    await this.ensureStateLoaded();
    const state = this.state;
    if (!state || !state.manifestComplete || state.manifestReceiving || message.themeId !== state.themeId ||
        !integer(message.fileIndex, 0, state.fileCount - 1) ||
        !integer(message.sizeBytes, 0, MAX_THEME_BYTES) ||
        !integer(message.chunkSizeBytes, 1, MAX_TEXT_CHARS))
      throw protocolError("invalid-manifest", "文件准备包与主题清单不匹配");

    const index = message.fileIndex;
    const stored = state.files[index];
    const chunkSize = message.chunkSizeBytes;
    const chunkCount = Math.ceil(stored.sizeBytes / chunkSize);
    const maxChunkBytes = Math.floor((Math.min(MAX_TEXT_CHARS, this.peerMaxTextChars) - 9) * 13 / 16);
    if (message.sizeBytes !== stored.sizeBytes || chunkSize > maxChunkBytes ||
        !integer(message.chunkCount, 0, 65536) || message.chunkCount !== chunkCount ||
        chunkCount > MAX_CHUNKS_PER_FILE)
      throw protocolError("invalid-manifest", "文件大小、分片参数或分片数量无效");

    const firstIncomplete = state.files.findIndex(entry => !entry.complete);
    if (firstIncomplete >= 0 && index > firstIncomplete)
      throw protocolError("invalid-manifest", "必须按顺序完成前一个文件");

    let next = state;
    if (stored.chunkSizeBytes !== null &&
        (stored.chunkSizeBytes !== chunkSize || stored.chunkCount !== chunkCount) &&
        receivedCount(stored.receivedBitmap, stored.chunkCount || 0) > 0)
      throw protocolError("invalid-manifest", "已接收分片的大小发生变化；请使用 replace");
    if (stored.chunkSizeBytes !== chunkSize || stored.chunkCount !== chunkCount ||
        (!stored.complete && !stored.receivedBitmap)) {
      const files = state.files.slice();
      files[index] = { ...stored, chunkSizeBytes: chunkSize, chunkCount,
        receivedBitmap: stored.complete ? "" : new Array(Math.ceil(chunkCount / 4) + 1).join("0") };
      next = { ...state, files };
      await this.persist(next);
    }
    await this.sendPrepared(index, next.files[index]);
    this.updateTransferProgress(next, index, `等待接收：${stored.relativePath}`);
    if (!this.transferPageOpened) {
      this.transferPageOpened = true;
      try { this.onTransferPage?.(); }
      catch (_error) { /* Navigation must not interrupt the transfer. */ }
    }
  }

  private async handleFileChunk(packet: string): Promise<void> {
    await this.ensureStateLoaded();
    if (packet.length < 9 || !/^[0-9a-fA-F]{8}$/.test(packet.slice(1, 9)))
      throw protocolError("invalid-manifest", "文件分片头格式无效");
    const fileIndex = parseInt(packet.slice(1, 5), 16);
    const chunkIndex = parseInt(packet.slice(5, 9), 16);
    const state = this.state;
    if (!state || !state.manifestComplete || state.manifestReceiving || fileIndex >= state.fileCount)
      throw protocolError("invalid-manifest", "文件分片没有对应的主题清单");
    const stored = state.files[fileIndex];
    if (stored.chunkSizeBytes === null || stored.chunkCount === null ||
        chunkIndex >= stored.chunkCount)
      throw protocolError("invalid-manifest", "文件分片没有对应的 P 准备状态");
    if (packet.length > Math.min(MAX_TEXT_CHARS, this.peerMaxTextChars))
      throw protocolError("invalid-manifest", "文件分片超过协商的消息长度");

    if (stored.complete || hasChunk(stored.receivedBitmap, chunkIndex)) {
      await this.sendChunkAck(fileIndex, chunkIndex);
      this.updateTransferProgress(state, fileIndex, `已接收：${stored.relativePath}`);
      return;
    }
    const position = chunkIndex * stored.chunkSizeBytes;
    const expectedLength = Math.min(stored.chunkSizeBytes, stored.sizeBytes - position);
    const data = decodeBase91(packet.slice(9), expectedLength);

    const targetUri = this.themeFileUri(state.themeId, stored.relativePath);
    const slash = targetUri.lastIndexOf("/");
    await this.ensureDirectory(targetUri.slice(0, slash + 1));
    await file.writeArrayBuffer(targetUri, data, position);

    const files = state.files.slice();
    files[fileIndex] = { ...stored, receivedBitmap: markChunk(stored.receivedBitmap, chunkIndex) };
    const next = { ...state, files };
    await this.persist(next);
    await this.sendChunkAck(fileIndex, chunkIndex);
    this.updateTransferProgress(next, fileIndex, `正在接收：${stored.relativePath}`);
  }

  private async handleFileComplete(packet: string): Promise<void> {
    await this.ensureStateLoaded();
    if (packet.length !== 5 || !/^[0-9a-fA-F]{4}$/.test(packet.slice(1)))
      throw protocolError("invalid-manifest", "文件完成包格式无效");
    const index = parseInt(packet.slice(1), 16);
    const state = this.state;
    if (!state || !state.manifestComplete || state.manifestReceiving || index >= state.fileCount)
      throw protocolError("invalid-manifest", "文件完成包没有对应的主题文件");
    const stored = state.files[index];
    if (stored.chunkCount === null || stored.chunkSizeBytes === null)
      throw protocolError("invalid-manifest", "文件尚未准备");
    if (!stored.complete && receivedCount(stored.receivedBitmap, stored.chunkCount) !== stored.chunkCount) {
      await this.sendPrepared(index, stored);
      this.updateTransferProgress(state, index, `等待补传：${stored.relativePath}`);
      return;
    }
    let next = state;
    if (!stored.complete) {
      const files = state.files.slice();
      files[index] = { ...stored, complete: true, receivedBitmap: "" };
      next = { ...state, files };
      await this.persist(next);
    }
    await this.send(`C${hex4(index)}`);
    this.updateTransferProgress(next, index, `文件已接收：${stored.relativePath}`);
  }

  private async finishTransfer(message: { [key: string]: unknown }): Promise<void> {
    await this.ensureStateLoaded();
    const state = this.state;
    if (!state || message.themeId !== state.themeId || !state.manifestComplete || state.manifestReceiving ||
        state.files.length !== state.fileCount || state.files.some(entry => !entry.complete))
      throw protocolError("invalid-manifest", "主题仍有未完成文件，不能登记为可用");
    if (!state.finished) await this.persist({ ...state, finished: true });
    await this.registerTheme(state.themeId);
    await this.sendStatus(state.themeId, "ready");
    this.updateSnapshot({ phase: "success", message: `主题 ${state.themeId} 已接收完成，可由主题管理器选择应用。`,
      themeId: state.themeId, fileName: "", fileIndex: state.fileCount, fileCount: state.fileCount,
      bytesReceived: state.totalBytes, totalBytes: state.totalBytes, percent: 100 });
  }

  private async sendPrepared(index: number, stored: StoredFile): Promise<void> {
    const chunkCount = stored.chunkCount || 0;
    const received = stored.complete ? chunkCount : receivedCount(stored.receivedBitmap, chunkCount);
    const complete = stored.complete || received === chunkCount;
    const status = complete ? "complete" : received > 0 ? "resume" : "ready";
    const packet = JSON.stringify({
      themeId: this.state?.themeId,
      fileIndex: index,
      status,
      window: MAX_WINDOW
    });
    await this.send(`P${packet.slice(0, -1)},"receivedRanges":${rangesJson(stored.receivedBitmap, chunkCount, stored.complete)}}`);
  }

  private async sendTransferAck(themeId: string, itemType: "begin"): Promise<void> {
    await this.sendJson("T", { operation: "ack", themeId, itemType });
  }

  private async sendTransferFileAck(themeId: string, fileIndex: number): Promise<void> {
    await this.sendJson("T", { operation: "ack", themeId, itemType: "file", fileIndex });
  }

  private async sendStatus(themeId: string, status: "ready" | "reject"): Promise<void> {
    await this.sendJson("T", { operation: "status", themeId, status });
  }

  private async sendChunkAck(fileIndex: number, chunkIndex: number): Promise<void> {
    await this.send(`A${hex4(fileIndex)}${hex4(chunkIndex)}`);
  }

  private async sendError(errorCodeValue: string): Promise<void> {
    await this.sendJson("E", {
      themeId: this.state?.themeId,
      fileIndex: this.currentFileIndex(),
      errorCode: errorCodeValue
    });
  }

  private currentFileIndex(): number | undefined {
    if (!this.state) return undefined;
    const index = this.state.files.findIndex(entry => !entry.complete);
    return index >= 0 ? index : undefined;
  }

  private async sendJson(type: string, value: { [key: string]: unknown }): Promise<void> {
    await this.send(type + JSON.stringify(value));
  }

  private async send(packet: string): Promise<void> {
    if (!this.link) throw new Error("interconnect 尚未连接");
    if (packet.length > Math.min(MAX_TEXT_CHARS, this.peerMaxTextChars))
      throw new Error("待发送互联消息超过协商长度");
    const link = this.link;
    await new Promise<void>((resolve, reject) => {
      try {
        link.send({
          data: packet,
          success: resolve,
          fail: (data, code) => reject(new Error(`interconnect.send 失败（${code}）：${String(data)}`))
        });
      } catch (error) { reject(error); }
    });
  }

  private async ensureDirectory(uri: string): Promise<void> {
    try {
      const info = await file.readFileInfo(uri);
      if (info.type === "file") throw protocolError("write-failed", "目标父路径已存在为普通文件");
      return;
    } catch (error) {
      if ((error as FileOperationError).code !== 301) throw error;
    }
    try {
      await file.makeDirectory(uri);
    } catch (error) {
      // Treat EEXIST as success only after confirming the directory is now present.
      const info = await file.readFileInfo(uri).catch(() => null);
      if (!info || info.type === "file") throw error;
    }
  }

  private themeFileUri(themeId: string, relativePath: string): string {
    return `${THEME_ROOT_URI}${themeId}/${relativePath}`;
  }

  private async isThemeActive(themeId: string): Promise<boolean> {
    const mappings = await file.readOptionalText(MAPPINGS_URI);
    if (!mappings) return false;
    const destinationPrefix = `${NATIVE_THEME_ROOT}${themeId}/`;
    return mappings.split(/\r?\n/).some(line => {
      if (!line || line[0] === "#") return false;
      const separator = line.indexOf("\t");
      return separator > 0 && line.slice(separator + 1).startsWith(destinationPrefix);
    });
  }

  private async removeThemeDirectory(themeId: string): Promise<void> {
    const uri = `${THEME_ROOT_URI}${themeId}/`;
    let info: file.FileInfo;
    try { info = await file.readFileInfo(uri); }
    catch (error) {
      if ((error as FileOperationError).code === 301) return;
      throw error;
    }
    if (info.type === "file") await file.deleteFile(uri);
    else await file.removeDirectory(uri);
  }

  private async readInstalledThemes(): Promise<string[]> {
    const text = await file.readOptionalText(INSTALLED_THEMES_URI);
    if (text === null) return [];
    let value: unknown;
    try { value = JSON.parse(text); }
    catch (_error) { throw protocolError("write-failed", "已安装主题索引损坏"); }
    if (!Array.isArray(value) || value.some(item => !validThemeId(item)))
      throw protocolError("write-failed", "已安装主题索引格式无效");
    return value as string[];
  }

  private async unregisterTheme(themeId: string): Promise<void> {
    const themes = await this.readInstalledThemes();
    const next = themes.filter(item => item !== themeId);
    if (next.length !== themes.length)
      await file.writeText(INSTALLED_THEMES_URI, JSON.stringify(next));
  }

  private async registerTheme(themeId: string): Promise<void> {
    const themes = await this.readInstalledThemes();
    if (themes.indexOf(themeId) < 0) {
      themes.push(themeId);
      await file.writeText(INSTALLED_THEMES_URI, JSON.stringify(themes));
    }
  }
}
