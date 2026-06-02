import {
  Mic,
  PlugZap,
  SendHorizontal,
} from "lucide-react";
import {
  Component,
  useCallback,
  useMemo,
  useRef,
  useState,
  useEffect,
  type ButtonHTMLAttributes,
  type ErrorInfo,
  type HTMLAttributes,
  type ReactNode,
} from "react";
import TextareaAutosize from "react-textarea-autosize";
import {
  controlAgent,
  getContractStatus,
  getMetrics,
  getRuntimeProfile,
  openEventStream,
  readToken,
  sendInput,
  type ControlAction,
  type Metrics,
  type RuntimeProfile,
} from "./ds4Bridge";

type MessageKind = "agent" | "user" | "system";

type Message = {
  id: string;
  kind: MessageKind;
  text: string;
  ts: number;
};

type SpeechRecognitionLike = {
  continuous: boolean;
  interimResults: boolean;
  lang: string;
  processLocally?: boolean;
  abort: () => void;
  start: () => void;
  stop: () => void;
  onend: (() => void) | null;
  onerror: (() => void) | null;
  onresult: ((event: {
    resultIndex: number;
    results: ArrayLike<ArrayLike<{ transcript: string }> & { isFinal: boolean }>;
  }) => void) | null;
};

type SpeechRecognitionConstructor = new () => SpeechRecognitionLike;

type Notice = {
  id: string;
  text: string;
};

const STORE = "ds4_cockpit_v1_";
const MAX_MESSAGE_CHARS = 120_000;

function classNames(...parts: Array<string | false | null | undefined>) {
  return parts.filter(Boolean).join(" ");
}

type ButtonProps = ButtonHTMLAttributes<HTMLButtonElement> & {
  size?: "xs" | "sm";
  variant?: "default" | "ghost" | "outline";
};

function Button({ className = "", size = "sm", type = "button", variant = "default", ...props }: ButtonProps) {
  return (
    <button
      className={classNames("ds4-button", `ds4-button-${variant}`, `ds4-button-${size}`, className)}
      type={type}
      {...props}
    />
  );
}

type BadgeProps = HTMLAttributes<HTMLSpanElement> & {
  size?: "sm";
  variant?: "outline";
};

function Badge({ className = "", size = "sm", variant = "outline", ...props }: BadgeProps) {
  return <span className={classNames("ds4-badge", `ds4-badge-${variant}`, `ds4-badge-${size}`, className)} {...props} />;
}

function Card({ className = "", ...props }: HTMLAttributes<HTMLDivElement>) {
  return <div className={classNames("ds4-card", className)} {...props} />;
}

function makeId(prefix = "m") {
  return `${prefix}${Date.now().toString(36)}${Math.random().toString(36).slice(2, 8)}`;
}

function compactNumber(value: number | null | undefined) {
  if (value == null || !Number.isFinite(value)) return "--";
  if (value >= 1_000_000) return `${(value / 1_000_000).toFixed(1)}m`;
  if (value >= 1_000) return `${(value / 1_000).toFixed(1)}k`;
  return String(Math.round(value));
}

function compactPath(value: string | null | undefined) {
  if (!value) return "--";
  const home = "/Users/mac/";
  if (value.startsWith(home)) return `~/${value.slice(home.length)}`;
  return value;
}

function shortSha(value: string | null | undefined) {
  return value ? value.slice(0, 8) : "--";
}

function stripAnsi(text: string) {
  return text
    .replace(/\x1b\][^\x07]*(\x07|\x1b\\)/g, "")
    .replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, "")
    .replace(/\x1b[ -/]*[@-~]/g, "")
    .replace(/\x07/g, "")
    .replace(/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/g, "")
    .replace(/\r\n/g, "\n")
    .replace(/\r/g, "");
}

function stripTerminalNoise(text: string) {
  return stripAnsi(text)
    .replace(/ctx\s+[\d.]+[kKmM]?\/\d+[kKmM]?\s*\|\s*generation\s+\d+\s+tokens\s+[\d.]+\s*t\/s/g, "")
    .replace(/ds4-agent>\s*/g, "")
    .replace(/<｜end▁of▁sentence｜>/g, "")
    .replace(/<\/think>/g, "")
    .replace(/<｜Assistant｜>/g, "")
    .replace(/<｜User｜>/g, "");
}

