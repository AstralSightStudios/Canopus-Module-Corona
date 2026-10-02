export interface ModuleControlFileApi {
  readOptionalText(uri: string): Promise<string | null>;
  writeText(uri: string, text: string): Promise<void>;
}

// Retain the file-interface name for existing reload callers, not the old paths.
export type ReloadSignalFileApi = ModuleControlFileApi;

export interface ModuleStatus {
  state: "running" | "config_error";
  configError: number;
  activeRuleCount: number;
  refreshPending: boolean;
}

export interface ModuleStatusOutcome {
  status: ModuleStatus | null;
  reason: "response" | "timeout" | "read_error" | "write_error" | "cancelled";
  message: string;
}

export interface ReloadResult {
  version: number;
  result: number;
  pending: boolean;
  changed: number;
  status: ModuleStatus | null;
}

export interface ReloadOutcome {
  successful: boolean;
  message: string;
  status: ModuleStatus | null;
  reason?: "timeout" | "read_error" | "rejected" | "pending" | "cancelled";
}

export type ReloadOperation = "resource-hook-reload-v2" | "resource-hook-reload-v1";
const REQUEST_URI = "internal://files/control.request";
const RESPONSE_URI = "internal://files/control.response";
const STATUS_OPERATION = "resource-hook-status-v1";
const RELOAD_OPERATION: ReloadOperation = "resource-hook-reload-v2";
const VALID_ID = /^[A-Za-z0-9._-]{1,64}$/;

interface ControlSession {
  active: boolean;
  latest: ModuleStatusOutcome | null;
  initial: Promise<ModuleStatusOutcome> | null;
  sleepers: Set<() => void>;
}
function newSession(): ControlSession {
  return { active: true, latest: null, initial: null, sleepers: new Set() };
}
interface ModuleControlRuntime {
  session: ControlSession;
  operationTail: Promise<void>;
  activeOperationSession: ControlSession | null;
  nextId: number;
}
type ControlGlobals = typeof globalThis & { resourceHookModuleControl?: ModuleControlRuntime };

/** Vela embeds separate module copies in app/page bundles. Resolve the shared
 * app-owned runtime on every call, never retain a bundle-local session/queue.
 * Lazily create it for standalone callers; app onCreate starts the real session.
 */
function controlRuntime(): ModuleControlRuntime {
  const globals = globalThis as ControlGlobals;
  if (!globals.resourceHookModuleControl) globals.resourceHookModuleControl = {
    session: newSession(), operationTail: Promise.resolve(), activeOperationSession: null, nextId: 0,
  };
  return globals.resourceHookModuleControl;
}

function current(owner: ControlSession): boolean {
  return owner.active && owner === controlRuntime().session;
}
function cancel(owner: ControlSession): void {
  owner.active = false;
  owner.sleepers.forEach(wake => wake());
  owner.sleepers.clear();
}

/** Start a fresh app session; page creation must NOT call this. */
export function initializeModuleControlSession(): void {
  const runtime = controlRuntime();
  cancel(runtime.session);
  runtime.session = newSession();
}

/** In-flight I/O may finish, but can no longer write/cache into a new session. */
export function destroyModuleControlSession(): void {
  // Retain the inactive session and transport queue/ID counter globally. Deleting
  // them would let a recreated bundle resurrect a session or overlap old I/O.
  cancel(controlRuntime().session);
}

/** One shared slot: wrap an entire send + wait, never just the individual calls.
 * queryModuleStatus owns this queue already and must not be nested inside it.
 * Keep this queue across sessions so outstanding file I/O cannot overlap.
 */
export function withModuleControlOperation<T>(operation: () => Promise<T>): Promise<T> {
  const runtime = controlRuntime();
  const owner = runtime.session;
  const result = runtime.operationTail.then(async () => {
    if (!current(owner)) throw new Error("模块控制会话已结束");
    runtime.activeOperationSession = owner;
    try {
      return await operation();
    } finally {
      runtime.activeOperationSession = null;
    }
  });
  runtime.operationTail = result.then(() => undefined, () => undefined);
  return result;
}

function requestLine(operation: string, id: string): string {
  return `${operation}\tng.lst.corona\t${id}\n`;
}

