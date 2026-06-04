export type Metrics = {
  ok: boolean;
  ts: number;
  agent: {
    pid: number;
    seq: number;
    turns: number;
    uptime_sec: number;
    waiting: boolean;
    exited: boolean;
    exit_status: number | null;
  };
  tokens: {
    total: number;
    per_second: number;
    window_sec: number;
  };
  memory: {
    total_mb: number | null;
    used_mb: number | null;
    used_pct: number | null;
    agent_rss_mb: number | null;
  };
  runtime?: {
    ctx?: number;
    backend?: string;
    prefill_chunk?: number;
  };
};

export type Ds4ContractStatus = {
  ok: boolean;
  ds4_root: string;
  ds4_head: string | null;
  contract_status: "match" | "mismatch" | "unknown";
  bridge_http: { endpoints: Array<{ method: string; path: string }> };
  contract?: {
    ds4_revision?: string;
    server_http?: { endpoints?: Array<{ method: string; path: string }> };
    agent_slash_commands?: Array<{ name: string; takes_args: boolean }>;
    agent_tools?: Array<{ name: string; args: string[] }>;
    browser_cdp_methods?: string[];
    env_vars?: Array<{ name: string; source: string }>;
  };
};

export type RuntimeOwnership = "owned" | "attached";

export type RuntimeProfile = {
  schema_version: number;
  id: string;
  kind: string;
  ownership: RuntimeOwnership;
  platform: string;
  ds4_root: string;
  ds4_head: string | null;
  contract_revision: string | null;
  contract_status: "match" | "mismatch" | "unknown";
  command: string[];
  command_display: string;
  command_cwd: string;
  chdir: string;
  model_path: string | null;
  backend: string;
  ctx: number;
  prefill_chunk: number | null;
  mtp_path: string | null;
  bridge: {
    host: string;
    port: number;
  };
  process: {
    pid: number;
    started_at: number;
    uptime_sec: number;
    waiting: boolean;
    exited: boolean;
    exit_status: number | null;
  };
  env: Record<string, string>;
};

export type RuntimeProfileResponse = {
  ok: boolean;
  profile: RuntimeProfile;
};

export type ControlAction = "save" | "list" | "history" | "interrupt" | "restart";

const STORE = "ds4_cockpit_v1_";

export function readToken() {
  const params = new URLSearchParams(window.location.search);
  const pathToken = window.location.pathname.startsWith("/t/")
    ? decodeURIComponent(window.location.pathname.slice(3))
    : "";
  const token = params.get("token") || pathToken || localStorage.getItem(`${STORE}token`) || "";
  if (token) localStorage.setItem(`${STORE}token`, token);
  return token;
}

function authParams(token: string, extra?: Record<string, string>) {
  const params = new URLSearchParams(extra);
  if (token) params.set("token", token);
  return params.toString();
}

function bridgeUrl(path: string, token: string, extra?: Record<string, string>) {
  const query = authParams(token, extra);
  return `${path}${query ? `?${query}` : ""}`;
}

async function responseText(response: Response) {
  try {
    return await response.text();
  } catch {
    return `${response.status} ${response.statusText}`.trim();
  }
}

export async function getMetrics(token: string) {
  const response = await fetch(bridgeUrl("/metrics", token), { cache: "no-store" });
  if (!response.ok) throw new Error(await responseText(response));
  return (await response.json()) as Metrics;
}

export async function getContractStatus(token: string) {
  const response = await fetch(bridgeUrl("/contract", token), { cache: "no-store" });
  if (!response.ok) throw new Error(await responseText(response));
  return (await response.json()) as Ds4ContractStatus;
}

export async function getRuntimeProfile(token: string) {
  const response = await fetch(bridgeUrl("/profile", token), { cache: "no-store" });
  if (!response.ok) throw new Error(await responseText(response));
  return (await response.json()) as RuntimeProfileResponse;
}

export function openEventStream(token: string, lastSeq: number) {
  return new EventSource(bridgeUrl("/events", token, { last: String(lastSeq) }));
}

export async function sendInput(token: string, text: string) {
  const response = await fetch(bridgeUrl("/input", token), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ text }),
  });
  if (!response.ok) throw new Error(await responseText(response));
}

export async function controlAgent(token: string, action: ControlAction, extra: Record<string, unknown> = {}) {
  const response = await fetch(bridgeUrl("/control", token), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ action, ...extra }),
  });
  if (!response.ok) throw new Error(await responseText(response));
  return (await response.json()) as { ok: boolean; action: ControlAction; [key: string]: unknown };
}