function displayAgentText(raw: string) {
  return stripTerminalNoise(raw)
    .replace(/\+DWARFSTAR_WAITING/g, "")
    .split("\n")
    .filter((line) => {
      const trimmed = line.trim();
      if (!trimmed) return true;
      return !/^(ds4:|ds4-agent:|Usage:|done$)/.test(trimmed);
    })
    .join("\n")
    .replace(/\n{3,}/g, "\n\n");
}

function friendlyError(action: string, err: unknown) {
  const raw = String(err instanceof Error ? err.message : err);
  if (/<!doctype|<html|no tunnel here|bad gateway|502|failed to fetch|networkerror/i.test(raw)) {
    return `${action} failed: connection dropped; the client is still alive and will reconnect.`;
  }
  return `${action} failed: ${raw.replace(/<[^>]*>/g, " ").replace(/\s+/g, " ").trim().slice(0, 180)}`;
}

function NoticeToast({ notice, onDismiss }: { notice: Notice | null; onDismiss: () => void }) {
  if (!notice) return null;
  return (
    <div aria-live="polite" className="ds4-error-toast" role="status">
      <span>{notice.text}</span>
      <button aria-label="Dismiss" onClick={onDismiss} type="button">
        OK
      </button>
    </div>
  );
}

class AppErrorBoundary extends Component<
  { children: ReactNode; onError: (message: string) => void },
  { failed: boolean }
> {
  state = { failed: false };

  static getDerivedStateFromError() {
    return { failed: true };
  }

  componentDidCatch(error: unknown, _info: ErrorInfo) {
    this.props.onError(friendlyError("client", error));
  }

  render() {
    if (this.state.failed) {
      return (
        <div className="ds4-shell ds4-fallback text-foreground">
          <div className="ds4-fallback-panel">
            <div className="font-heading text-sm uppercase tracking-wider">Runtime</div>
            <p>Client UI recovered from an exception.</p>
            <button onClick={() => window.location.reload()} type="button">
              Reload
            </button>
          </div>
        </div>
      );
    }
    return this.props.children;
  }
}

function loadMessages(sessionId: string): Message[] {
  try {
    return JSON.parse(localStorage.getItem(`${STORE}messages_${sessionId}`) || "[]") as Message[];
  } catch {
    return [];
  }
}

function saveMessages(sessionId: string, messages: Message[]) {
  const compact = messages.slice(-120).map((message) => ({
    ...message,
    text:
      message.text.length > MAX_MESSAGE_CHARS
        ? `${message.text.slice(0, MAX_MESSAGE_CHARS)}\n\n[output trimmed locally]`
        : message.text,
  }));
  localStorage.setItem(`${STORE}messages_${sessionId}`, JSON.stringify(compact));
}

function initialSessionId() {
  const existing = localStorage.getItem(`${STORE}session`);
  if (existing) return existing;
  const next = makeId("s");
  localStorage.setItem(`${STORE}session`, next);
  return next;
}

function useMetrics(token: string) {
  const [metrics, setMetrics] = useState<Metrics | null>(null);
  const [status, setStatus] = useState("connecting");
  const rates = useRef<number[]>([]);

  useEffect(() => {
    let alive = true;
    let timer = 0;

    async function poll() {
      try {
        const payload = await getMetrics(token);
        if (!alive) return;
        rates.current.push(payload.tokens.per_second);
        rates.current = rates.current.slice(-60);
        setMetrics(payload);
        setStatus(payload.agent.exited ? "agent exited" : payload.agent.waiting ? "idle" : "running");
      } catch {
        if (alive) setStatus(navigator.onLine ? "metrics lost" : "offline");
      } finally {
        if (alive) timer = window.setTimeout(poll, 1200);
      }
    }

    poll();
    return () => {
      alive = false;
      window.clearTimeout(timer);
    };
  }, [token]);

  const rateMax = Math.max(0, ...rates.current);
  const rateAvg = rates.current.length
    ? rates.current.reduce((sum, value) => sum + value, 0) / rates.current.length
    : 0;

  return { metrics, rates: rates.current, status, rateAvg, rateMax, setStatus };
}

