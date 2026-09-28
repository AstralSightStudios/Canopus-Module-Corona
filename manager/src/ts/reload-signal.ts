export interface ReloadSignalFileApi {
  readOptionalText(uri: string): Promise<string | null>;
  writeText(uri: string, text: string): Promise<void>;
}

export interface ReloadResult {
  version: number;
  result: number;
  pending: boolean;
  changed: number;
}

const RELOAD_REQUEST_URI = "internal://files/reload.request";
const RELOAD_RESULT_URI = "internal://files/reload.result";
const RELOAD_PREFIX = "resource-hook-reload-v1\tng.lst.corona\t";
const VALID_REVISION = /^[A-Za-z0-9._-]{1,64}$/;

/** Writes the optional result placeholder, then signals the resident hook. */
export async function sendReloadSignal(
  revision: string,
  file: ReloadSignalFileApi,
): Promise<void> {
  if (!VALID_REVISION.test(revision)) throw new Error("重载版本标识无效");

  try {
    // The hook opens the result file without create flags; prepare it when possible.
    await file.writeText(RELOAD_RESULT_URI, `pending\t${revision}\n`);
  } catch (_error) {
    // Result reporting is optional and must not block the reload request.
  }

  await file.writeText(RELOAD_REQUEST_URI, `${RELOAD_PREFIX}${revision}\n`);
}

/** Parses the checksummed 256-byte response record written by the hook. */
export function parseReloadResult(text: string, revision: string): ReloadResult | null {
  const record = text.split("\0", 1)[0];
  const lines = record.split("\n");
  if (lines.length !== 4 || lines[3] !== "" || lines[0] !== `${RELOAD_PREFIX}${revision}`)
    return null;

  const fields = /^RHRS1\t([56])\t(-?\d+)\t([01])\t(\d+)$/.exec(lines[1]);
  if (!fields || !/^\d+$/.test(lines[2])) return null;

  let hash = 2166136261;
  const payload = `${lines[0]}\n${lines[1]}\n`;
  for (let i = 0; i < payload.length; i++)
    hash = Math.imul(hash ^ payload.charCodeAt(i), 16777619) >>> 0;
  if (Number(lines[2]) !== hash) return null;

  const result = Number(fields[2]);
  const changed = Number(fields[4]);
  if (!Number.isInteger(result) || result < -2147483648 || result > 1 ||
      !Number.isInteger(changed) || changed < 0 || changed > 4294967295)
    return null;
  return { version: Number(fields[1]), result, pending: fields[3] === "1", changed };
}

/** Waits for a response matching this exact request revision. */
export async function waitForReload(
  revision: string,
  file: Pick<ReloadSignalFileApi, "readOptionalText">,
  attempts = 30,
  intervalMs = 1000,
): Promise<string> {
  for (let attempt = 0; attempt < attempts; attempt++) {
    let text: string | null;
    try {
      text = await file.readOptionalText(RELOAD_RESULT_URI);
    } catch (_error) {
      return "无法读取模块响应";
    }

    const result = text === null ? null : parseReloadResult(text, revision);
    if (result) {
      if (result.result < 0) return `模块拒绝重载（${result.result}）`;
      if (!result.pending && result.result === 0)
        return result.changed > 0 ? `重载完成，更新 ${result.changed} 项资源` : "模块已响应，重载完成";
    }

    if (attempt + 1 < attempts && intervalMs > 0)
      await new Promise<void>((resolve) => setTimeout(resolve, intervalMs));
  }
  return "未收到模块响应";
}