/** Verify the complete text payload and exact echo, not native file padding. */
function responseFields(text: string, operation: string, id: string): string[] | null {
  if (!VALID_ID.test(id)) return null;
  // Text APIs may omit native NUL padding or preserve it in the returned string.
  // Both representations contain the same three-line checksummed payload.
  const record = text.split("\0", 1)[0];
  if (/[^\x00-\x7f]/.test(record)) return null;
  const lines = record.split("\n");
  if (lines.length !== 4 || lines[3] !== "" ||
      `${lines[0]}\n` !== requestLine(operation, id)) return null;
  const checksum = unsignedNumber(lines[2], 4294967295);
  if (checksum === null) return null;
  const payload = `${lines[0]}\n${lines[1]}\n`;
  let hash = 2166136261;
  for (let i = 0; i < payload.length; i++)
    hash = Math.imul(hash ^ payload.charCodeAt(i), 16777619) >>> 0;
  return checksum === hash ? lines[1].split("\t") : null;
}
function unsignedNumber(text: string, maximum: number): number | null {
  if (!/^(0|[1-9]\d*)$/.test(text)) return null;
  const value = Number(text);
  return Number.isInteger(value) && value <= maximum ? value : null;
}
function signedNumber(text: string, maximum = 2147483647): number | null {
  if (!/^(0|-?[1-9]\d*)$/.test(text)) return null;
  const value = Number(text);
  return Number.isInteger(value) && value >= -2147483648 && value <= maximum ? value : null;
}
function statusFields(state: string, error: string, count: string, pending: string): ModuleStatus | null {
  const configError = signedNumber(error);
  const activeRuleCount = unsignedNumber(count, 256);
  if (configError === null || activeRuleCount === null || !/^[01]$/.test(pending) ||
      (state !== "running" && state !== "config_error") ||
      (state === "running" ? configError !== 0 : configError >= 0)) return null;
  return { state, configError, activeRuleCount, refreshPending: pending === "1" };
}

export function parseModuleStatus(text: string, requestId: string): ModuleStatus | null {
  const fields = responseFields(text, STATUS_OPERATION, requestId);
  if (!fields || fields.length !== 6 || fields[0] !== "RHST1" || fields[1] !== "1") return null;
  return statusFields(fields[2], fields[3], fields[4], fields[5]);
}

/** v1 is available to compatible controllers only by explicitly selecting its
 * operation. A v1 echo can never satisfy a Manager v2 request (or vice versa).
 */
export function parseReloadResult(
  text: string, revision: string, operation: ReloadOperation = RELOAD_OPERATION,
): ReloadResult | null {
  if (operation !== RELOAD_OPERATION && operation !== "resource-hook-reload-v1") return null;
  const fields = responseFields(text, operation, revision);
  if (!fields) return null;
  const modern = operation === RELOAD_OPERATION;
  if (modern ? fields.length !== 8 || fields[0] !== "RHRS2" || fields[1] !== "1" :
      fields.length !== 5 || fields[0] !== "RHRS1" || !/^[56]$/.test(fields[1])) return null;
  const result = signedNumber(fields[2], 1);
  const changed = unsignedNumber(fields[4], 4294967295);
  if (result === null || changed === null || !/^[01]$/.test(fields[3]) ||
      (result === 1 && fields[3] !== "1")) return null;
  const status = modern ? statusFields(fields[5], fields[6], fields[7], fields[3]) : null;
  if (modern && !status) return null;
  return { version: Number(fields[1]), result, pending: fields[3] === "1", changed, status };
}

function remember(owner: ControlSession, outcome: ModuleStatusOutcome): ModuleStatusOutcome {
  if (current(owner)) owner.latest = outcome;
  return outcome;
}
function cancelled(): ModuleStatusOutcome {
  return { status: null, reason: "cancelled", message: "模块控制会话已结束" };
}
function statusMessage(status: ModuleStatus): string {
  return status.state === "config_error" ? `模块配置错误（${status.configError}）` :
    status.refreshPending ? "模块在线，资源刷新待完成" : "模块运行中";
}
function pause(owner: ControlSession, intervalMs: number): Promise<void> {
  if (!current(owner) || intervalMs <= 0) return Promise.resolve();
  return new Promise(resolve => {
    const wake = () => {
      clearTimeout(timer);
      owner.sleepers.delete(wake);
      resolve();
    };
    const timer = setTimeout(wake, intervalMs);
    owner.sleepers.add(wake);
  });
}
function pollLimit(attempts: number): number {
  // Even erroneous caller input cannot cause an unbounded poll.
  return Number.isFinite(attempts) ? Math.max(0, Math.floor(attempts)) : 0;
}

/** A fresh, read-only query. Owns the queue, prepares a NONEMPTY response file. */
export async function queryModuleStatus(
  file: ModuleControlFileApi, attempts = 5, intervalMs = 1000,
): Promise<ModuleStatusOutcome> {
  const runtime = controlRuntime();
  const owner = runtime.session;
  try {
    return await withModuleControlOperation(async () => {
      const id = `status-${Date.now().toString(36)}-${(++runtime.nextId).toString(36)}`;
      try {
        await file.writeText(RESPONSE_URI, `pending\t${id}\n`);
        if (!current(owner)) return cancelled();
        await file.writeText(REQUEST_URI, requestLine(STATUS_OPERATION, id));
      } catch (_error) {
        if (!current(owner)) return cancelled();
        return remember(owner, { status: null, reason: "write_error", message: "无法写入模块状态请求" });
      }
      const limit = pollLimit(attempts);
      for (let attempt = 0; attempt < limit; attempt++) {
        if (!current(owner)) return cancelled();
        let text: string | null;
        try {
          text = await file.readOptionalText(RESPONSE_URI);
        } catch (_error) {
          if (!current(owner)) return cancelled();
          return remember(owner, { status: null, reason: "read_error", message: "无法读取模块响应" });
        }
        if (!current(owner)) return cancelled();
        const status = text === null ? null : parseModuleStatus(text, id);
        if (status) return remember(owner, { status, reason: "response", message: statusMessage(status) });
        if (attempt + 1 < limit) await pause(owner, intervalMs);
      }
      if (!current(owner)) return cancelled();
      return remember(owner, { status: null, reason: "timeout", message: "未收到模块响应，无法确认运行状态" });
    });
  } catch (error) {
    if (!current(owner)) return cancelled();
    throw error;
  }
}