function useRuntimeProfile(token: string, _onNotice: (text: string) => void) {
  const [profile, setProfile] = useState<RuntimeProfile | null>(null);

  const refreshProfile = useCallback(async () => {
    const payload = await getRuntimeProfile(token);
    setProfile(payload.profile);
    return payload.profile;
  }, [token]);

  useEffect(() => {
    let alive = true;
    let timer = 0;

    async function poll() {
      try {
        const payload = await getRuntimeProfile(token);
        if (alive) setProfile(payload.profile);
      } catch (err) {
        // Profile refresh is telemetry, not the primary agent path. Keep it
        // best-effort so mobile network churn cannot cover the demo surface.
        void err;
      } finally {
        if (alive) timer = window.setTimeout(poll, 5000);
      }
    }

    poll();
    return () => {
      alive = false;
      window.clearTimeout(timer);
    };
  }, [token]);

  return { profile, refreshProfile };
}

function CodeBlock({ code, language }: { code: string; language: string }) {
  return (
    <figure className="my-2 overflow-hidden rounded-lg border bg-code text-code-foreground">
      <figcaption className="border-b px-3 py-1 font-mono text-muted-foreground text-xs">
        {language || "text"}
      </figcaption>
      <pre className="overflow-auto p-3 text-sm">
        <code>{code}</code>
      </pre>
    </figure>
  );
}

function MessageContent({ text }: { text: string }) {
  const parts = useMemo(() => {
    const next: Array<{ type: "text"; value: string } | { type: "code"; value: string; language: string }> = [];
    const fence = /```([a-zA-Z0-9_-]*)\n([\s\S]*?)```/g;
    let last = 0;
    for (const match of text.matchAll(fence)) {
      if (match.index > last) next.push({ type: "text", value: text.slice(last, match.index) });
      next.push({ type: "code", language: match[1] || "text", value: match[2] || "" });
      last = match.index + match[0].length;
    }
    if (last < text.length) next.push({ type: "text", value: text.slice(last) });
    return next;
  }, [text]);

  return (
    <>
      {parts.map((part, index) =>
        part.type === "code" ? (
          <CodeBlock code={part.value} language={part.language} key={`${index}-code`} />
        ) : (
          <span className="whitespace-pre-wrap" key={`${index}-text`}>
            {part.value}
          </span>
        ),
      )}
    </>
  );
}

function MessageBubble({ message }: { message: Message }) {
  const isUser = message.kind === "user";
  const isSystem = message.kind === "system";

  return (
    <div className={`flex ${isUser ? "justify-end" : "justify-start"}`}>
      <div
        className={`ds4-message max-w-[min(860px,86vw)] rounded-xl border px-3 py-2 text-sm shadow-sm ${
          isUser ? "ds4-message-user" : isSystem ? "ds4-message-system" : "ds4-message-agent"
        }`}
      >
        <MessageContent text={message.text} />
      </div>
    </div>
  );
}

function MetricChip({ className = "", label, value }: { className?: string; label: string; value: string }) {
  return (
    <Badge
      className={`ds4-metric-chip h-5 gap-1 rounded-md px-1.5 font-mono text-[0.68rem] leading-none normal-case tracking-normal ${className}`}
      size="sm"
      variant="outline"
    >
      <span className="ds4-metric-label">{label}</span>
      <span>{value}</span>
    </Badge>
  );
}

function MetricSparkline({ values }: { values: number[] }) {
  const points = useMemo(() => {
    const sample = values.length ? values.slice(-36) : [0, 0];
    const maxValue = Math.max(0, ...sample);
    const max = Math.max(1, maxValue);
    const last = Math.max(1, sample.length - 1);
    return sample
      .map((value, index) => {
        const x = (index / last) * 100;
        const y = maxValue <= 0 ? 5 : 9 - (Math.max(0, value) / max) * 7;
        return `${x.toFixed(2)},${y.toFixed(2)}`;
      })
      .join(" ");
  }, [values]);

  return (
    <svg aria-hidden="true" className="ds4-navbar-sparkline" preserveAspectRatio="none" viewBox="0 0 100 10">
      <polyline className="ds4-navbar-sparkline-glow" fill="none" points={points} />
      <polyline className="ds4-navbar-sparkline-line" fill="none" points={points} />
    </svg>
  );
}

