import { file as systemFile } from "./import.js";

/** Callback API subset matching the Vela runtime used by this app. */
type Callbacks<T> = {
  success: (data: T) => void;
  fail: (data: unknown, code: number) => void;
};

export interface FileInfo {
  length: number;
  type?: string;
}

export interface FileApi {
  readText(
    options: Callbacks<{ text: string }> & { uri: string; encoding?: string },
  ): void;
  writeText(
    options: Callbacks<void> & { uri: string; text: string; encoding?: string },
  ): void;
  readArrayBuffer(
    options: Callbacks<{ buffer: Uint8Array }> & {
      uri: string;
      position?: number;
      length?: number;
    },
  ): void;
  writeArrayBuffer(
    options: Callbacks<void> & {
      uri: string;
      buffer: Uint8Array;
      position?: number;
    },
  ): void;
  get(options: Callbacks<FileInfo> & { uri: string }): void;
  mkdir(options: Callbacks<void> & { uri: string; recursive?: boolean }): void;
  delete(options: Callbacks<void> & { uri: string }): void;
}

export interface FileOperationError extends Error {
  operation: string;
  uri: string;
  code: number;
  data: unknown;
}

/** Only 301 means a missing file on reads; 300 is an I/O failure. */
export function isFileNotFound(error: unknown): boolean {
  return (
    typeof error === "object" &&
    error !== null &&
    "code" in error &&
    error.code === 301
  );
}

// ux-types 1.7.0 declares writeArrayBuffer.buffer as Uint8Array[], but the
// Vela runtime expects one Uint8Array. Keep the correction at this boundary.
const api = systemFile as unknown as FileApi;

function invoke<T>(
  operation: string,
  uri: string,
  start: (callbacks: Callbacks<T>) => void,
): Promise<T> {
  // The Promise executor also preserves synchronous native exceptions as rejections.
  return new Promise<T>((resolve, reject) =>
    start({
      success: resolve,
      fail: (data, code) => {
        const error = new Error(
          `${operation} ${uri} 失败（code ${code}）：${String(data)}`,
        ) as FileOperationError;
        error.operation = operation;
        error.uri = uri;
        error.code = code;
        error.data = data;
        reject(error);
      },
    }),
  );
}

export async function readText(uri: string): Promise<string> {
  const result = await invoke<{ text: string }>("读取", uri, (callbacks) =>
    api.readText({ uri, encoding: "UTF-8", ...callbacks }),
  );
  return result.text;
}

export function writeText(uri: string, text: string): Promise<void> {
  return invoke<void>("写入", uri, (callbacks) =>
    api.writeText({ uri, text, encoding: "UTF-8", ...callbacks }),
  );
}

export async function readOptionalText(uri: string): Promise<string | null> {
  try {
    return await readText(uri);
  } catch (error) {
    if (isFileNotFound(error)) return null;
    throw error;
  }
}

export async function readArrayBuffer(
  uri: string,
  position?: number,
  length?: number,
): Promise<Uint8Array> {
  const result = await invoke<{ buffer: Uint8Array }>(
    "读取",
    uri,
    (callbacks) => {
      const options: Parameters<FileApi["readArrayBuffer"]>[0] = {
        uri,
        ...callbacks,
      };
      // Omit unspecified fields; passing undefined can change native validation.
      if (position !== undefined) options.position = position;
      if (length !== undefined) options.length = length;
      api.readArrayBuffer(options);
    },
  );
  return result.buffer;
}

export function writeArrayBuffer(
  uri: string,
  buffer: Uint8Array,
  position?: number,
): Promise<void> {
  return invoke<void>("写入", uri, (callbacks) => {
    const options: Parameters<FileApi["writeArrayBuffer"]>[0] = {
      uri,
      buffer,
      ...callbacks,
    };
    if (position !== undefined) options.position = position;
    api.writeArrayBuffer(options);
  });
}

export async function readOptionalArrayBuffer(
  uri: string,
): Promise<Uint8Array | null> {
  try {
    return await readArrayBuffer(uri);
  } catch (error) {
    if (isFileNotFound(error)) return null;
    throw error;
  }
}

export async function readFileInfo(uri: string): Promise<FileInfo> {
  const result = await invoke<FileInfo>("检查文件", uri, (callbacks) =>
    api.get({ uri, ...callbacks }),
  );
  return { length: result.length, type: result.type };
}

export function makeDirectory(uri: string, recursive = true): Promise<void> {
  return invoke<void>("创建目录", uri, (callbacks) =>
    api.mkdir({ uri, recursive, ...callbacks }),
  );
}

export function deleteFile(uri: string): Promise<void> {
  return invoke<void>("删除", uri, (callbacks) =>
    api.delete({ uri, ...callbacks }),
  );
}