/** One query on the first homepage entry per app session. Subsequent entries
 * receive the latest cached outcome, including status learned from a reload.
 */
export function getInitialModuleStatus(file: ModuleControlFileApi): Promise<ModuleStatusOutcome> {
  const owner = controlRuntime().session;
  if (!owner.active) return Promise.resolve(cancelled());
  if (!owner.initial) owner.initial = queryModuleStatus(file);
  return owner.latest ? Promise.resolve(owner.latest) : owner.initial;
}

/** Low-level; caller owns the send + wait queue reservation. Reload remains
 * possible when optional response preparation fails, but absence is not success.
 */
export async function sendReloadSignal(revision: string, file: ModuleControlFileApi): Promise<void> {
  if (!VALID_ID.test(revision)) throw new Error("重载版本标识无效");
  const runtime = controlRuntime();
  const owner = runtime.activeOperationSession || runtime.session;
  if (!current(owner)) throw new Error("模块控制会话已结束");
  try {
    await file.writeText(RESPONSE_URI, `pending\t${revision}\n`);
  } catch (_error) {
    // A reload request may still be processed without a receipt file.
  }
  if (!current(owner)) throw new Error("模块控制会话已结束");
  await file.writeText(REQUEST_URI, requestLine(RELOAD_OPERATION, revision));
}

/** Low-level; no auto-lock (send + wait must share the caller's reservation). */
export async function waitForReloadOutcome(
  revision: string, file: Pick<ModuleControlFileApi, "readOptionalText">,
  attempts = 30, intervalMs = 1000, operation: ReloadOperation = RELOAD_OPERATION,
): Promise<ReloadOutcome> {
  const runtime = controlRuntime();
  const owner = runtime.activeOperationSession || runtime.session;
  let latestStatus: ModuleStatus | null = null;
  let sawPending = false;
  const limit = pollLimit(attempts);
  const interrupted = (): ReloadOutcome => ({
    successful: false, status: latestStatus, reason: "cancelled",
    message: "模块控制会话已结束",
  });
  for (let attempt = 0; attempt < limit; attempt++) {
    if (!current(owner)) return interrupted();
    let text: string | null;
    try {
      text = await file.readOptionalText(RESPONSE_URI);
    } catch (_error) {
      if (!current(owner)) return interrupted();
      const outcome: ReloadOutcome = {
        successful: false, status: latestStatus, reason: "read_error", message: "无法读取模块响应",
      };
      if (!latestStatus && !owner.latest)
        remember(owner, { status: null, reason: "read_error", message: outcome.message });
      return outcome;
    }
    if (!current(owner)) return interrupted();
    const result = text === null ? null : parseReloadResult(text, revision, operation);
    if (result) {
      latestStatus = result.status;
      if (latestStatus) remember(owner, {
        status: latestStatus, reason: "response", message: statusMessage(latestStatus),
      });
      if (result.result < 0) return {
        successful: false, status: latestStatus, reason: "rejected", message: `模块拒绝重载（${result.result}）`,
      };
      if (!result.pending && result.result === 0) return {
        successful: true, status: latestStatus,
        message: result.changed > 0 ? `重载完成，更新 ${result.changed} 项资源` : "模块已响应，重载完成",
      };
      sawPending = true;
    }
    if (attempt + 1 < limit) await pause(owner, intervalMs);
  }
  if (!current(owner)) return interrupted();
  const outcome: ReloadOutcome = {
    successful: false, status: latestStatus, reason: sawPending ? "pending" : "timeout",
    message: sawPending ? "模块已响应，资源刷新仍待完成" : "未收到模块响应",
  };
  if (!sawPending)
    remember(owner, { status: null, reason: "timeout", message: outcome.message });
  return outcome;
}

export async function waitForReload(
  revision: string, file: Pick<ModuleControlFileApi, "readOptionalText">,
  attempts = 30, intervalMs = 1000,
): Promise<string> {
  return (await waitForReloadOutcome(revision, file, attempts, intervalMs)).message;
}
