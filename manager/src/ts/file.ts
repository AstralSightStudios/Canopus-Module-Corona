import { file as systemFile } from "./import.js";

/** Callback API subset matching the Vela runtime used by this app. */
type Callbacks<T> = {
  success: (data: T) => void;
  fail: (data: unknown, code: number) => void;
};

export interface FileInfo {
  uri?: string;
  length: number;
  type?: string;
  subFiles?: FileInfo[];
}

export interface FileListEntry {
  uri: string;
  length: number;
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
  copy(options: Callbacks<string> & { srcUri: string; dstUri: string }): void;
  get(options: Callbacks<FileInfo> & { uri: string; recursive?: boolean }): void;
  list(options: Callbacks<{ fileList: FileListEntry[] }> & { uri: string }): void;
  mkdir(options: Callbacks<void> & { uri: string; recursive?: boolean }): void;
  delete(options: Callbacks<void> & { uri: string }): void;
  rmdir(options: Callbacks<void> & { uri: string; recursive?: boolean }): void;
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

/** Vela copies in its native worker and reports completion with a destination URI. */
export async function copyFile(srcUri: string, dstUri: string): Promise<void> {
  await invoke<string>("复制", dstUri, callbacks =>
    api.copy({ srcUri, dstUri, ...callbacks }),
  );
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

export async function readFileInfo(uri: string, recursive = false): Promise<FileInfo> {
  const result = await invoke<FileInfo>("检查文件", uri, (callbacks) => {
    const options: Parameters<FileApi["get"]>[0] = { uri, ...callbacks };
    if (recursive) options.recursive = true;
    api.get(options);
  });
  return {
    ...(typeof result.uri === "string" ? { uri: result.uri } : {}),
    length: result.length,
    type: result.type,
    ...(Array.isArray(result.subFiles) ? { subFiles: result.subFiles } : {})
  };
}

export async function listDirectory(uri: string): Promise<FileListEntry[]> {
  const result = await invoke<{ fileList: FileListEntry[] }>("列出目录", uri, (callbacks) =>
    api.list({ uri, ...callbacks }),
  );
  if (!Array.isArray(result.fileList) || result.fileList.some(entry =>
    !entry || typeof entry.uri !== "string" || typeof entry.length !== "number"))
    throw new Error(`目录清单格式无效：${uri}`);
  return result.fileList.map(entry => ({ uri: entry.uri, length: entry.length }));
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

export function removeDirectory(uri: string): Promise<void> {
  return invoke<void>("删除目录", uri, (callbacks) =>
    api.rmdir({ uri, recursive: true, ...callbacks }),
  );
}