function ProfilePill({ label, value, tone = "default" }: { label: string; value: string; tone?: "default" | "good" | "warn" }) {
  return (
    <span className={`ds4-profile-pill ds4-profile-pill-${tone}`} title={`${label}: ${value}`}>
      <span>{label}</span>
      <strong>{value}</strong>
    </span>
  );
}

function AppShell({ onNotice }: { onNotice: (text: string) => void }) {
  const [token] = useState(readToken);
  const [sessionId, setSessionId] = useState(initialSessionId);
  const [messages, setMessages] = useState<Message[]>(() => loadMessages(initialSessionId()));
  const [input, setInput] = useState("");
  const [listening, setListening] = useState(false);
  const [streamState, setStreamState] = useState("connecting");
  const [theme, setTheme] = useState(() => localStorage.getItem(`${STORE}theme`) || "dark");
  const chatRef = useRef<HTMLDivElement | null>(null);
  const holdTimer = useRef(0);
  const recognitionRef = useRef<SpeechRecognitionLike | null>(null);
  const shouldStickToBottom = useRef(true);
  const forceLiveScroll = useRef(false);
  const lastSeq = useRef(Number(localStorage.getItem(`${STORE}seq`) || "0"));
  const pendingEcho = useRef("");
  const currentAgentId = useRef<string | null>(null);
  const agentBuffer = useRef("");
  const flushTimer = useRef(0);
  const suppressDsml = useRef(false);
  const suppressToolResult = useRef(false);
  const suppressHumanWrite = useRef(false);
  const { metrics, rates, status, rateAvg, rateMax, setStatus } = useMetrics(token);
  const { profile, refreshProfile } = useRuntimeProfile(token, onNotice);

  useEffect(() => {
    let alive = true;
    getContractStatus(token)
      .then((status) => {
        if (!alive || status.contract_status !== "mismatch") return;
        onNotice(`DS4 contract drift: bridge head ${status.ds4_head ?? "unknown"} differs from generated contract.`);
      })
      .catch(() => {
        if (alive) onNotice("DS4 contract check is unavailable on this bridge.");
      });
    return () => {
      alive = false;
    };
  }, [onNotice, token]);

  useEffect(() => {
    function onWindowError(event: ErrorEvent) {
      onNotice(friendlyError("client", event.error || event.message));
    }

    function onUnhandledRejection(event: PromiseRejectionEvent) {
      onNotice(friendlyError("client", event.reason));
    }

    window.addEventListener("error", onWindowError);
    window.addEventListener("unhandledrejection", onUnhandledRejection);
    return () => {
      window.removeEventListener("error", onWindowError);
      window.removeEventListener("unhandledrejection", onUnhandledRejection);
    };
  }, [onNotice]);

  useEffect(() => {
    document.documentElement.classList.toggle("dark", theme === "dark");
    localStorage.setItem(`${STORE}theme`, theme);
  }, [theme]);

  useEffect(() => {
    const id = window.setTimeout(() => saveMessages(sessionId, messages), 200);
    return () => window.clearTimeout(id);
  }, [messages, sessionId]);

  useEffect(() => {
    if (!shouldStickToBottom.current && !forceLiveScroll.current) return;
    requestAnimationFrame(() => {
      chatRef.current?.scrollTo({ top: chatRef.current.scrollHeight });
    });
  }, [messages]);

  const appendMessage = useCallback((kind: MessageKind, text: string) => {
    const cleaned = stripAnsi(text).trimEnd();
    if (!cleaned) return;
    setMessages((prev) => [...prev.slice(-119), { id: makeId(), kind, text: cleaned, ts: Date.now() }]);
  }, []);

  const flushAgentBuffer = useCallback(() => {
    window.clearTimeout(flushTimer.current);
    flushTimer.current = 0;
    const text = agentBuffer.current;
    agentBuffer.current = "";
    if (!text) return;

    setMessages((prev) => {
      const next = [...prev];
      const currentId = currentAgentId.current;
      const index = currentId ? next.findIndex((message) => message.id === currentId) : -1;
      if (index >= 0) {
        const current = next[index];
        next[index] = {
          ...current,
          text: `${current.text}${text}`.slice(0, MAX_MESSAGE_CHARS),
        };
        return next.slice(-120);
      }
      const id = makeId();
      currentAgentId.current = id;
      next.push({ id, kind: "agent", text: text.slice(0, MAX_MESSAGE_CHARS), ts: Date.now() });
      return next.slice(-120);
    });
  }, []);

  const stripProtocolText = useCallback((raw: string) => {
    let text = stripAnsi(raw);
    let output = "";
    const toolStart = "<｜DSML｜tool_calls>";
    const toolEnd = "</｜DSML｜tool_calls>";
    const toolResultStart = "Tool: Tool result";
    const assistantStart = "<｜Assistant｜>";
    const humanWriteStart = /[🛠🔧]\ufe0f?\s*write\b|^write\s+path=/m;

    while (text) {
      if (suppressHumanWrite.current) {
        const waiting = text.indexOf("+DWARFSTAR_WAITING");
        if (waiting >= 0) {
          output += "\n[write finished]\n";
          text = text.slice(waiting + "+DWARFSTAR_WAITING".length);
          suppressHumanWrite.current = false;
          continue;
        }
        const done = text.search(/\bDone\b\.|\bWrote\s+\d+\s+bytes\b/);
        if (done < 0) return output;
        text = text.slice(done);
        suppressHumanWrite.current = false;
        continue;
      }

      if (suppressDsml.current) {
        const end = text.indexOf(toolEnd);
        if (end < 0) return output;
        text = text.slice(end + toolEnd.length);
        suppressDsml.current = false;
        output += "\n[ds4 tool call]\n";
        continue;
      }

      if (suppressToolResult.current) {
        const end = text.indexOf(assistantStart);
        if (end < 0) return output;
        const hidden = text.slice(0, end);
        const wrote = hidden.match(/Wrote\s+\d+\s+bytes\s+to\s+([^\n<]+)/);
        if (wrote) output += `\n[tool wrote ${wrote[1].trim()}]\n`;
        text = text.slice(end + assistantStart.length);
        suppressToolResult.current = false;
        continue;
      }

      const nextTool = text.indexOf(toolStart);
      const nextResult = text.indexOf(toolResultStart);
      const nextHumanWrite = text.search(humanWriteStart);
      const candidates = [nextTool, nextResult, nextHumanWrite].filter((index) => index >= 0);
      const next = candidates.length ? Math.min(...candidates) : -1;

      if (next < 0) {
        output += text;
        break;
      }

      output += text.slice(0, next);
      if (next === nextTool) {
        text = text.slice(next + toolStart.length);
        suppressDsml.current = true;
      } else if (next === nextHumanWrite) {
        output += "\n[tool: write file]\n";
        text = text.slice(next);
        suppressHumanWrite.current = true;
      } else {
        text = text.slice(next);
        suppressToolResult.current = true;
      }
    }

    return output;
  }, []);

  const queueAgentText = useCallback(
    (raw: string) => {
      const text = displayAgentText(stripProtocolText(raw));
      if (!text.trim()) return;
      if (pendingEcho.current && text.trim() === pendingEcho.current.trim()) {
        pendingEcho.current = "";
        return;
      }
      agentBuffer.current += text;
      forceLiveScroll.current = true;
      shouldStickToBottom.current = true;
      if (!flushTimer.current) {
        flushTimer.current = window.setTimeout(flushAgentBuffer, 80);
      }
    },
    [flushAgentBuffer, stripProtocolText],
  );

  useEffect(() => {
    const events = openEventStream(token, lastSeq.current);
    events.onopen = () => setStreamState("connected");
    events.onerror = () => setStreamState(navigator.onLine ? "reconnecting" : "offline");
    events.addEventListener("pty", (event) => {
      if (event.lastEventId) {
        lastSeq.current = Number(event.lastEventId);
        localStorage.setItem(`${STORE}seq`, String(lastSeq.current));
      }
      try {
        queueAgentText(JSON.parse(event.data).text || "");
      } catch {
        queueAgentText(event.data);
      }
    });
    events.addEventListener("exit", (event) => {
      appendMessage("system", `agent exited: ${event.data}`);
      setStreamState("agent exited");
      events.close();
    });
    return () => events.close();
  }, [appendMessage, queueAgentText, token]);

  async function sendRaw(text: string, label = "send") {
    await sendInput(token, text);
    if (label !== "interrupt") setStatus("running");
  }

  async function submit() {
    const text = input;
    if (!text.trim()) return;
    shouldStickToBottom.current = true;
    forceLiveScroll.current = true;
    flushAgentBuffer();
    currentAgentId.current = null;
    suppressDsml.current = false;
    suppressToolResult.current = false;
    suppressHumanWrite.current = false;
    pendingEcho.current = text.trimEnd();
    setInput("");
    appendMessage("user", text.trimEnd());
    try {
      await sendRaw(`${text.replace(/\r?\n/g, "\n")}\r`);
    } catch (err) {
      onNotice(friendlyError("send", err));
    }
  }

  async function command(text: string, label: string) {
    shouldStickToBottom.current = true;
    flushAgentBuffer();
    currentAgentId.current = null;
    suppressDsml.current = false;
    suppressToolResult.current = false;
    suppressHumanWrite.current = false;
    appendMessage("system", label);
    try {
      await sendRaw(`${text.replace(/\r?\n/g, "\n")}\r`, label);
    } catch (err) {
      onNotice(friendlyError(label, err));
    }
  }

  async function runControl(action: ControlAction, label: string, extra: Record<string, unknown> = {}) {
    shouldStickToBottom.current = true;
    flushAgentBuffer();
    currentAgentId.current = null;
    suppressDsml.current = false;
    suppressToolResult.current = false;
    suppressHumanWrite.current = false;
    appendMessage("system", label);
    try {
      await controlAgent(token, action, extra);
      await refreshProfile();
      if (action !== "interrupt") setStatus("running");
    } catch (err) {
      onNotice(friendlyError(label, err));
    }
  }

  async function restartOwnedRuntime() {
    if (!window.confirm("Restart ds4-agent from the owned runtime profile?")) return;
    await runControl("restart", "restart owned ds4-agent");
  }

  function newLocalSession() {
    const next = makeId("s");
    localStorage.setItem(`${STORE}session`, next);
    saveMessages(sessionId, messages);
    setSessionId(next);
    setMessages([]);
    currentAgentId.current = null;
  }

  function onChatScroll() {
    if (forceLiveScroll.current) {
      forceLiveScroll.current = false;
      return;
    }
    const scrollEl = chatRef.current;
    if (!scrollEl) return;
    shouldStickToBottom.current = scrollEl.scrollHeight - scrollEl.scrollTop - scrollEl.clientHeight < 96;
  }

  function toggleMic() {
    if (recognitionRef.current) {
      recognitionRef.current.stop();
      recognitionRef.current = null;
      setListening(false);
      return;
    }

    const speechWindow = window as Window & {
      SpeechRecognition?: SpeechRecognitionConstructor;
      webkitSpeechRecognition?: SpeechRecognitionConstructor;
    };
    const Recognition = speechWindow.SpeechRecognition || speechWindow.webkitSpeechRecognition;
    if (!Recognition) {
      onNotice("Speech input is not available in this browser.");
      return;
    }

    const recognition = new Recognition();
    const baseInput = input.trimEnd();
    recognition.continuous = false;
    recognition.interimResults = true;
    recognition.lang = navigator.language || "en-US";
    if ("processLocally" in recognition) recognition.processLocally = true;
    recognition.onresult = (event) => {
      let finalText = "";
      let interimText = "";
      for (let index = event.resultIndex; index < event.results.length; index += 1) {
        const result = event.results[index];
        const transcript = result[0]?.transcript || "";
        if (result.isFinal) finalText += transcript;
        else interimText += transcript;
      }
      const spoken = `${finalText}${interimText}`.trim();
      setInput([baseInput, spoken].filter(Boolean).join(baseInput && spoken ? " " : ""));
    };
    recognition.onerror = () => {
      recognitionRef.current = null;
      setListening(false);
      onNotice("Speech input stopped; the browser could not keep recognition active.");
    };
    recognition.onend = () => {
      recognitionRef.current = null;
      setListening(false);
    };
    recognitionRef.current = recognition;
    setListening(true);
    try {
      recognition.start();
    } catch (err) {
      recognitionRef.current = null;
      setListening(false);
      onNotice(friendlyError("speech input", err));
    }
  }

  const runtime = metrics?.runtime;
  const agentState = metrics?.agent.exited ? "exited" : metrics?.agent.waiting ? "idle" : "run";
  const currentRate = (metrics?.tokens.per_second ?? 0).toFixed(1);
  const profileState = profile?.process.exited ? "exited" : profile?.process.waiting ? "idle" : "run";

  return (
    <div className="ds4-shell grid grid-rows-[auto_auto_1fr_auto] text-foreground">
      <header className="ds4-header min-h-12 items-center gap-2 border-b px-3 py-2 backdrop-blur">
        <div className="flex min-w-0 items-center gap-2">
          <img alt="" className="size-7 rounded-md border" src="/asset/app-icon.jpg" />
          <div className="min-w-0">
            <div className="truncate font-heading font-semibold text-sm uppercase tracking-wider">Runtime</div>
            <div className="ds4-subtitle truncate text-xs">
              {profile ? `${profile.ownership} ${profile.kind}` : "owned DS4 runtime profile"}
            </div>
          </div>
        </div>
        <div className="ds4-top-metrics flex min-w-0 items-center justify-end gap-1 text-xs">
          <MetricChip label="state" value={streamState === "connected" ? agentState : streamState} />
          <MetricChip className="metric-rate" label="t/s" value={`${currentRate}/${rateAvg.toFixed(1)}`} />
          <MetricChip
            className="metric-secondary"
            label="mem"
            value={metrics?.memory.used_pct == null ? "--" : `${metrics.memory.used_pct.toFixed(0)}%`}
          />
          <MetricChip className="metric-secondary" label="max" value={rateMax.toFixed(1)} />
          <MetricChip className="metric-wide" label="tok" value={compactNumber(metrics?.tokens.total)} />
          <MetricChip className="metric-wide" label="ctx" value={runtime?.ctx ? compactNumber(runtime.ctx) : "100k"} />
          <Button className="ds4-theme-chip metric-wide h-6 px-2 text-[0.68rem]" onClick={() => setTheme(theme === "dark" ? "light" : "dark")} size="xs" variant="outline">
            {theme === "dark" ? "dark" : "light"}
          </Button>
          <MetricSparkline values={rates} />
        </div>
      </header>

      <section className="ds4-profile-bar border-b px-3 py-2 backdrop-blur">
        <div className="mx-auto flex max-w-5xl flex-wrap items-center gap-1.5">
          <ProfilePill label="mode" tone={profile?.ownership === "owned" ? "good" : "warn"} value={profile ? `${profile.ownership}/${profileState}` : "owned/--"} />
          <ProfilePill label="ds4" tone={profile?.contract_status === "match" ? "good" : "warn"} value={shortSha(profile?.ds4_head)} />
          <ProfilePill label="cwd" value={compactPath(profile?.command_cwd)} />
          <ProfilePill label="chdir" value={compactPath(profile?.chdir)} />
          <ProfilePill label="model" value={profile?.model_path ? compactPath(profile.model_path).split("/").pop() || compactPath(profile.model_path) : "--"} />
          <ProfilePill label="backend" value={profile?.backend || runtime?.backend || "--"} />
          <ProfilePill label="ctx" value={compactNumber(profile?.ctx || runtime?.ctx)} />
          <ProfilePill label="mtp" value={profile?.mtp_path ? "on" : "off"} />
          <div className="ds4-profile-actions ml-auto flex items-center gap-1">
            <Button className="h-6 px-2 text-[0.68rem]" onClick={() => void runControl("save", "ds4 session save")} size="xs" type="button" variant="outline">
              Save
            </Button>
            <Button className="h-6 px-2 text-[0.68rem]" onClick={() => void runControl("list", "list KV sessions")} size="xs" type="button" variant="outline">
              List KV
            </Button>
            <Button className="h-6 px-2 text-[0.68rem]" onClick={() => void runControl("history", "show recent DS4 history", { turns: 8 })} size="xs" type="button" variant="outline">
              History
            </Button>
            <Button className="h-6 px-2 text-[0.68rem]" onClick={() => void restartOwnedRuntime()} size="xs" type="button" variant="outline">
              Restart
            </Button>
          </div>
        </div>
      </section>

      <main className="min-h-0 overflow-hidden">
        <div className="ds4-chat-scroll h-full overflow-y-auto px-3 py-4" onScroll={onChatScroll} ref={chatRef}>
          <div className="ds4-chat-stack mx-auto flex min-h-full max-w-5xl flex-col gap-3">
            {messages.length === 0 ? (
              <Card className="mx-auto max-w-2xl p-4">
                <div className="mb-2 flex items-center gap-2 font-heading text-sm uppercase tracking-wider">
                  <PlugZap className="size-4" />
                  DS4-native wrapper
                </div>
                <p className="text-muted-foreground text-sm">
                  This client is intentionally narrow: SSE stream, PTY input, local session memory, KV commands,
                  throughput, and health. Generic chat-app features stay out until they serve DS4 directly.
                </p>
              </Card>
            ) : null}
            {messages.map((message) => (
              <MessageBubble key={message.id} message={message} />
            ))}
          </div>
        </div>
      </main>

      <footer className="border-t bg-background/95 px-2 pb-[calc(env(safe-area-inset-bottom)+0.5rem)] pt-2 backdrop-blur">
        <div className="mx-auto grid max-w-5xl gap-1.5">
          <div className="ds4-composer-chips flex items-center gap-1.5">
            <select
              aria-label="DeepSeek V4 Flash / DS4 local GGUF / Metal"
              className="ds4-model-chip"
              defaultValue="deepseek-v4-flash"
              title="DeepSeek V4 Flash / DS4 local GGUF / Metal"
            >
              <option value="deepseek-v4-flash">deepseek-v4-flash...</option>
            </select>
          </div>
          <form
            className="ds4-composer grid items-end gap-1.5 rounded-2xl border bg-card/95 px-2 py-2 shadow-sm"
            onSubmit={(event) => {
              event.preventDefault();
              void submit();
            }}
          >
            <TextareaAutosize
              className="min-w-0 max-h-[24dvh] min-h-8 w-full resize-none bg-transparent px-2 py-1.5 text-sm outline-none"
              maxRows={7}
              minRows={1}
              onChange={(event) => setInput(event.target.value)}
              onKeyDown={(event) => {
                if (event.key === "Enter" && (event.metaKey || event.ctrlKey)) {
                  event.preventDefault();
                  void submit();
                }
              }}
              placeholder="Message ds4-agent"
              value={input}
            />
            <Button
              aria-label={listening ? "Stop dictation" : "Start dictation"}
              className={`ds4-icon-button h-8 w-8 px-0 ${listening ? "ds4-mic-active" : ""}`}
              onClick={toggleMic}
              size="xs"
              type="button"
              variant="ghost"
            >
              <Mic className="size-4" />
            </Button>
            <Button
              aria-label="Send"
              className="h-9 w-9 rounded-full px-0"
              onPointerDown={() => {
                holdTimer.current = window.setTimeout(() => void command("\u0003", "interrupt"), 700);
              }}
              onPointerLeave={() => window.clearTimeout(holdTimer.current)}
              onPointerUp={() => window.clearTimeout(holdTimer.current)}
              size="sm"
              type="submit"
              variant="default"
            >
              <SendHorizontal className="size-4" />
            </Button>
          </form>
        </div>
      </footer>
    </div>
  );
}

export function App() {
  const [notice, setNotice] = useState<Notice | null>(null);
  const pushNotice = useCallback((text: string) => {
    setNotice({ id: makeId("n"), text });
  }, []);

  useEffect(() => {
    if (!notice) return;
    const timer = window.setTimeout(() => setNotice(null), 4500);
    return () => window.clearTimeout(timer);
  }, [notice]);

  return (
    <>
      <AppErrorBoundary onError={pushNotice}>
        <AppShell onNotice={pushNotice} />
      </AppErrorBoundary>
      <NoticeToast notice={notice} onDismiss={() => setNotice(null)} />
    </>
  );
}
