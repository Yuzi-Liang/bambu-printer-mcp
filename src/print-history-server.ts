#!/usr/bin/env node
import { execFile } from "node:child_process";
import { constants, readFileSync } from "node:fs";
import { copyFile, mkdtemp, readFile, realpath, rm } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { CallToolRequestSchema, ErrorCode, ListToolsRequestSchema, McpError, type Tool } from "@modelcontextprotocol/sdk/types.js";

const run = promisify(execFile);
const tool: Tool = {
  name: "list_account_print_history",
  description: "Read a page of past print jobs from the Bambu account signed into the local Windows Bambu Studio installation. Returns titles, status, printers and timestamps. Includes older jobs sent outside the MCP if the Bambu account recorded them.",
  inputSchema: {
    type: "object",
    additionalProperties: false,
    properties: {
      offset: { type: "integer", minimum: 0, maximum: 2_147_483_647, default: 0 },
      limit: { type: "integer", minimum: 1, maximum: 50, default: 20 },
    },
  },
  annotations: { readOnlyHint: true, destructiveHint: false, idempotentHint: true, openWorldHint: true },
};

function nonemptyText(value: unknown): string | null {
  return typeof value === "string" && value.trim() ? value : null;
}

function studioCountry(region: unknown): string {
  switch (region) {
    case "CHN":
    case "China":
      return "CN";
    case "USA":
    case "Europe":
    case "North America":
      return "US";
    default:
      return "Others";
  }
}

function normalizeTask(task: Record<string, unknown>) {
  const statusCode = typeof task.status === "number" && Number.isInteger(task.status) ? task.status : null;
  const status = statusCode === 2 ? "success" :
    statusCode === 3 ? "failed" :
    statusCode === 1 || statusCode === 4 ? "printing" : "unknown";
  const startTime = nonemptyText(task.startTime);
  const endTime = nonemptyText(task.endTime);
  const elapsed = startTime && endTime ? (Date.parse(endTime) - Date.parse(startTime)) / 1000 : NaN;

  return {
    id: nonemptyText(task.id) ?? (Number.isSafeInteger(task.id) ? String(task.id) : null),
    title: nonemptyText(task.designTitle) ?? nonemptyText(task.title),
    status,
    status_code: statusCode,
    printer_name: nonemptyText(task.deviceName),
    printer_model: nonemptyText(task.deviceModel),
    start_time: startTime,
    end_time: endTime,
    elapsed_seconds: Number.isFinite(elapsed) && elapsed >= 0 ? elapsed : null,
  };
}

async function readHistory(offset: number, limit: number, signal: AbortSignal) {
  if (process.platform !== "win32" || !process.env.APPDATA || !process.env.LOCALAPPDATA) {
    throw new Error("Print history requires the signed-in Windows installation of Bambu Studio.");
  }
  const studio = process.env.BAMBU_HISTORY_STUDIO_PROFILE ?? path.join(process.env.APPDATA, "BambuStudio");
  const certDirectory = process.env.BAMBU_HISTORY_CERT_DIR;
  if (!certDirectory || !path.isAbsolute(certDirectory)) {
    throw new Error("Set BAMBU_HISTORY_CERT_DIR to Bambu Studio's absolute resources/cert directory.");
  }
  const helper = fileURLToPath(new URL("../temp/print-history/Release/bambu-print-history.exe", import.meta.url));
  const configText = (await readFile(path.join(studio, "BambuStudio.conf"), "utf8")).replace(/^\uFEFF/, "");
  // Studio appends a checksum comment after its JSON configuration.
  const config = JSON.parse(configText.slice(0, configText.lastIndexOf("}") + 1));
  const country = studioCountry(config.app?.region);
  const temp = await realpath(path.join(process.env.LOCALAPPDATA, "Temp"));
  const profile = await mkdtemp(path.join(temp, "bambu-print-history-"));

  try {
    await copyFile(path.join(studio, "BambuNetworkEngine.conf"),
      path.join(profile, "BambuNetworkEngine.conf"), constants.COPYFILE_EXCL);
    const { stdout } = await run(helper, [path.resolve(studio, "plugins", "bambu_networking.dll"),
      profile, certDirectory, country, String(offset), String(limit)], {
      signal,
      timeout: 25_000,
      maxBuffer: 2 * 1024 * 1024,
      windowsHide: true,
    }).catch((error: NodeJS.ErrnoException & { killed?: boolean }) => {
      // The plug-in can log account metadata. Do not forward its output on failure.
      if (Number(error.code) === 3) {
        throw new Error("Bambu Studio has no usable saved account session. Sign in to Bambu Studio and retry.");
      }
      if (error.code === "ENOENT") {
        throw new Error("The print-history helper is missing. Build its Windows Release target first.");
      }
      throw new Error(error.killed ? "Bambu account history query timed out." :
        "Bambu account history query failed. Check the Bambu Studio network plug-in and login session.");
    });
    const page = JSON.parse(stdout);
    if (!page || !Number.isSafeInteger(page.total) || page.total < 0 || !Array.isArray(page.hits) ||
      page.hits.length > limit || page.hits.some((hit: unknown) => !hit || typeof hit !== "object" || Array.isArray(hit))) {
      throw new Error("The Bambu account returned an invalid print-history page.");
    }
    const tasks = page.hits.map(normalizeTask);
    const next = offset + tasks.length;
    return { source: "bambu_cloud", total: page.total, offset, limit,
      next_offset: tasks.length > 0 && next < page.total ? next : null, tasks };
  } finally {
    const resolved = await realpath(profile);
    if (path.dirname(resolved) !== temp || !path.basename(resolved).startsWith("bambu-print-history-")) {
      throw new Error("Could not verify the temporary history profile before removing it.");
    }
    await rm(resolved, { recursive: true, force: true, maxRetries: 3 });
  }
}

const { version } = JSON.parse(readFileSync(new URL("../package.json", import.meta.url), "utf8"));
const server = new Server({ name: "bambu-print-history", version }, { capabilities: { tools: {} } });
server.setRequestHandler(ListToolsRequestSchema, async () => ({ tools: [tool] }));
server.setRequestHandler(CallToolRequestSchema, async ({ params }, { signal }) => {
  if (params.name !== tool.name) {
    throw new McpError(ErrorCode.MethodNotFound, "Unknown print-history tool.");
  }
  const args = params.arguments ?? {};
  const { offset = 0, limit = 20 } = args;
  if (Object.keys(args).some((key) => key !== "offset" && key !== "limit") ||
    typeof offset !== "number" || !Number.isInteger(offset) || offset < 0 || offset > 2_147_483_647 ||
    typeof limit !== "number" || !Number.isInteger(limit) || limit < 1 || limit > 50) {
    throw new McpError(ErrorCode.InvalidParams, "Use integer offset (0 or greater) and limit (1 to 50).");
  }
  try {
    const data = await readHistory(offset, limit, signal);
    return { content: [{ type: "text", text: JSON.stringify(data) }], structuredContent: data };
  } catch (error) {
    const message = error instanceof SyntaxError ? "The saved Studio configuration or account response is invalid." :
      error instanceof Error ? error.message : "Bambu account history query failed.";
    return { isError: true, content: [{ type: "text", text: message }] };
  }
});

await server.connect(new StdioServerTransport());
