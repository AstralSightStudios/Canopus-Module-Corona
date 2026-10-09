import * as file from "./file";

export interface StoredFile {
  relativePath: string;
  sizeBytes: number;
  chunkSizeBytes: number | null;
  chunkCount: number | null;
  receivedBitmap: string;
  complete: boolean;
}

export interface TransferState {
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

type Header = Omit<TransferState, "files">;
interface BaseRecord {
  kind: "base";
  generation: number;
  token: string;
  state: TransferState;
}
interface DeltaRecord {
  kind: "delta";
  generation: number;
  token: string;
  sequence: number;
  previous: number;
  header: Header;
  fileIndex: number;
  fileLength: number;
  entry?: StoredFile;
}
interface Frame {
  value: unknown;
  checksum: number;
  end: number;
}
interface LoadedSlot {
  slot: number;
  generation: number;
  token: string;
  sequence: number;
  checksum: number;
  offset: number;
  baseEnd: number;
  state: TransferState;
  dirty?: boolean;
}

interface SlotRead {
  kind: "missing" | "incomplete" | "valid";
  value?: LoadedSlot;
}
class StorageFormatError extends Error {}

const DEFAULT_URI = "internal://files/interconnect-transfer.json";
const PREFIX = "ICTJ0001";
const SUFFIX = "ICTJEND!";
const FRAME_OVERHEAD = 32;
const TERMINATOR_BYTES = 8;
const MAX_BASE_BYTES = 64 * 1024 * 1024;
const MAX_LOG_BYTES = 1024 * 1024;
const MAX_LOG_RECORDS = 1024;

function record(value: unknown): value is { [key: string]: any } {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
function natural(value: unknown): value is number {
  return typeof value === "number" && Number.isSafeInteger(value) && value >= 0;
}
function header(state: TransferState): Header {
  return {
    version: state.version, themeId: state.themeId, mode: state.mode,
    fileCount: state.fileCount, totalBytes: state.totalBytes,
    manifestComplete: state.manifestComplete, manifestReceiving: state.manifestReceiving,
    manifestSeen: state.manifestSeen, finished: state.finished
  };
}
function basicState(value: unknown): value is TransferState {
  return record(value) && value.version === 1 && Array.isArray(value.files) &&
    typeof value.themeId === "string" && (value.mode === "replace" || value.mode === "resume") &&
    natural(value.fileCount) && natural(value.totalBytes) && natural(value.manifestSeen) &&
    typeof value.manifestComplete === "boolean" && typeof value.manifestReceiving === "boolean" &&
    typeof value.finished === "boolean";
}
function hex(value: number): string {
  return ("00000000" + (value >>> 0).toString(16)).slice(-8);
}
function checksum(bytes: Uint8Array, start: number, end: number): number {
  let hash = 0x811c9dc5;
  for (let i = start; i < end; i++) hash = Math.imul(hash ^ bytes[i], 0x01000193);
  return hash >>> 0;
}
function ascii(bytes: Uint8Array, start: number, end: number): string {
  // Bounded blocks avoid argument limits and quadratic per-byte concatenation.
  let result = "";
  for (let i = start; i < end; i += 4096) {
    const codes: number[] = [];
    for (let j = i; j < Math.min(end, i + 4096); j++) codes.push(bytes[j]);
    result += String.fromCharCode.apply(null, codes);
  }
  return result;
}
function encode(value: unknown): { bytes: Uint8Array; checksum: number } {
  // Escape UTF-16 code units, including surrogate pairs. JSON.parse restores them.
  const text = JSON.stringify(value).replace(/[\u007f-\uffff]/g,
    character => "\\u" + ("0000" + character.charCodeAt(0).toString(16)).slice(-4));
  if (text.length > MAX_BASE_BYTES) throw new Error("Transfer checkpoint exceeds storage limit");
  const bytes = new Uint8Array(text.length + FRAME_OVERHEAD + TERMINATOR_BYTES);
  for (let i = 0; i < text.length; i++) bytes[24 + i] = text.charCodeAt(i);
  const sum = checksum(bytes, 24, 24 + text.length);
  const lead = PREFIX + hex(text.length) + hex(sum);
  for (let i = 0; i < lead.length; i++) bytes[i] = lead.charCodeAt(i);
  for (let i = 0; i < SUFFIX.length; i++) bytes[24 + text.length + i] = SUFFIX.charCodeAt(i);
  return { bytes, checksum: sum };
}
function allZero(bytes: Uint8Array, start: number): boolean {
  for (let i = start; i < bytes.length; i++) if (bytes[i] !== 0) return false;
  return true;
}
function tornLiteral(bytes: Uint8Array, offset: number, literal: string): boolean {
  // Recognize a sequential partial native write into preallocated zero space.
  // Nonzero bytes beyond the torn boundary mean this is not a final torn frame.
  for (let i = 0; i < literal.length; i++) {
    if (offset + i === bytes.length) return true;
    if (bytes[offset + i] === 0) return allZero(bytes, offset + i);
    if (bytes[offset + i] !== literal.charCodeAt(i)) return false;
  }
  return false;
}
function decode(bytes: Uint8Array, offset: number): Frame | null {
  const remaining = bytes.length - offset;
  if (remaining === 0 || allZero(bytes, offset)) return null;
  if (remaining < 8 || ascii(bytes, offset, offset + 8) !== PREFIX) {
    if (tornLiteral(bytes, offset, PREFIX)) return null;
    throw new StorageFormatError("Corrupt transfer frame prefix");
  }
  const fields = ascii(bytes, offset + 8, Math.min(bytes.length, offset + 24));
  if (!/^[0-9a-f]{16}$/.test(fields)) {
    const zero = fields.indexOf("\0");
    if (zero >= 0 && /^[0-9a-f]*$/.test(fields.slice(0, zero)) &&
        allZero(bytes, offset + 8 + zero)) return null;
    if (remaining < 24 && /^[0-9a-f]*$/.test(fields)) return null;
    throw new StorageFormatError("Corrupt transfer frame header");
  }
  const length = parseInt(fields.slice(0, 8), 16);
  const sum = parseInt(fields.slice(8), 16);
  const end = offset + FRAME_OVERHEAD + length;
  if (length > MAX_BASE_BYTES) throw new StorageFormatError("Transfer frame exceeds storage limit");
  if (end > bytes.length) return null;
  if (ascii(bytes, end - 8, end) !== SUFFIX) {
    if (tornLiteral(bytes, end - 8, SUFFIX)) return null;
    throw new StorageFormatError("Corrupt transfer frame boundary");
  }
  if (checksum(bytes, offset + 24, end - 8) !== sum)
    throw new StorageFormatError("Corrupt transfer frame checksum");
  let value: unknown;
  try { value = JSON.parse(ascii(bytes, offset + 24, end - 8)); }
  catch (_error) { throw new StorageFormatError("Corrupt transfer frame JSON"); }
  return { value, checksum: sum, end };
}

/**
 * Single-writer durable state storage. Await save before publishing state or ACK.
 * The caller must validate load's result with the receiver's full v1 validator.
 * Caller-owned states and entries must not be mutated during or after save.
 * Complete corruption (even in an older slot) fails closed: never roll back ACKs.
 * Only identifiable final torn writes are ignored; native I/O failures propagate.
 * Legacy JSON is read-only: downgraded receivers cannot see new journal progress.
 * The 64 MiB base cap bounds disk usage, not peak JS memory: ASCII JSON encoding
 * and checkpoint array copies require additional temporary memory.
 */
export class TransferStorage {
  private loaded = false;
  private state: TransferState | null = null;
  private active: LoadedSlot | null = null;
  private busy = false;

  constructor(private readonly snapshotUri: string = DEFAULT_URI) {}

  private slotUri(slot: number): string { return `${this.snapshotUri}.journal-${slot}`; }

  private async readSlot(slot: number): Promise<SlotRead> {
    let bytes: Uint8Array;
    try {
      const info = await file.readFileInfo(this.slotUri(slot));
      if (!natural(info.length) || info.length > MAX_BASE_BYTES + FRAME_OVERHEAD + MAX_LOG_BYTES + TERMINATOR_BYTES)
        throw new StorageFormatError("Invalid transfer journal size");
      bytes = await file.readArrayBuffer(this.slotUri(slot), 0, info.length);
      if (bytes.length !== info.length) throw new StorageFormatError("Short transfer journal read");
    } catch (error) {
      if (file.isFileNotFound(error)) return { kind: "missing" };
      throw error;
    }
    const base = decode(bytes, 0);
    if (!base) return { kind: "incomplete" };
    if (!record(base.value) || base.value.kind !== "base" ||
        !natural(base.value.generation) || base.value.generation === 0 ||
        typeof base.value.token !== "string" || !basicState(base.value.state)) throw new StorageFormatError("Invalid transfer checkpoint");
    const loaded: LoadedSlot = {
      slot, generation: base.value.generation, token: base.value.token,
      sequence: 0, checksum: base.checksum, offset: base.end, baseEnd: base.end,
      state: base.value.state
    };
    while (loaded.sequence < MAX_LOG_RECORDS && loaded.offset < bytes.length) {
      const frame = decode(bytes, loaded.offset);
      if (!frame) {
        loaded.dirty = bytes.length - loaded.offset < TERMINATOR_BYTES ||
          !bytes.slice(loaded.offset, loaded.offset + TERMINATOR_BYTES).every(value => value === 0);
        break;
      }
      if (frame.end - loaded.baseEnd > MAX_LOG_BYTES) throw new StorageFormatError("Transfer log exceeds limit");
      const delta = frame.value;
      if (!record(delta) || delta.kind !== "delta" || delta.generation !== loaded.generation ||
          delta.token !== loaded.token || delta.sequence !== loaded.sequence + 1 ||
          delta.previous !== loaded.checksum || !record(delta.header) ||
          !natural(delta.fileLength) || !Number.isInteger(delta.fileIndex))
        throw new StorageFormatError("Corrupt transfer delta chain");
      const index = delta.fileIndex;
      const files = loaded.state.files;
      if (index < -1 || index > files.length ||
          delta.fileLength !== files.length + (index === files.length ? 1 : 0) ||
          (index >= 0 && !record(delta.entry))) throw new StorageFormatError("Invalid transfer delta file");
      const next = { ...delta.header, files };
      if (!basicState(next)) throw new StorageFormatError("Invalid transfer delta header");
      if (index >= 0) files[index] = delta.entry;
      loaded.state = next;
      loaded.sequence++;
      loaded.checksum = frame.checksum;
      loaded.offset = frame.end;
    }
    if (loaded.sequence === MAX_LOG_RECORDS && decode(bytes, loaded.offset))
      throw new StorageFormatError("Transfer log exceeds record limit");
    // Ignore incomplete tails, but checkpoint on retry rather than overwriting
    // possibly stale bytes. This also protects retries that fail a second time.
    return { kind: "valid", value: loaded };
  }

  async load(): Promise<TransferState | null> {
    if (this.loaded) return this.state;
    if (this.busy) throw new Error("Concurrent transfer storage operation");
    this.busy = true;
    try {
      const left = await this.readSlot(0);
      const right = await this.readSlot(1);
      const leftValue = left.value;
      const rightValue = right.value;
      this.active = !leftValue ? rightValue || null : !rightValue ? leftValue :
        (rightValue.generation > leftValue.generation ? rightValue : leftValue);
      if (this.active) this.state = this.active.state;
      else {
        // Only a first migration may use legacy after an incomplete checkpoint.
        // Two existing unusable slots must not resurrect an obsolete snapshot.
        if (left.kind !== "missing" && right.kind !== "missing")
          throw new StorageFormatError("No valid transfer checkpoint");
        const text = await file.readOptionalText(this.snapshotUri);
        if (text !== null) {
          const parsed: unknown = JSON.parse(text);
          if (!basicState(parsed)) throw new Error("Invalid legacy transfer snapshot");
          this.state = parsed;
        }
      }
      this.loaded = true;
      return this.state;
    } finally { this.busy = false; }
  }

  /**
   * fileIndex >= 0 replaces/appends exactly one file; -1 changes only headers.
   * Omit fileIndex for replace/reset or any multi-file change (new checkpoint).
   * With changedFile, next.files is the old array; its indexed entry is committed
   * by this store only after native success. Checkpoints materialize a copy first.
   * Hints are authoritative: unchanged files are never scanned or serialized.
   */
  async save(next: TransferState, fileIndex?: number, changedFile?: StoredFile): Promise<void> {
    if (!this.loaded) await this.load();
    if (this.busy) throw new Error("Concurrent transfer storage operation");
    this.busy = true;
    try {
      if (!basicState(next)) throw new Error("Invalid transfer state");
      const active = this.active;
      if (fileIndex !== undefined && (!Number.isInteger(fileIndex) || fileIndex < -1 || fileIndex > next.files.length))
        throw new Error("Invalid transfer file change hint");
      if (changedFile !== undefined && (fileIndex === undefined || fileIndex < 0))
        throw new Error("Changed file requires a nonnegative file index");
      const entry = changedFile === undefined ?
        (fileIndex !== undefined && fileIndex >= 0 ? next.files[fileIndex] : undefined) : changedFile;
      const fileLength = changedFile !== undefined && fileIndex === next.files.length ?
        next.files.length + 1 : next.files.length;
      if (fileIndex !== undefined && this.state) {
        if (!Number.isInteger(fileIndex) || fileIndex < -1 || fileIndex > this.state.files.length ||
            fileLength !== this.state.files.length + (fileIndex === this.state.files.length ? 1 : 0) ||
            (fileIndex >= 0 && !entry))
          throw new Error("Invalid transfer file change hint");
      }
      if (active && !active.dirty && fileIndex !== undefined) {
        const delta: DeltaRecord = {
          kind: "delta", generation: active.generation, token: active.token,
          sequence: active.sequence + 1, previous: active.checksum,
          header: header(next), fileIndex, fileLength,
          ...(fileIndex >= 0 ? { entry } : {})
        };
        const frame = encode(delta);
        if (active.sequence < MAX_LOG_RECORDS &&
            active.offset - active.baseEnd + frame.bytes.length - TERMINATOR_BYTES <= MAX_LOG_BYTES) {
          try { await file.writeArrayBuffer(this.slotUri(active.slot), frame.bytes, active.offset); }
          catch (error) { active.dirty = true; throw error; }
          if (changedFile !== undefined) next.files[fileIndex!] = changedFile;
          active.offset += frame.bytes.length - TERMINATOR_BYTES;
          active.sequence++;
          active.checksum = frame.checksum;
          active.state = next;
          this.state = next;
          return;
        }
      }
      if (changedFile !== undefined) {
        // Only checkpoints copy the array; ordinary appends serialize one entry.
        const files = next.files.slice();
        files[fileIndex!] = changedFile;
        await this.checkpoint({ ...next, files });
        next.files[fileIndex!] = changedFile;
        this.state = next;
        this.active!.state = next;
      } else await this.checkpoint(next);
    } finally { this.busy = false; }
  }

  private async checkpoint(next: TransferState): Promise<void> {
    const generation = this.active ? this.active.generation + 1 : 1;
    if (!Number.isSafeInteger(generation)) throw new Error("Transfer generation exhausted");
    const slot = this.active ? 1 - this.active.slot : 0;
    const token = `${generation}-${Date.now()}-${Math.random().toString(36).slice(2)}`;
    const base: BaseRecord = { kind: "base", generation, token, state: next };
    const frame = encode(base);
    // Delete only the inactive slot. Never rely on native writes truncating files.
    // A failed delete/write leaves the active checkpoint and legacy snapshot intact.
    try { await file.deleteFile(this.slotUri(slot)); }
    catch (error) { if (!file.isFileNotFound(error)) throw error; }
    await file.writeArrayBuffer(this.slotUri(slot), frame.bytes, 0);
    this.active = {
      slot, generation, token, sequence: 0, checksum: frame.checksum,
      offset: frame.bytes.length - TERMINATOR_BYTES,
      baseEnd: frame.bytes.length - TERMINATOR_BYTES, state: next
    };
    this.state = next;
  }
}
